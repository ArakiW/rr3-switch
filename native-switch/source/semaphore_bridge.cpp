#include "rr3/semaphore_bridge.hpp"
#include "rr3/guest_view_trace.hpp"
#include "bionic_errno.hpp"

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
        errno = kGuestErrnoEINVAL;
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

// Bounded wait slices for BOTH semaphore waiters. An unbounded semaphoreWait
// has no breadcrumb when an FMOD mixer thread blocks on a missing Android audio
// callback, so a blocked guest is invisible. Each 500 ms slice reports via the
// existing sync-blocked primitive (same pattern as guestPthreadCondWait). The
// waits themselves are unchanged: guestSemWait stays indefinite, and
// guestSemTimedWait still re-checks the guest's absolute deadline every
// iteration, so only the granularity of the poll changes.
constexpr long kSyncBlockedReportNanos = 500000000LL;

} // namespace

int guestSemInit(void* guestSemaphore, int processShared, unsigned int value) {
    if (guestSemaphore == nullptr || processShared != 0) {
        errno = processShared != 0 ? ENOSYS : EINVAL;
        return -1;
    }
    mutexLock(&gSemaphoreRegistryMutex);
    if (findEntry(guestSemaphore) != nullptr) {
        mutexUnlock(&gSemaphoreRegistryMutex);
        errno = kGuestErrnoEBUSY;
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
    errno = kGuestErrnoENOSPC;
    return -1;
}

int guestSemDestroy(void* guestSemaphore) {
    mutexLock(&gSemaphoreRegistryMutex);
    SemaphoreEntry* entry = findEntry(guestSemaphore);
    if (entry == nullptr) {
        mutexUnlock(&gSemaphoreRegistryMutex);
        errno = kGuestErrnoEINVAL;
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
    // Bounded wait slices: see kSyncBlockedReportNanos. The wait itself is
    // still indefinite so legitimate long waits keep their blocking semantics.
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
        errno = kGuestErrnoEAGAIN;
        return -1;
    }
    return 0;
}

int guestSemTimedWait(void* guestSemaphore, const struct timespec* absoluteTimeout) {
    if (absoluteTimeout == nullptr || absoluteTimeout->tv_nsec < 0 ||
        absoluteTimeout->tv_nsec >= 1000000000L) {
        errno = kGuestErrnoEINVAL;
        return -1;
    }
    SemaphoreEntry* entry = requireEntry(guestSemaphore);
    if (entry == nullptr) {
        return -1;
    }
    // Same slice and breadcrumb as guestSemWait (ids 52/53/54 are mutex,
    // cond-wait and sem-wait; 55 is this one). This was the only live 1 ms poll
    // loop left in the tree and the only waiter with no diagnostic at all.
    // The deadline check stays at the top of every iteration and is never
    // skipped or reordered behind the sleep, so the guest's absolute deadline
    // is still honoured on each pass; only the sleep between passes changed,
    // from 1 ms to the shared 500 ms slice.
    while (!semaphoreTryWait(&entry->native)) {
        if (deadlineReached(*absoluteTimeout)) {
            // NB: this constant is deliberately left as the HOST libc's
            // numbering. The guest is a bionic build and devkitA64 is newlib,
            // so ETIMEDOUT here is 116, which the guest reads as ESTALE. That
            // mismatch is real and already quantified, but it is being fixed
            // separately -- do not "correct" the value in this file.
            errno = kGuestErrnoETIMEDOUT;
            return -1;
        }
        guestViewTraceSyncBlocked("sem-timedwait", 55);
        svcSleepThread(kSyncBlockedReportNanos);
    }
    return 0;
}

int guestSemGetValue(void* guestSemaphore, int* value) {
    if (value == nullptr) {
        errno = kGuestErrnoEINVAL;
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
    errno = kGuestErrnoENOSYS;
    return reinterpret_cast<void*>(-1);
}

int guestSemClose(void*) {
    errno = kGuestErrnoENOSYS;
    return -1;
}

int guestSemUnlink(const char*) {
    errno = kGuestErrnoENOSYS;
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
