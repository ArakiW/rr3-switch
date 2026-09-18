#include "rr3/cxx_lifecycle.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>

namespace rr3 {
namespace {

struct DestructorEntry {
    GuestDestructor destructor{};
    void* argument{};
    void* dsoHandle{};
    bool active{};
};

constexpr size_t kMaximumGlobalDestructors = 256;
constexpr size_t kMaximumThreadDestructors = 64;
DestructorEntry gGlobalDestructors[kMaximumGlobalDestructors]{};
pthread_mutex_t gGlobalDestructorMutex = PTHREAD_MUTEX_INITIALIZER;

struct ThreadDestructorList {
    DestructorEntry entries[kMaximumThreadDestructors]{};
    size_t count{};
};

pthread_key_t gThreadDestructorKey{};
pthread_once_t gThreadDestructorKeyOnce = PTHREAD_ONCE_INIT;
int gThreadDestructorKeyResult = EAGAIN;

void runThreadDestructors(void* value) {
    auto* list = static_cast<ThreadDestructorList*>(value);
    if (list == nullptr) {
        return;
    }
    for (size_t index = list->count; index != 0; --index) {
        DestructorEntry& entry = list->entries[index - 1];
        if (entry.active && entry.destructor != nullptr) {
            entry.active = false;
            entry.destructor(entry.argument);
        }
    }
    std::free(list);
}

void createThreadDestructorKey() {
    gThreadDestructorKeyResult =
        pthread_key_create(&gThreadDestructorKey, runThreadDestructors);
}

int gTestSequence = 0;
std::atomic<int> gThreadTestCount{0};

void sequenceDestructor(void* value) {
    gTestSequence = gTestSequence * 10 + static_cast<int>(reinterpret_cast<uintptr_t>(value));
}

void threadTestDestructor(void* value) {
    if (reinterpret_cast<uintptr_t>(value) == 7) {
        gThreadTestCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void* lifecycleTestThread(void*) {
    const int result = guestCxaThreadAtexitImpl(
        threadTestDestructor, reinterpret_cast<void*>(7), reinterpret_cast<void*>(0x33));
    return reinterpret_cast<void*>(static_cast<intptr_t>(result));
}

} // namespace

int guestCxaAtexit(GuestDestructor destructor, void* argument, void* dsoHandle) {
    if (destructor == nullptr) {
        return -1;
    }
    pthread_mutex_lock(&gGlobalDestructorMutex);
    for (DestructorEntry& entry : gGlobalDestructors) {
        if (!entry.active) {
            entry = {destructor, argument, dsoHandle, true};
            pthread_mutex_unlock(&gGlobalDestructorMutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&gGlobalDestructorMutex);
    errno = ENOMEM;
    return -1;
}

void guestCxaFinalize(void* dsoHandle) {
    for (size_t index = kMaximumGlobalDestructors; index != 0; --index) {
        GuestDestructor destructor = nullptr;
        void* argument = nullptr;

        pthread_mutex_lock(&gGlobalDestructorMutex);
        DestructorEntry& entry = gGlobalDestructors[index - 1];
        if (entry.active && (dsoHandle == nullptr || entry.dsoHandle == dsoHandle)) {
            entry.active = false;
            destructor = entry.destructor;
            argument = entry.argument;
        }
        pthread_mutex_unlock(&gGlobalDestructorMutex);

        if (destructor != nullptr) {
            destructor(argument);
        }
    }
}

int guestCxaThreadAtexitImpl(GuestDestructor destructor, void* argument,
                            void* dsoHandle) {
    if (destructor == nullptr) {
        return -1;
    }
    const int onceResult = pthread_once(&gThreadDestructorKeyOnce, createThreadDestructorKey);
    if (onceResult != 0 || gThreadDestructorKeyResult != 0) {
        errno = onceResult != 0 ? onceResult : gThreadDestructorKeyResult;
        return -1;
    }

    auto* list = static_cast<ThreadDestructorList*>(
        pthread_getspecific(gThreadDestructorKey));
    if (list == nullptr) {
        list = static_cast<ThreadDestructorList*>(std::calloc(1, sizeof(*list)));
        if (list == nullptr) {
            return -1;
        }
        const int setResult = pthread_setspecific(gThreadDestructorKey, list);
        if (setResult != 0) {
            std::free(list);
            errno = setResult;
            return -1;
        }
    }
    if (list->count == kMaximumThreadDestructors) {
        errno = ENOMEM;
        return -1;
    }
    list->entries[list->count++] = {destructor, argument, dsoHandle, true};
    return 0;
}

uint64_t cxxLifecycleSelfTest() {
    uint64_t passed = 0;
    gTestSequence = 0;
    gThreadTestCount.store(0, std::memory_order_relaxed);
    void* dsoA = reinterpret_cast<void*>(0x11);
    void* dsoB = reinterpret_cast<void*>(0x22);

    if (guestCxaAtexit(sequenceDestructor, reinterpret_cast<void*>(1), dsoA) == 0 &&
        guestCxaAtexit(sequenceDestructor, reinterpret_cast<void*>(2), dsoA) == 0 &&
        guestCxaAtexit(sequenceDestructor, reinterpret_cast<void*>(3), dsoB) == 0) {
        passed |= 1ULL << 0;
    }

    guestCxaFinalize(dsoA);
    if (gTestSequence == 21) {
        passed |= 1ULL << 1;
    }
    guestCxaFinalize(dsoA);
    if (gTestSequence == 21) {
        passed |= 1ULL << 2;
    }
    guestCxaFinalize(nullptr);
    if (gTestSequence == 213) {
        passed |= 1ULL << 3;
    }

    pthread_t thread{};
    void* result = reinterpret_cast<void*>(-1);
    if (pthread_create(&thread, nullptr, lifecycleTestThread, nullptr) == 0) {
        passed |= 1ULL << 4;
        if (pthread_join(thread, &result) == 0 && result == nullptr &&
            gThreadTestCount.load(std::memory_order_relaxed) == 1) {
            passed |= 1ULL << 5;
        }
    }
    return passed;
}

} // namespace rr3
