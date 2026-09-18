#include "rr3/semaphore_bridge.hpp"
#include "rr3/guest_view_trace.hpp"

#include <switch.h>

#include <cerrno>
#include <cstring>

namespace rr3 {
namespace {

struct SemaphoreEntry {
    void* guest{};
    Semaphore native{};
    bool used{};
};

constexpr size_t kMaximumSemaphores = 96;
SemaphoreEntry gSemaphores[kMaximumSemaphores]{};
Mutex gSemaphoreRegistryMutex{};

SemaphoreEntry* findEntry(void* guest) {
    for (SemaphoreEntry& entry : gSemaphores) {
        if (entry.used && entry.guest == guest) {
            return &entry;
        }
    }
    return nullptr;
}

SemaphoreEntry* requireEntry(void* guest) {
    mutexLock(&gSemaphoreRegistryMutex);
    SemaphoreEntry* entry = findEntry(guest);
    mutexUnlock(&gSemaphoreRegistryMutex);
    if (entry == nullptr) {
        errno = EINVAL;
    }
    return entry;
}

bool deadlineReached(const struct timespec& deadline) {
    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return true;
    }
    return now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

} // namespace

int guestSemInit(void* guestSemaphore, int processShared, unsigned int value) {
    if (guestSemaphore == nullptr || processShared != 0) {
        errno = processShared != 0 ? ENOSYS : EINVAL;
        return -1;
    }
    mutexLock(&gSemaphoreRegistryMutex);
    if (findEntry(guestSemaphore) != nullptr) {
        mutexUnlock(&gSemaphoreRegistryMutex);
        errno = EBUSY;
        return -1;
    }
    for (SemaphoreEntry& entry : gSemaphores) {
        if (!entry.used) {
            entry = {};
            entry.guest = guestSemaphore;
            entry.used = true;
            semaphoreInit(&entry.native, value);
            mutexUnlock(&gSemaphoreRegistryMutex);
            return 0;
        }
    }
    mutexUnlock(&gSemaphoreRegistryMutex);
    errno = ENOSPC;
    return -1;
}

int guestSemDestroy(void* guestSemaphore) {
    mutexLock(&gSemaphoreRegistryMutex);
    SemaphoreEntry* entry = findEntry(guestSemaphore);
    if (entry == nullptr) {
        mutexUnlock(&gSemaphoreRegistryMutex);
        errno = EINVAL;
        return -1;
    }
    entry->used = false;
    entry->guest = nullptr;
    mutexUnlock(&gSemaphoreRegistryMutex);
    return 0;
}

int guestSemPost(void* guestSemaphore) {
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    semaphoreSignal(&entry->native);
    return 0;
}

int guestSemWait(void* guestSemaphore) {
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    // Bounded wait slices: an unbounded semaphoreWait has no breadcrumb when
    // an FMOD mixer thread blocks on a missing Android audio callback. Each
    // 500 ms slice reports via the existing sync-blocked primitive (same
    // pattern as guestPthreadCondWait). The wait itself is still indefinite
    // so legitimate long waits keep their blocking semantics.
    constexpr long kSyncBlockedReportNanos = 500000000LL;
    while (!semaphoreTryWait(&entry->native)) {
        guestViewTraceSyncBlocked("sem-wait", 54);
        svcSleepThread(kSyncBlockedReportNanos);
    }
    return 0;
}

int guestSemTryWait(void* guestSemaphore) {
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    if (!semaphoreTryWait(&entry->native)) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

int guestSemTimedWait(void* guestSemaphore, const struct timespec* absoluteTimeout) {
    if (absoluteTimeout == nullptr || absoluteTimeout->tv_nsec < 0 ||
        absoluteTimeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    while (!semaphoreTryWait(&entry->native)) {
        if (deadlineReached(*absoluteTimeout)) {
            errno = ETIMEDOUT;
            return -1;
        }
        svcSleepThread(1000000);
    }
    return 0;
}

int guestSemGetValue(void* guestSemaphore, int* value) {
    if (value == nullptr) {
        errno = EINVAL;
        return -1;
    }
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    mutexLock(&entry->native.mutex);
    *value = entry->native.count > static_cast<uint64_t>(INT32_MAX)
        ? INT32_MAX
        : static_cast<int>(entry->native.count);
    mutexUnlock(&entry->native.mutex);
    return 0;
}

void* guestSemOpen(const char*, int, ...) {
    errno = ENOSYS;
    return reinterpret_cast<void*>(-1);
}

int guestSemClose(void*) {
    errno = ENOSYS;
    return -1;
}

int guestSemUnlink(const char*) {
    errno = ENOSYS;
    return -1;
}

uint64_t semaphoreBridgeSelfTest() {
    uint64_t passed = 0;
    alignas(8) unsigned char guest[8]{};
    if (guestSemInit(guest, 0, 1) == 0) {
        passed |= 1ULL << 0;
    }
    if (guestSemTryWait(guest) == 0) {
        passed |= 1ULL << 1;
    }
    errno = 0;
    if (guestSemTryWait(guest) == -1 && errno == EAGAIN) {
        passed |= 1ULL << 2;
    }
    if (guestSemPost(guest) == 0 && guestSemWait(guest) == 0) {
        passed |= 1ULL << 3;
    }
    int value = -1;
    if (guestSemGetValue(guest, &value) == 0 && value == 0) {
        passed |= 1ULL << 4;
    }
    struct timespec past{};
    errno = 0;
    if (guestSemTimedWait(guest, &past) == -1 && errno == ETIMEDOUT &&
        guestSemDestroy(guest) == 0) {
        passed |= 1ULL << 5;
    }
    return passed;
}

} // namespace rr3
