#include "rr3/pthread_bridge.hpp"
#include "rr3/tls_probe.hpp"
#include "rr3/guest_view_trace.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <switch.h>

namespace rr3 {
namespace {

constexpr size_t kMutexCapacity = 256;
constexpr size_t kCondCapacity = 128;
constexpr size_t kRwlockCapacity = 128;
constexpr size_t kOnceCapacity = 64;
constexpr size_t kAttrCapacity = 64;
constexpr size_t kThreadCapacity = 96;
constexpr int kGuestJoinable = 0;
constexpr int kGuestDetached = 1;
constexpr size_t kGuestMinimumStackSize = 0x4000;

enum class ThreadLifecycle {
    Running,
    Detached,
    Joined,
};

struct ThreadSlot {
    pthread_t host{};
    GuestThread guest{};
    bool used{};
    bool joinable{};
    bool recycled{};
    bool joinInProgress{};
    ThreadLifecycle lifecycle{};
};

template <typename T>
struct ObjectSlot {
    const void* guest{};
    bool used{};
    T host{};
};

struct MutexAttrSlot {
    const void* guest{};
    bool used{};
    int type{};
};

struct ThreadAttrSlot {
    const void* guest{};
    bool used{};
    int detachState{kGuestJoinable};
    size_t stackSize{};
};

Mutex gRegistryLock{};
ObjectSlot<pthread_mutex_t> gMutexes[kMutexCapacity]{};
ObjectSlot<pthread_cond_t> gConditions[kCondCapacity]{};
ObjectSlot<pthread_rwlock_t> gRwlocks[kRwlockCapacity]{};
ObjectSlot<pthread_once_t> gOnceControls[kOnceCapacity]{};
MutexAttrSlot gMutexAttributes[kAttrCapacity]{};
ThreadAttrSlot gThreadAttributes[kAttrCapacity]{};
ThreadSlot gThreads[kThreadCapacity]{};
CondVar gThreadsChanged{};
bool gReaperStarted{};
GuestThread gReaperGuest{};
// Sticky once set: the process is shutting down guest images, so new guest
// threads must not be created and the reaper must not be restarted.
bool gQuiesceRequested{};

// Guest synchronization can block indefinitely inside Render. Bounded waits
// keep the real blocking semantics but emit a durable breadcrumb when a
// primitive stays blocked, so the next device run pins the hang instead of
// stalling silently on a black screen.
constexpr long kSyncBlockedReportNanos = 500000000LL;


template <typename Slot, size_t N>
Slot* findSlot(Slot (&slots)[N], const void* guest) {
    for (Slot& slot : slots) {
        if (slot.used && slot.guest == guest) {
            return &slot;
        }
    }
    return nullptr;
}

template <typename Slot, size_t N>
Slot* freeSlot(Slot (&slots)[N]) {
    for (Slot& slot : slots) {
        if (!slot.used) {
            return &slot;
        }
    }
    return nullptr;
}

ThreadSlot* findThreadSlotLocked(GuestThread thread) {
    for (ThreadSlot& slot : gThreads) {
        if (slot.used && slot.guest == thread) {
            return &slot;
        }
    }
    return nullptr;
}

ThreadSlot* createThreadSlotLocked(pthread_t host) {
    for (ThreadSlot& slot : gThreads) {
        if (slot.used && slot.host == host) {
            return nullptr;
        }
    }
    ThreadSlot* slot = freeSlot(gThreads);
    if (slot == nullptr) {
        return nullptr;
    }
    slot->host = host;
    slot->guest = reinterpret_cast<uintptr_t>(host);
    slot->used = true;
    slot->joinable = true;
    slot->recycled = false;
    slot->joinInProgress = false;
    slot->lifecycle = ThreadLifecycle::Running;
    return slot;
}

// Registers a thread whose slot was already reserved before pthread_create.
// Used for the reaper, which must not take gRegistryLock inside create while
// the caller holds it.
ThreadSlot* adoptReservedThreadSlotLocked(pthread_t host) {
    for (ThreadSlot& slot : gThreads) {
        if (slot.used && slot.host == host) {
            return &slot;
        }
    }
    for (ThreadSlot& slot : gThreads) {
        if (slot.used && slot.guest == 0) {
            slot.host = host;
            slot.guest = reinterpret_cast<uintptr_t>(host);
            return &slot;
        }
    }
    return nullptr;
}

int mutexTypeForAttributeLocked(const void* attribute) {
    if (attribute == nullptr) {
        return PTHREAD_MUTEX_NORMAL;
    }
    const MutexAttrSlot* slot = findSlot(gMutexAttributes, attribute);
    return slot != nullptr ? slot->type : PTHREAD_MUTEX_NORMAL;
}

size_t stackSizeForAttributeLocked(const void* attribute) {
    if (attribute == nullptr) {
        return 0;
    }
    const ThreadAttrSlot* slot = findSlot(gThreadAttributes, attribute);
    return slot != nullptr ? slot->stackSize : 0;
}

int createMutexLocked(void* guest, int type, pthread_mutex_t** output) {
    if (guest == nullptr) {
        return EINVAL;
    }
    ObjectSlot<pthread_mutex_t>* existing = findSlot(gMutexes, guest);
    if (existing != nullptr) {
        *output = &existing->host;
        return 0;
    }
    ObjectSlot<pthread_mutex_t>* slot = freeSlot(gMutexes);
    if (slot == nullptr) {
        return ENOMEM;
    }
    pthread_mutexattr_t attribute;
    int result = ::pthread_mutexattr_init(&attribute);
    const bool attributeInitialized = result == 0;
    if (result == 0) {
        result = ::pthread_mutexattr_settype(&attribute, type);
    }
    if (result == 0) {
        result = ::pthread_mutex_init(&slot->host, &attribute);
    }
    if (attributeInitialized) {
        ::pthread_mutexattr_destroy(&attribute);
    }
    if (result != 0) {
        return result;
    }
    slot->guest = guest;
    slot->used = true;
    *output = &slot->host;
    return 0;
}

int getMutex(void* guest, pthread_mutex_t** output) {
    mutexLock(&gRegistryLock);
    const int result = createMutexLocked(guest, PTHREAD_MUTEX_NORMAL, output);
    mutexUnlock(&gRegistryLock);
    return result;
}

int getCondition(void* guest, pthread_cond_t** output) {
    if (guest == nullptr) {
        return EINVAL;
    }
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_cond_t>* slot = findSlot(gConditions, guest);
    if (slot == nullptr) {
        slot = freeSlot(gConditions);
        if (slot == nullptr) {
            mutexUnlock(&gRegistryLock);
            return ENOMEM;
        }
        const int result = ::pthread_cond_init(&slot->host, nullptr);
        if (result != 0) {
            mutexUnlock(&gRegistryLock);
            return result;
        }
        slot->guest = guest;
        slot->used = true;
    }
    *output = &slot->host;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int getRwlock(void* guest, pthread_rwlock_t** output) {
    if (guest == nullptr) {
        return EINVAL;
    }
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_rwlock_t>* slot = findSlot(gRwlocks, guest);
    if (slot == nullptr) {
        slot = freeSlot(gRwlocks);
        if (slot == nullptr) {
            mutexUnlock(&gRegistryLock);
            return ENOMEM;
        }
        const int result = ::pthread_rwlock_init(&slot->host, nullptr);
        if (result != 0) {
            mutexUnlock(&gRegistryLock);
            return result;
        }
        slot->guest = guest;
        slot->used = true;
    }
    *output = &slot->host;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int gOnceCounter = 0;
void onceSelfTestCallback() {
    ++gOnceCounter;
}

struct GuestThreadStartContext {
    GuestStartRoutine start;
    void* argument;
};

void* guestThreadStart(void* opaque) {
    const GuestThreadStartContext context = *static_cast<GuestThreadStartContext*>(opaque);
    std::free(opaque);
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        std::abort();
    }
    void* result = context.start(context.argument);
    // libnx runs TSD destructors inside pthread_exit, so keep this TLS frame alive until thread termination.
    guestPthreadExit(result);
}

struct ThreadTlsSelfTest {
    GuestKey key{};
    uintptr_t parentTls{};
    uintptr_t workerTls{};
    uint64_t canary{};
    bool explicitExit{};
    bool entryValid{};
    bool destructorValid{};
};

uintptr_t selfTestTlsBase() {
    uintptr_t value = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

void threadTlsSelfTestDestructor(void* argument) {
    auto* test = static_cast<ThreadTlsSelfTest*>(argument);
    const uintptr_t tls = selfTestTlsBase();
    test->destructorValid = tls != 0 && tls == test->workerTls &&
        *reinterpret_cast<volatile const uint64_t*>(tls + 0x28) == test->canary;
}

void* threadSelfTestStart(void* argument) {
    auto* test = static_cast<ThreadTlsSelfTest*>(argument);
    test->workerTls = selfTestTlsBase();
    if (test->workerTls != 0 && test->workerTls != test->parentTls) {
        test->canary = *reinterpret_cast<volatile const uint64_t*>(test->workerTls + 0x28);
        test->entryValid = test->canary != 0 &&
            guestPthreadSetSpecific(test->key, argument) == 0;
    }
    if (test->explicitExit) {
        guestPthreadExit(argument);
    }
    return argument;
}

} // namespace

int guestPthreadAttrInit(void* attribute) {
    if (attribute == nullptr) return EINVAL;
    mutexLock(&gRegistryLock);
    if (findSlot(gThreadAttributes, attribute) != nullptr) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    ThreadAttrSlot* slot = freeSlot(gThreadAttributes);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return ENOMEM;
    }
    slot->guest = attribute;
    slot->detachState = kGuestJoinable;
    slot->stackSize = 0;
    slot->used = true;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadAttrDestroy(void* attribute) {
    mutexLock(&gRegistryLock);
    ThreadAttrSlot* slot = findSlot(gThreadAttributes, attribute);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    *slot = {};
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadAttrSetDetachState(void* attribute, int state) {
    if (state != kGuestJoinable && state != kGuestDetached) return EINVAL;
    mutexLock(&gRegistryLock);
    ThreadAttrSlot* slot = findSlot(gThreadAttributes, attribute);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    slot->detachState = state;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadAttrSetStackSize(void* attribute, size_t stackSize) {
    if (stackSize < kGuestMinimumStackSize) return EINVAL;
    mutexLock(&gRegistryLock);
    ThreadAttrSlot* slot = findSlot(gThreadAttributes, attribute);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    slot->stackSize = stackSize;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadCreate(GuestThread* thread, const void* attribute,
                       GuestStartRoutine start, void* argument) {
    if (thread == nullptr || start == nullptr) return EINVAL;
    mutexLock(&gRegistryLock);
    if (gQuiesceRequested) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    const size_t guestStackSize = stackSizeForAttributeLocked(attribute);
    mutexUnlock(&gRegistryLock);

    pthread_attr_t hostAttribute;
    int result = ::pthread_attr_init(&hostAttribute);
    if (result != 0) return result;
    result = ::pthread_attr_setdetachstate(&hostAttribute, PTHREAD_CREATE_JOINABLE);
    if (result == 0 && guestStackSize != 0) {
        result = ::pthread_attr_setstacksize(&hostAttribute, guestStackSize);
    }
    pthread_t hostThread{};
    if (result == 0) {
        auto* context = static_cast<GuestThreadStartContext*>(
            std::malloc(sizeof(GuestThreadStartContext)));
        if (context == nullptr) {
            result = ENOMEM;
        }
        else {
            *context = {start, argument};
            result = ::pthread_create(&hostThread, &hostAttribute, guestThreadStart, context);
            if (result != 0) {
                std::free(context);
            }
        }
    }
    ::pthread_attr_destroy(&hostAttribute);
    if (result == 0) {
        static_assert(sizeof(hostThread) <= sizeof(*thread));
        *thread = reinterpret_cast<uintptr_t>(hostThread);
        mutexLock(&gRegistryLock);
        if (adoptReservedThreadSlotLocked(hostThread) == nullptr &&
            createThreadSlotLocked(hostThread) == nullptr) {
            mutexUnlock(&gRegistryLock);
            return ENOMEM;
        }
        condvarWakeAll(&gThreadsChanged);
        mutexUnlock(&gRegistryLock);
    }
    return result;
}

void* reapDetachedThreadWorker(void*);

void recycleDetachedThreadSlotLocked(ThreadSlot& slot) {
    slot.used = false;
    slot.host = {};
    slot.guest = 0;
    slot.joinable = false;
    slot.recycled = false;
    slot.joinInProgress = false;
    slot.lifecycle = ThreadLifecycle::Running;
    condvarWakeAll(&gThreadsChanged);
}

void reapDetachedThreadSlotLocked(ThreadSlot& slot) {
    slot.joinInProgress = true;
    mutexUnlock(&gRegistryLock);
    void* ignored = nullptr;
    const int joinResult = ::pthread_join(slot.host, &ignored);
    mutexLock(&gRegistryLock);
    slot.joinInProgress = false;
    if (joinResult == 0 || joinResult == ESRCH) {
        recycleDetachedThreadSlotLocked(slot);
    }
    else if (joinResult == EINVAL) {
        recycleDetachedThreadSlotLocked(slot);
    }
    else {
        slot.recycled = true;
    }
}

void reapDetachedThreadsLocked() {
    for (ThreadSlot& slot : gThreads) {
        if (slot.used && slot.lifecycle == ThreadLifecycle::Detached &&
            !slot.joinInProgress) {
            reapDetachedThreadSlotLocked(slot);
        }
    }
}

int guestPthreadJoin(GuestThread thread, void** value) {
    mutexLock(&gRegistryLock);
    reapDetachedThreadsLocked();
    ThreadSlot* slot = findThreadSlotLocked(thread);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return ESRCH;
    }
    if (!slot->joinable || slot->lifecycle != ThreadLifecycle::Running) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    slot->joinInProgress = true;
    mutexUnlock(&gRegistryLock);
    const int result = ::pthread_join(slot->host, value);
    mutexLock(&gRegistryLock);
    slot->joinInProgress = false;
    if (result == 0) {
        slot->lifecycle = ThreadLifecycle::Joined;
        recycleDetachedThreadSlotLocked(*slot);
    }
    mutexUnlock(&gRegistryLock);
    return result;
}

void* reapDetachedThreadWorker(void*) {
    for (;;) {
        mutexLock(&gRegistryLock);
        while (true) {
            if (gQuiesceRequested) {
                mutexUnlock(&gRegistryLock);
                return nullptr;
            }
            bool pending = false;
            for (const ThreadSlot& slot : gThreads) {
                if (slot.used && slot.lifecycle == ThreadLifecycle::Detached &&
                    !slot.joinInProgress) {
                    pending = true;
                    break;
                }
            }
            if (pending) {
                break;
            }
            const Result waitResult = condvarWait(&gThreadsChanged, &gRegistryLock);
            if (R_FAILED(waitResult)) {
                mutexUnlock(&gRegistryLock);
                return nullptr;
            }
        }
        reapDetachedThreadsLocked();
        mutexUnlock(&gRegistryLock);
    }
}

int startDetachedReaper() {
    mutexLock(&gRegistryLock);
    if (gReaperStarted) {
        mutexUnlock(&gRegistryLock);
        return 0;
    }
    // Reserve the reaper slot before creating the thread: the child needs
    // gRegistryLock to register itself, so it must not be created while the
    // caller already holds the lock (that self-deadlocks).
    ThreadSlot* reaperSlot = freeSlot(gThreads);
    if (reaperSlot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return ENOMEM;
    }
    reaperSlot->used = true;
    reaperSlot->host = {};
    reaperSlot->guest = 0;
    reaperSlot->joinable = true;
    reaperSlot->recycled = false;
    reaperSlot->joinInProgress = false;
    reaperSlot->lifecycle = ThreadLifecycle::Running;
    gReaperStarted = true;
    mutexUnlock(&gRegistryLock);

    GuestThread reaper{};
    const int result = guestPthreadCreate(&reaper, nullptr, reapDetachedThreadWorker, nullptr);
    if (result != 0) {
        mutexLock(&gRegistryLock);
        gReaperStarted = false;
        *reaperSlot = {};
        mutexUnlock(&gRegistryLock);
        return result;
    }
    mutexLock(&gRegistryLock);
    reaperSlot->host = reinterpret_cast<pthread_t>(static_cast<uintptr_t>(reaper));
    reaperSlot->guest = reaper;
    gReaperGuest = reaper;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadDetach(GuestThread thread) {
    mutexLock(&gRegistryLock);
    if (gQuiesceRequested) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    reapDetachedThreadsLocked();
    ThreadSlot* slot = findThreadSlotLocked(thread);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return ESRCH;
    }
    if (!slot->joinable || slot->lifecycle != ThreadLifecycle::Running) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    slot->joinable = false;
    slot->lifecycle = ThreadLifecycle::Detached;
    slot->recycled = false;
    condvarWakeAll(&gThreadsChanged);
    const bool needReaper = !gReaperStarted;
    mutexUnlock(&gRegistryLock);
    if (needReaper) {
        return startDetachedReaper();
    }
    return 0;
}

unsigned int guestPthreadQuiesceAll() {
    unsigned int joined = 0;
    mutexLock(&gRegistryLock);
    // Arm the reaper exit flag BEFORE any join. The reaper blocks in
    // condvarWait with no timeout; joining it while it is still waiting
    // deadlocks the process at handoff-quiesce-enter (device rounds 7-8).
    gQuiesceRequested = true;
    condvarWakeAll(&gThreadsChanged);

    // Join only the reaper. Guest workers spawned during View Changed / FMOD
    // routinely outlive the probe and wait on conditions that will never be
    // signalled again; ::pthread_join on those hangs PLUS/Y at
    // handoff-quiesce-enter even after the reaper can exit. Leave them in
    // the registry and let the caller refuse image unload (or dirty-exit).
    GuestThread reaperGuest = gReaperGuest;
    if (reaperGuest != 0) {
        ThreadSlot* reaperSlot = findThreadSlotLocked(reaperGuest);
        if (reaperSlot != nullptr && reaperSlot->used && !reaperSlot->joinInProgress) {
            reaperSlot->joinInProgress = true;
            const pthread_t reaperHost = reaperSlot->host;
            mutexUnlock(&gRegistryLock);
            void* ignored = nullptr;
            ::pthread_join(reaperHost, &ignored);
            mutexLock(&gRegistryLock);
            reaperSlot->joinInProgress = false;
            recycleDetachedThreadSlotLocked(*reaperSlot);
            ++joined;
        }
        gReaperStarted = false;
        gReaperGuest = 0;
    }
    mutexUnlock(&gRegistryLock);
    return joined;
}

unsigned int guestPthreadLiveCount() {
    mutexLock(&gRegistryLock);
    unsigned int live = 0;
    for (const ThreadSlot& slot : gThreads) {
        if (slot.used) {
            ++live;
        }
    }
    mutexUnlock(&gRegistryLock);
    return live;
}

int guestPthreadEqual(GuestThread left, GuestThread right) {
    return ::pthread_equal(
        reinterpret_cast<pthread_t>(static_cast<uintptr_t>(left)),
        reinterpret_cast<pthread_t>(static_cast<uintptr_t>(right)));
}

GuestThread guestPthreadSelf() {
    return reinterpret_cast<uintptr_t>(::pthread_self());
}

[[noreturn]] void guestPthreadExit(void* value) {
    ::pthread_exit(value);
    std::abort();
}

int guestPthreadMutexAttrInit(void* attribute) {
    if (attribute == nullptr) return EINVAL;
    mutexLock(&gRegistryLock);
    if (findSlot(gMutexAttributes, attribute) != nullptr) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    MutexAttrSlot* slot = freeSlot(gMutexAttributes);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return ENOMEM;
    }
    slot->guest = attribute;
    slot->type = PTHREAD_MUTEX_NORMAL;
    slot->used = true;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadMutexAttrDestroy(void* attribute) {
    mutexLock(&gRegistryLock);
    MutexAttrSlot* slot = findSlot(gMutexAttributes, attribute);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    *slot = {};
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadMutexAttrSetType(void* attribute, int type) {
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) return EINVAL;
    mutexLock(&gRegistryLock);
    MutexAttrSlot* slot = findSlot(gMutexAttributes, attribute);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    slot->type = type;
    mutexUnlock(&gRegistryLock);
    return 0;
}

int guestPthreadMutexInit(void* mutex, const void* attribute) {
    if (mutex == nullptr) return EINVAL;
    mutexLock(&gRegistryLock);
    if (findSlot(gMutexes, mutex) != nullptr) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    const int type = mutexTypeForAttributeLocked(attribute);
    pthread_mutex_t* host = nullptr;
    const int result = createMutexLocked(mutex, type, &host);
    mutexUnlock(&gRegistryLock);
    return result;
}

int guestPthreadMutexDestroy(void* mutex) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_mutex_t>* slot = findSlot(gMutexes, mutex);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    const int result = ::pthread_mutex_destroy(&slot->host);
    if (result == 0) *slot = {};
    mutexUnlock(&gRegistryLock);
    return result;
}

int guestPthreadMutexLock(void* mutex) {
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    if (result != 0) return result;
    int lockResult = ::pthread_mutex_trylock(host);
    while (lockResult == EBUSY) {
        guestViewTraceSyncBlocked("mutex", 52);
        svcSleepThread(kSyncBlockedReportNanos);
        lockResult = ::pthread_mutex_trylock(host);
    }
    return lockResult;
}

int guestPthreadMutexTryLock(void* mutex) {
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    return result == 0 ? ::pthread_mutex_trylock(host) : result;
}

int guestPthreadMutexUnlock(void* mutex) {
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    return result == 0 ? ::pthread_mutex_unlock(host) : result;
}

int guestPthreadCondInit(void* condition, const void* attribute) {
    (void)attribute;
    pthread_cond_t* host = nullptr;
    return getCondition(condition, &host);
}

int guestPthreadCondDestroy(void* condition) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_cond_t>* slot = findSlot(gConditions, condition);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    const int result = ::pthread_cond_destroy(&slot->host);
    if (result == 0) *slot = {};
    mutexUnlock(&gRegistryLock);
    return result;
}

int guestPthreadCondWait(void* condition, void* mutex) {
    pthread_cond_t* hostCondition = nullptr;
    pthread_mutex_t* hostMutex = nullptr;
    int result = getCondition(condition, &hostCondition);
    if (result == 0) result = getMutex(mutex, &hostMutex);
    if (result != 0) return result;
    for (;;) {
        guestViewTraceSyncBlocked("cond-wait", 53);
        struct timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        const int waitResult = ::pthread_cond_timedwait(hostCondition, hostMutex, &deadline);
        if (waitResult == 0) return 0;
        if (waitResult != ETIMEDOUT) return waitResult;
    }
}

int guestPthreadCondTimedWait(void* condition, void* mutex,
                              const struct timespec* absoluteTime) {
    pthread_cond_t* hostCondition = nullptr;
    pthread_mutex_t* hostMutex = nullptr;
    int result = getCondition(condition, &hostCondition);
    if (result == 0) result = getMutex(mutex, &hostMutex);
    return result == 0
        ? ::pthread_cond_timedwait(hostCondition, hostMutex, absoluteTime) : result;
}

int guestPthreadCondSignal(void* condition) {
    pthread_cond_t* host = nullptr;
    const int result = getCondition(condition, &host);
    return result == 0 ? ::pthread_cond_signal(host) : result;
}

int guestPthreadCondBroadcast(void* condition) {
    pthread_cond_t* host = nullptr;
    const int result = getCondition(condition, &host);
    return result == 0 ? ::pthread_cond_broadcast(host) : result;
}

int guestPthreadRwlockInit(void* lock, const void* attribute) {
    (void)attribute;
    pthread_rwlock_t* host = nullptr;
    return getRwlock(lock, &host);
}

int guestPthreadRwlockDestroy(void* lock) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_rwlock_t>* slot = findSlot(gRwlocks, lock);
    if (slot == nullptr) {
        mutexUnlock(&gRegistryLock);
        return EINVAL;
    }
    const int result = ::pthread_rwlock_destroy(&slot->host);
    if (result == 0) *slot = {};
    mutexUnlock(&gRegistryLock);
    return result;
}

int guestPthreadRwlockReadLock(void* lock) {
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_rdlock(host) : result;
}

int guestPthreadRwlockWriteLock(void* lock) {
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_wrlock(host) : result;
}

int guestPthreadRwlockUnlock(void* lock) {
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_unlock(host) : result;
}

int guestPthreadOnce(void* onceControl, void (*initializer)()) {
    if (onceControl == nullptr || initializer == nullptr) return EINVAL;
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_once_t>* slot = findSlot(gOnceControls, onceControl);
    if (slot == nullptr) {
        slot = freeSlot(gOnceControls);
        if (slot == nullptr) {
            mutexUnlock(&gRegistryLock);
            return ENOMEM;
        }
        std::memset(&slot->host, 0, sizeof(slot->host));
        slot->guest = onceControl;
        slot->used = true;
    }
    pthread_once_t* host = &slot->host;
    mutexUnlock(&gRegistryLock);
    return ::pthread_once(host, initializer);
}

int guestPthreadKeyCreate(GuestKey* key, void (*destructor)(void*)) {
    if (key == nullptr) return EINVAL;
    pthread_key_t hostKey{};
    const int result = ::pthread_key_create(&hostKey, destructor);
    if (result == 0) *key = static_cast<GuestKey>(hostKey);
    return result;
}

int guestPthreadKeyDelete(GuestKey key) {
    return ::pthread_key_delete(static_cast<pthread_key_t>(key));
}

void* guestPthreadGetSpecific(GuestKey key) {
    return ::pthread_getspecific(static_cast<pthread_key_t>(key));
}

int guestPthreadSetSpecific(GuestKey key, const void* value) {
    return ::pthread_setspecific(static_cast<pthread_key_t>(key), value);
}

uint64_t pthreadBridgeSelfTest() {
    uint64_t passed = 0;
    alignas(8) unsigned char guestMutex[40]{};
    alignas(8) unsigned char guestCondition[48]{};
    alignas(8) unsigned char guestRwlock[56]{};
    alignas(8) unsigned char guestMutexAttr[8]{};
    alignas(8) unsigned char guestThreadAttr[64]{};
    int guestOnce = 0;

    if (guestPthreadMutexAttrInit(guestMutexAttr) == 0 &&
        guestPthreadMutexAttrSetType(guestMutexAttr, PTHREAD_MUTEX_RECURSIVE) == 0 &&
        guestPthreadMutexInit(guestMutex, guestMutexAttr) == 0 &&
        guestPthreadMutexLock(guestMutex) == 0 &&
        guestPthreadMutexLock(guestMutex) == 0 &&
        guestPthreadMutexUnlock(guestMutex) == 0 &&
        guestPthreadMutexUnlock(guestMutex) == 0 &&
        guestPthreadMutexDestroy(guestMutex) == 0 &&
        guestPthreadMutexAttrDestroy(guestMutexAttr) == 0) {
        passed |= 1ULL << 0;
    }

    if (guestPthreadCondInit(guestCondition, nullptr) == 0 &&
        guestPthreadCondSignal(guestCondition) == 0 &&
        guestPthreadCondBroadcast(guestCondition) == 0 &&
        guestPthreadCondDestroy(guestCondition) == 0) {
        passed |= 1ULL << 1;
    }

    if (guestPthreadRwlockInit(guestRwlock, nullptr) == 0 &&
        guestPthreadRwlockReadLock(guestRwlock) == 0 &&
        guestPthreadRwlockUnlock(guestRwlock) == 0 &&
        guestPthreadRwlockWriteLock(guestRwlock) == 0 &&
        guestPthreadRwlockUnlock(guestRwlock) == 0 &&
        guestPthreadRwlockDestroy(guestRwlock) == 0) {
        passed |= 1ULL << 2;
    }

    gOnceCounter = 0;
    if (guestPthreadOnce(&guestOnce, onceSelfTestCallback) == 0 &&
        guestPthreadOnce(&guestOnce, onceSelfTestCallback) == 0 && gOnceCounter == 1) {
        passed |= 1ULL << 3;
    }

    GuestKey key{};
    int tlsValue = 0x52;
    if (guestPthreadKeyCreate(&key, nullptr) == 0 &&
        guestPthreadSetSpecific(key, &tlsValue) == 0 &&
        guestPthreadGetSpecific(key) == &tlsValue && guestPthreadKeyDelete(key) == 0) {
        passed |= 1ULL << 4;
    }

    if (guestPthreadAttrInit(guestThreadAttr) == 0 &&
        guestPthreadAttrSetDetachState(guestThreadAttr, kGuestJoinable) == 0) {
        passed |= 1ULL << 5;
    }

    GuestKey destructorKey{};
    if (guestPthreadKeyCreate(&destructorKey, threadTlsSelfTestDestructor) == 0) {
        bool threadsPassed = true;
        GuestTlsScope parentTls;
        for (int exitMode = 0; exitMode < 2; ++exitMode) {
            ThreadTlsSelfTest test{};
            test.key = destructorKey;
            test.parentTls = parentTls.base();
            test.explicitExit = exitMode != 0;
            GuestThread thread{};
            void* threadResult = nullptr;
            const bool created = guestPthreadCreate(
                &thread, guestThreadAttr, threadSelfTestStart, &test) == 0;
            if (created && guestPthreadJoin(thread, &threadResult) != 0) {
                std::abort();
            }
            threadsPassed = threadsPassed && created && threadResult == &test &&
                test.entryValid && test.destructorValid &&
                selfTestTlsBase() == parentTls.base();
        }
        if (guestPthreadKeyDelete(destructorKey) == 0 && threadsPassed) {
            passed |= 1ULL << 6;
        }
    }
    if (guestPthreadAttrDestroy(guestThreadAttr) == 0) {
        passed |= 1ULL << 7;
    }

    // Detach semantics: a joinable thread detached by the guest must be
    // reaped by the host reaper without an explicit join. The slot is
    // recycled once the thread finishes, so repeated detach cycles must
    // not exhaust the fixed slot table.
    {
        GuestThread detachedThread{};
        ThreadTlsSelfTest detachTest{};
        detachTest.key = destructorKey;
        detachTest.parentTls = 0;
        if (guestPthreadCreate(&detachedThread, nullptr, threadSelfTestStart, &detachTest) == 0 &&
            guestPthreadDetach(detachedThread) == 0) {
            // Wait briefly for the reaper to collect the finished thread.
            bool reaped = false;
            for (int attempt = 0; attempt < 200; ++attempt) {
                mutexLock(&gRegistryLock);
                reaped = findThreadSlotLocked(detachedThread) == nullptr;
                mutexUnlock(&gRegistryLock);
                if (reaped) {
                    break;
                }
                svcSleepThread(1000000);
            }
            if (reaped && detachTest.entryValid) {
                passed |= 1ULL << 10;
            }
        }
    }

    const GuestThread self = guestPthreadSelf();
    if (self != 0 && guestPthreadEqual(self, self) != 0) {
        passed |= 1ULL << 8;
    }

    if (guestPthreadMutexLock(guestMutex) == 0 &&
        guestPthreadMutexUnlock(guestMutex) == 0 &&
        guestPthreadMutexDestroy(guestMutex) == 0) {
        passed |= 1ULL << 9;
    }
    return passed;
}

} // namespace rr3
