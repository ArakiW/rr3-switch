#include "rr3/recovery_guard.hpp"
#include "rr3/libcxx_shims.hpp"
#include "bionic_errno.hpp"

#include <switch.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace rr3 {

alignas(16) unsigned char bionicStandardFilesStorage[3 * kBionicFileObjectSize]{};
std::FILE* bionicStdoutObject = reinterpret_cast<std::FILE*>(
    bionicStandardFilesStorage + kBionicFileObjectSize);
std::FILE* bionicStderrObject = reinterpret_cast<std::FILE*>(
    bionicStandardFilesStorage + 2 * kBionicFileObjectSize);

namespace {

constexpr int kAndroidAtFdcwd = -100;
constexpr int kTargetAtFdcwd = -2;
constexpr int kAndroidScClkTck = 0x0006;
constexpr int kAndroidScPageSize = 0x0027;
constexpr int kAndroidScPageSizeAlias = 0x0028;
constexpr int kAndroidScProcessorsConfigured = 0x0060;
constexpr int kAndroidScProcessorsOnline = 0x0061;
constexpr long kAarch64SysFutex = 98;
constexpr long kAarch64SysGetTid = 178;
constexpr int kFutexWait = 0;
constexpr int kFutexWake = 1;
constexpr int kFutexPrivateFlag = 128;

thread_local char gAbortMessage[1024]{};
char gLogIdentity[64] = "RR3-libc++";

[[noreturn]] void libcxxFortifyFailure(const char* operation) {
    recordGuestTermination(operation, 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::printf("\nRR3 libc++ fortify failure: %s\n", operation);
    std::fflush(stdout);
    std::abort();
}

bool isCurrentDirectoryFd(int directoryFd) {
    return directoryFd == kAndroidAtFdcwd || directoryFd == kTargetAtFdcwd;
}

void libcxxVsyslog(int priority, const char* format, va_list arguments) {
    std::printf("[S%d/%s] ", priority, gLogIdentity);
    std::vprintf(format != nullptr ? format : "(null)", arguments);
    std::printf("\n");
}

std::FILE* translateBionicStandardFile(std::FILE* stream) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(stream);
    const uintptr_t base = reinterpret_cast<uintptr_t>(bionicStandardFilesStorage);
    if (value == base) {
        return stdin;
    }
    if (value == base + kBionicFileObjectSize) {
        return stdout;
    }
    if (value == base + 2 * kBionicFileObjectSize) {
        return stderr;
    }
    return stream;
}

// ---------------------------------------------------------------------------
// Futex trace.
//
// The two threads that stop advancing do so inside the guest's raw aarch64
// futex syscall, so this path answers "which word, and who was supposed to
// write it" by logging every operation: the address, the value it is compared
// against, the decoded op, the timeout, the caller's return address, the
// census key, and the Result svcWaitForAddress returned.
//
// Bounding matters here. A lock/unlock storm or a self-test flood would
// otherwise write millions of fsync'd lines to the SD card and bury the one
// line that matters, so detail is logged per thread for a fixed number of
// operations, indefinite waits get their own smaller budget (they are the
// park candidates and stay interesting far past the general budget), and
// everything past both degrades to a per-(op, address) counter that only
// prints a periodic summary.
// ---------------------------------------------------------------------------

constexpr const char* kFutexLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";
constexpr size_t kFutexSiteCount = 24;
// Full-detail lines per thread, and the extra allowance for the indefinite
// waits (timeout == nullptr) that are the actual park candidates.
constexpr unsigned kFutexDetailBudget = 32;
constexpr unsigned kFutexIndefiniteBudget = 16;
// Per-(op, address) counts are folded into one line this often. Small enough
// that a run with NO futex traffic at all is still distinguishable from one
// where the guest is quietly spinning on futex ops -- which is the whole
// question here -- and large enough that a lock/unlock storm stays bounded.
constexpr unsigned kFutexSummaryEvery = 64;
constexpr int kFutexClockRealtimeFlag = 256;

struct FutexSite {
    const int32_t* address{};
    int32_t command{};
    uint64_t count{};
    uint64_t waits{};
    uint64_t returned{};
    uint64_t failed{};
};

FutexSite gFutexSites[kFutexSiteCount];
unsigned gFutexSiteUsed{};
// Deliberately UNLOCKED. appendGuestBootLine is itself an unsynchronised
// fopen/fprintf/fsync (the census documents that concurrent writers just
// interleave), so a lock here buys ordering that is not needed. Both a libnx
// `Mutex` and a newlib pthread_mutex_t were tried on this path and, measured
// on Eden, the second acquire from the same thread never returned -- the boot
// stopped dead immediately after the self-test's first futex line. The trace
// must never be able to stop the thing it measures, so the counters below are
// best-effort and the log write goes straight through.
unsigned long long gFutexSequence{};
unsigned long long gFutexOpsSinceSummary{};
thread_local unsigned gFutexDetailLeft = kFutexDetailBudget;
thread_local unsigned gFutexIndefiniteLeft = kFutexIndefiniteBudget;

const char* futexCommandName(int32_t command) {
    switch (command) {
        case 0: return "WAIT";
        case 1: return "WAKE";
        case 2: return "FD";
        case 3: return "REQUEUE";
        case 4: return "CMP_REQUEUE";
        case 5: return "WAIT_BITSET";
        case 6: return "WAKE_BITSET";
        case 7: return "WAIT_REQUEUE_PI";
        case 8: return "CMP_REQUEUE_PI";
        case 9: return "LOCK_PI";
        case 10: return "UNLOCK_PI";
        case 11: return "TRYLOCK_PI";
        default: return "UNKNOWN";
    }
}

// Same register the pthread bridge's census keys on (TPIDRRO_EL0 survives
// GuestTlsScope, so this identifies the census entry for the calling thread).
uintptr_t futexThreadKey() {
    uintptr_t value{};
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

const char* futexFlagName(int operation) {
    return (operation & kFutexClockRealtimeFlag) != 0 ? "+CLOCK_REALTIME" : "";
}

FutexSite* futexSiteFor(const int32_t* address, int32_t command) {
    for (unsigned index = 0; index < gFutexSiteUsed; ++index) {
        if (gFutexSites[index].address == address &&
            gFutexSites[index].command == command) {
            return &gFutexSites[index];
        }
    }
    if (gFutexSiteUsed >= kFutexSiteCount) {
        // The table is full: fold the new word into the first slot rather than
        // growing without bound. Dropping a site only costs it its own
        // identity in the summary, never its line-level trace.
        return &gFutexSites[0];
    }
    FutexSite& site = gFutexSites[gFutexSiteUsed++];
    site.address = address;
    site.command = command;
    return &site;
}

void futexLog(const char* phase, unsigned long long sequence,
              const int32_t* address, const char* details) {
    appendGuestBootLine(kFutexLogPath, phase, sequence,
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(address)),
                        details);
}

FutexSite* futexSiteFor(const int32_t* address, int32_t command);
void futexLog(const char* phase, unsigned long long sequence,
              const int32_t* address, const char* details);
void futexLogCounts();

// Called before svcWaitForAddress and before every early return, so a wait
// that never comes back still leaves its line behind.
//
// Only ops that actually PARK are written to the log. The EAGAIN fast path and
// the wake are the hot ones -- every contended guest mutex runs through them
// thousands of times -- and doing file I/O for each one both floods the SD card
// and, measured on Eden, wedged the boot right here the first time this
// landed. They still feed the per-(op, address) counters and the summary, so
// nothing about them is lost; only their line-level detail is.
void futexTraceEnter(uint32_t operation, int32_t command, int value,
                     const int32_t* address, s64 timeoutNanoseconds,
                     const void* caller, bool parked) {
    const int observed =
        address != nullptr ? *reinterpret_cast<const volatile int32_t*>(address) : 0;
    FutexSite* site = futexSiteFor(address, command);
    ++site->count;
    if (parked) {
        ++site->waits;
    }
    const bool indefinite = timeoutNanoseconds < 0;
    const bool detail = parked &&
        (gFutexDetailLeft > 0 || (indefinite && gFutexIndefiniteLeft > 0));
    if (detail) {
        if (gFutexDetailLeft > 0) {
            --gFutexDetailLeft;
        }
        if (indefinite && gFutexIndefiniteLeft > 0) {
            --gFutexIndefiniteLeft;
        }
    }
    const unsigned long long sequence = gFutexSequence++;
    if (++gFutexOpsSinceSummary >= kFutexSummaryEvery) {
        gFutexOpsSinceSummary = 0;
    }
    const bool summarise = gFutexOpsSinceSummary == 0;
    // errno is saved and restored around every log write. Callers of this shim
    // (and the bionic pthread code behind them) branch on the exact errno a
    // failed wait left behind, and plain file I/O tramples it.
    const int savedErrno = errno;
    char details[320];
    details[0] = '\0';
    if (detail) {
        __builtin_snprintf(details, sizeof(details),
                           "op=%s raw_op=%u%s val=%d mem=%d timeout_ns=%lld "
                           "indefinite=%d tpidrro=0x%llx lr=0x%llx detail=1",
                           futexCommandName(command), operation,
                           futexFlagName(static_cast<int>(operation)), value,
                           observed, static_cast<long long>(timeoutNanoseconds),
                           indefinite ? 1 : 0,
                           static_cast<unsigned long long>(futexThreadKey()),
                           static_cast<unsigned long long>(
                               reinterpret_cast<uintptr_t>(caller)));
    }
    if (detail) {
        futexLog("futex-park", sequence, address, details);
        errno = savedErrno;
    }
    if (summarise) {
        futexLogCounts();
        errno = savedErrno;
    }
}

void futexTraceLeave(int32_t command, const int32_t* address,
                     s64 timeoutNanoseconds, u32 result, long returned,
                     int failureErrno, bool parked) {
    char details[192];
    details[0] = '\0';
    {
        FutexSite* site = futexSiteFor(address, command);
        if (result == 0) {
            ++site->returned;
        } else {
            ++site->failed;
        }
        __builtin_snprintf(details, sizeof(details),
                           "op=%s res=0x%08x failed=%d ret=%ld errno=%d "
                           "timeout_ns=%lld", futexCommandName(command), result,
                           R_FAILED(result) ? 1 : 0, returned, failureErrno,
                           static_cast<long long>(timeoutNanoseconds));
    }
    // A park that COMES BACK is worth a line: it is the other half of the
    // park/signal pair. errno is preserved across the write.
    if (parked) {
        const int savedErrno = errno;
        futexLog("futex-return", gFutexSequence++, address, details);
        errno = savedErrno;
    }
}

void futexLogCounts() {
    char details[320];
    size_t used = 0;
    details[0] = '\0';
    const unsigned sites = gFutexSiteUsed;
    for (unsigned index = 0; index < sites; ++index) {
        const FutexSite& site = gFutexSites[index];
        if (site.count < 2) {
            continue;
        }
        const int written = __builtin_snprintf(
            details + used, sizeof(details) - used,
            "%s%s@0x%llx=%llu/w%llu/r%llu/f%llu", used == 0 ? "" : " ",
            futexCommandName(site.command),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(site.address)),
            static_cast<unsigned long long>(site.count),
            static_cast<unsigned long long>(site.waits),
            static_cast<unsigned long long>(site.returned),
            static_cast<unsigned long long>(site.failed));
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
        if (used >= sizeof(details) - 1) {
            break;
        }
    }
    futexLog("futex-counts", gFutexSequence++, nullptr, details);
}

// ---------------------------------------------------------------------------
// Indefinite-wait probe.
//
// The branch that actually parks in the field -- timeout == nullptr, i.e.
// timeoutNanoseconds == -1 -- had no coverage: the self-test only exercised
// the EAGAIN fast path and the wake.
//
// The probe is therefore ENTIRELY off the boot thread. An earlier version ran
// it inline and stalled the launch on Eden right after its first log line, so
// nothing after the self-test ever ran and no guest futex was ever observed.
// A self-test that can wedge the thing it is instrumenting is worthless, so
// the whole sequence now lives on one detached helper: it waits, wakes, polls
// and reports through the log. The self-test mask is unchanged (main.cpp
// prints "expected=7" and is not ours to edit); the probe's verdict is
// evidence in the log, not a bit in that mask.
// ---------------------------------------------------------------------------

struct FutexIndefiniteProbe {
    alignas(4) int32_t word;
    volatile int32_t finished;
    long returned;
    int failureErrno;
};

// BSS, not a stack frame: a probe that never wakes must not be left pointing
// at memory whose owner has already returned.
FutexIndefiniteProbe gFutexIndefiniteProbe{5, 0, 0, 0};
bool gFutexProbeStarted = false;

void* futexIndefiniteWaiterEntry(void* opaque) {
    auto* probe = static_cast<FutexIndefiniteProbe*>(opaque);
    errno = 0;
    probe->returned = libcxxSyscall(
        kAarch64SysFutex, &probe->word, kFutexWait | kFutexPrivateFlag, 5,
        static_cast<const struct timespec*>(nullptr), nullptr, 0);
    probe->failureErrno = errno;
    probe->finished = 1;
    return nullptr;
}

// The whole probe, off the boot thread: start a waiter, let it park, release
// it, then report whether the indefinite-wait branch actually round-tripped.
void* futexIndefiniteWaitDriver(void* opaque) {
    (void)opaque;
    gFutexIndefiniteProbe.word = 5;
    gFutexIndefiniteProbe.finished = 0;
    gFutexIndefiniteProbe.returned = -1;
    gFutexIndefiniteProbe.failureErrno = 0;

    pthread_attr_t attribute;
    pthread_t waiter;
    if (::pthread_attr_init(&attribute) != 0) {
        return nullptr;
    }
    ::pthread_attr_setstacksize(&attribute, 0x4000);
    ::pthread_attr_setdetachstate(&attribute, PTHREAD_CREATE_DETACHED);
    const int created = ::pthread_create(&waiter, &attribute,
                                         futexIndefiniteWaiterEntry,
                                         &gFutexIndefiniteProbe);
    ::pthread_attr_destroy(&attribute);
    if (created != 0) {
        return nullptr;
    }

    svcSleepThread(50000000LL); // 50 ms: let the waiter reach the wait
    const bool woken = libcxxSyscall(
        kAarch64SysFutex, &gFutexIndefiniteProbe.word,
        kFutexWake | kFutexPrivateFlag, 1,
        static_cast<const struct timespec*>(nullptr), nullptr, 0) == 0;
    for (unsigned poll = 0; poll < 200 && gFutexIndefiniteProbe.finished == 0;
         ++poll) {
        svcSleepThread(10000000LL); // 10 ms, at most 2 s
    }
    const bool completed = gFutexIndefiniteProbe.finished != 0;
    char details[224];
    __builtin_snprintf(details, sizeof(details),
                       "indefinite_wake=%d completed=%d ret=%ld errno=%d "
                       "verdict=%s probe=0x%llx", woken ? 1 : 0, completed ? 1 : 0,
                       gFutexIndefiniteProbe.returned,
                       gFutexIndefiniteProbe.failureErrno,
                       (woken && completed && gFutexIndefiniteProbe.returned == 0)
                           ? "indefinite-wait-ok" : "indefinite-wait-BROKEN",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                           &gFutexIndefiniteProbe.word)));
    futexLog("futex-selftest", gFutexSequence++, &gFutexIndefiniteProbe.word,
             details);
    return nullptr;
}

// Kicks off the probe and returns immediately. Never blocks the caller.
void futexStartIndefiniteWaitProbe() {
    if (gFutexProbeStarted) {
        return;
    }
    gFutexProbeStarted = true;
    pthread_attr_t attribute;
    pthread_t prober;
    if (::pthread_attr_init(&attribute) != 0) {
        return;
    }
    ::pthread_attr_setstacksize(&attribute, 0x8000);
    ::pthread_attr_setdetachstate(&attribute, PTHREAD_CREATE_DETACHED);
    const int created = ::pthread_create(&prober, &attribute, futexIndefiniteWaitDriver, nullptr);
    ::pthread_attr_destroy(&attribute);
    char details[96];
    __builtin_snprintf(details, sizeof(details), "step=probe-start rc=%d", created);
    futexLog("futex-selftest", gFutexSequence++, nullptr, details);
}

} // namespace


size_t bionicCtypeGetMbCurMax() {
    return MB_CUR_MAX;
}

int bionicOpen2(const char* path, int flags) {
    if ((flags & O_CREAT) != 0) {
        libcxxFortifyFailure("__open_2 called with O_CREAT");
    }
    return ::open(path, flags);
}

ssize_t bionicReadChk(int fd, void* destination, size_t count,
                      size_t destinationSize) {
    if (count > destinationSize) {
        libcxxFortifyFailure("__read_chk");
    }
    return ::read(fd, destination, count);
}

size_t bionicStrlenChk(const char* string, size_t stringSize) {
    const void* terminator = std::memchr(string, '\0', stringSize);
    if (terminator == nullptr) {
        libcxxFortifyFailure("__strlen_chk");
    }
    return static_cast<const char*>(terminator) - string;
}

const char* guestAbortMessage() {
    return gAbortMessage;
}

void androidSetAbortMessage(const char* message) {
    if (message == nullptr) {
        gAbortMessage[0] = '\0';
        return;
    }
    std::strncpy(gAbortMessage, message, sizeof(gAbortMessage) - 1);
    gAbortMessage[sizeof(gAbortMessage) - 1] = '\0';
}

int bionicFflush(std::FILE* stream) {
    return std::fflush(stream != nullptr ? translateBionicStandardFile(stream) : nullptr);
}

int bionicVfprintf(std::FILE* stream, const char* format, va_list arguments) {
    return std::vfprintf(translateBionicStandardFile(stream), format, arguments);
}

int bionicFprintf(std::FILE* stream, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int result = bionicVfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int bionicFputc(int character, std::FILE* stream) {
    return std::fputc(character, translateBionicStandardFile(stream));
}

// Save-write probe, 2026-10-04.
//
// 0x1105160 is called unconditionally from the per-frame path (0x12b48f0, inside
// onViewRenderJNI's body) and its only stdio calls are fopen / fwrite / fclose
// -- the save write. The guest then fails with SaveManager.cpp:699's
// "error loading your game data" and spins for ~900k unmapped accesses, and we
// have never once seen what fwrite returned. A short write is the classic way
// that turns into a retry loop, so it is worth one line when it happens.
//
// fwrite RETURNS ITEM COUNT, NOT BYTES. The first version of this probe tested
// `written != size * count` and reported 18 "short writes" that were 18
// perfectly good `fwrite(p, N, 1, f)` calls returning 1. The guest check is
// `written != count`; bytes actually written is `written * size`.
//
// Writes of kSaveProbeBytes or more are logged unconditionally, because the
// save payload is the one call that matters and a *successful* one is exactly
// what a "only log failures" probe never prints.
//
// Deliberately NOT logged on the small success path: this is a guest-visible hot
// function and the guest-visible paths must not pay a lock. Nothing here
// changes what the guest sees -- it only reports.
static std::atomic<uint64_t> gFwriteCalls{0};
static std::atomic<uint64_t> gFwriteShort{0};
static std::atomic<uint64_t> gFwriteBytes{0};
constexpr size_t kSaveProbeBytes = 64;

size_t bionicFwrite(const void* data, size_t size, size_t count, std::FILE* stream) {
    std::FILE* const real = translateBionicStandardFile(stream);
    // The errno in the log line has to belong to THIS call. A successful
    // fwrite does not clear errno, so without this the line reports
    // whatever some earlier operation left behind.
    //
    // MEASURED 2026-10-24, archive 20261024-121533-eden-race-steer2: 64 of
    // the 113 save writes printed "e=2(No such file or directory)" while
    // every one of them wrote all of its bytes (gotItems=wantB,
    // gotB=wantB, ferror=0). That reads exactly like "the game cannot save
    // its profile" -- and it is false: doc/CC_Data on the card holds
    // player_profile.2.dat, profile.dat and character.2.dat, and the
    // guest's own "Error creating directory" line is the harmless EEXIST
    // case the port already turns into a success.
    //
    // So the errno is cleared here, and the line carries an explicit
    // verdict instead of leaving the reader to interpret a number.
    errno = 0;
    const size_t written = std::fwrite(data, size, count, real);
    const size_t wantBytes = size * count;
    gFwriteCalls.fetch_add(1, std::memory_order_relaxed);
    gFwriteBytes.fetch_add(written * size, std::memory_order_relaxed);
    const bool shortWrite = (written != count);
    if (shortWrite) {
        gFwriteShort.fetch_add(1, std::memory_order_relaxed);
    }
    if (shortWrite || wantBytes >= kSaveProbeBytes) {
        char line[224];
        const int n = __builtin_snprintf(
            line, sizeof(line),
            "fwrite-%s: wantB=%llu gotItems=%llu/%llu gotB=%llu ferror=%d e=%d(%s)"
            " verdict=%s",
            shortWrite ? "short" : "save",
            static_cast<unsigned long long>(wantBytes),
            static_cast<unsigned long long>(written),
            static_cast<unsigned long long>(count),
            static_cast<unsigned long long>(written * size),
            std::ferror(real) != 0, errno,
            std::strerror(errno),
            (!shortWrite && std::ferror(real) == 0) ? "wrote-all"
                                                    : "INCOMPLETE");
        if (n > 0) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log", line, 0, 0);
        }
    }
    return written;
}

int bionicGetc(std::FILE* stream) {
    return std::getc(translateBionicStandardFile(stream));
}

int bionicUngetc(int character, std::FILE* stream) {
    return std::ungetc(character, translateBionicStandardFile(stream));
}

void bionicSetbuf(std::FILE* stream, char* buffer) {
    std::setbuf(translateBionicStandardFile(stream), buffer);
}

int bionicSetvbuf(std::FILE* stream, char* buffer, int mode, size_t size) {
    return std::setvbuf(translateBionicStandardFile(stream), buffer, mode, size);
}

void libcxxCloseLog() {
    std::strcpy(gLogIdentity, "RR3-libc++");
}

int libcxxFchmodAt(int directoryFd, const char* path, mode_t mode, int flags) {
    if (!isCurrentDirectoryFd(directoryFd) || flags != 0) {
        errno = kGuestErrnoENOTSUP;
        return -1;
    }
    return ::chmod(path, mode);
}

void libcxxOpenLog(const char* identity, int options, int facility) {
    (void)options;
    (void)facility;
    if (identity == nullptr || identity[0] == '\0') {
        std::strcpy(gLogIdentity, "RR3-libc++");
        return;
    }
    std::strncpy(gLogIdentity, identity, sizeof(gLogIdentity) - 1);
    gLogIdentity[sizeof(gLogIdentity) - 1] = '\0';
}

int libcxxPosixMemalign(void** result, size_t alignment, size_t size) {
    if (result == nullptr || alignment < sizeof(void*) ||
        (alignment & (alignment - 1)) != 0 || alignment % sizeof(void*) != 0) {
        return EINVAL;
    }
    void* allocation = ::memalign(alignment, size);
    if (allocation == nullptr) {
        return ENOMEM;
    }
    *result = allocation;
    return 0;
}

ssize_t libcxxSendfile(int outputFd, int inputFd, off_t* offset, size_t count) {
    off_t originalPosition = -1;
    if (offset != nullptr) {
        originalPosition = ::lseek(inputFd, 0, SEEK_CUR);
        if (originalPosition < 0 || ::lseek(inputFd, *offset, SEEK_SET) < 0) {
            return -1;
        }
    }

    char buffer[16 * 1024];
    size_t remaining = count;
    ssize_t total = 0;
    int savedErrno = 0;
    while (remaining != 0) {
        const size_t request = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const ssize_t received = ::read(inputFd, buffer, request);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            savedErrno = errno;
            if (total == 0) {
                total = -1;
            }
            break;
        }

        ssize_t written = 0;
        while (written < received) {
            const ssize_t result =
                ::write(outputFd, buffer + written, static_cast<size_t>(received - written));
            if (result <= 0) {
                savedErrno = result < 0 ? errno : EIO;
                if (total == 0 && written == 0) {
                    total = -1;
                }
                remaining = 0;
                break;
            }
            written += result;
            total += result;
            remaining -= static_cast<size_t>(result);
        }
        if (written != received) {
            break;
        }
    }

    if (offset != nullptr) {
        const off_t finalPosition = ::lseek(inputFd, 0, SEEK_CUR);
        if (finalPosition >= 0) {
            *offset = finalPosition;
        }
        if (::lseek(inputFd, originalPosition, SEEK_SET) < 0 && savedErrno == 0) {
            savedErrno = errno;
            if (total == 0) {
                total = -1;
            }
        }
    }
    if (savedErrno != 0) {
        errno = savedErrno;
    }
    return total;
}

long libcxxSysconf(int name) {
    switch (name) {
        case kAndroidScClkTck:
            return 100;
        case kAndroidScPageSize:
        case kAndroidScPageSizeAlias:
            return 0x1000;
        case kAndroidScProcessorsConfigured:
        case kAndroidScProcessorsOnline:
            return 4;
        default:
            errno = kGuestErrnoEINVAL;
            return -1;
    }
}

long libcxxSyscall(long number, ...) {
    if (number == kAarch64SysGetTid) {
        u64 threadId = 0;
        const Result result = svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
        if (R_FAILED(result)) {
            errno = kGuestErrnoEIO;
            return -1;
        }
        return static_cast<long>(threadId);
    }
    if (number != kAarch64SysFutex) {
        errno = kGuestErrnoENOSYS;
        return -1;
    }

    va_list arguments;
    va_start(arguments, number);
    auto* address = va_arg(arguments, int32_t*);
    const int operation = va_arg(arguments, int);
    const int value = va_arg(arguments, int);
    const auto* timeout = va_arg(arguments, const struct timespec*);
    (void)va_arg(arguments, void*);
    (void)va_arg(arguments, int);
    va_end(arguments);

    if (address == nullptr || (reinterpret_cast<uintptr_t>(address) & 3U) != 0) {
        errno = kGuestErrnoEINVAL;
        return -1;
    }
    const int command = operation & ~kFutexPrivateFlag;
    const void* caller = __builtin_return_address(0);
    if (command == kFutexWake) {
        futexTraceEnter(operation, command, value, address, 0, caller, false);
        const Result result = svcSignalToAddress(
            address, SignalType_Signal, 0, value < 0 ? 0 : value);
        if (R_FAILED(result)) {
            errno = kGuestErrnoEIO;
            futexTraceLeave(command, address, 0, result, -1, EIO, false);
            return -1;
        }
        futexTraceLeave(command, address, 0, result, 0, 0, false);
        return 0;
    }
    if (command != kFutexWait) {
        errno = kGuestErrnoENOSYS;
        return -1;
    }
    if (*reinterpret_cast<volatile int32_t*>(address) != value) {
        errno = kGuestErrnoEAGAIN;
        futexTraceEnter(operation, command, value, address, -1, caller, false);
        futexTraceLeave(command, address, -1, 0xffffffffu, -1, EAGAIN, false);
        return -1;
    }

    s64 timeoutNanoseconds = -1;
    if (timeout != nullptr) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L ||
            timeout->tv_sec > (INT64_MAX - timeout->tv_nsec) / 1000000000LL) {
            errno = kGuestErrnoEINVAL;
            return -1;
        }
        timeoutNanoseconds = static_cast<s64>(timeout->tv_sec) * 1000000000LL +
            static_cast<s64>(timeout->tv_nsec);
    }
    // Logged BEFORE the wait: this is the line that survives when the guest
    // parks forever and never reaches the log line below.
    futexTraceEnter(operation, command, value, address, timeoutNanoseconds,
                    caller, true);
    const Result result = svcWaitForAddress(
        address, ArbitrationType_WaitIfEqual, value, timeoutNanoseconds);
    if (R_FAILED(result)) {
        errno = timeout != nullptr ? ETIMEDOUT : EAGAIN;
        futexTraceLeave(command, address, timeoutNanoseconds, result, -1, errno, true);
        return -1;
    }
    futexTraceLeave(command, address, timeoutNanoseconds, result, 0, 0, true);
    return 0;
}

void libcxxSyslog(int priority, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    libcxxVsyslog(priority, format, arguments);
    va_end(arguments);
}

int libcxxUtimensAt(int directoryFd, const char* path,
                    const struct timespec times[2], int flags) {
    if (!isCurrentDirectoryFd(directoryFd) || flags != 0) {
        errno = kGuestErrnoENOTSUP;
        return -1;
    }
    if (times == nullptr) {
        return ::utimes(path, nullptr);
    }

    struct timeval converted[2]{};
    for (size_t index = 0; index < 2; ++index) {
        if (times[index].tv_nsec < 0 || times[index].tv_nsec >= 1000000000L) {
            errno = kGuestErrnoEINVAL;
            return -1;
        }
        converted[index].tv_sec = times[index].tv_sec;
        converted[index].tv_usec = static_cast<suseconds_t>(times[index].tv_nsec / 1000);
    }
    return ::utimes(path, converted);
}

uint64_t libcxxShimSelfTest() {
    uint64_t passed = 0;

    if (bionicCtypeGetMbCurMax() >= 1) {
        passed |= 1ULL << 0;
    }
    if (bionicStrlenChk("libc++", 7) == 6) {
        passed |= 1ULL << 1;
    }

    int fd = bionicOpen2("romfs:/libc++_shared.so", O_RDONLY);
    if (fd >= 0) {
        unsigned char magic[4]{};
        if (bionicReadChk(fd, magic, sizeof(magic), sizeof(magic)) == 4 &&
            magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
            passed |= 1ULL << 2;
        }
        ::close(fd);
    }

    void* allocation = nullptr;
    if (libcxxPosixMemalign(&allocation, 64, 127) == 0 && allocation != nullptr &&
        (reinterpret_cast<uintptr_t>(allocation) & 63U) == 0) {
        passed |= 1ULL << 3;
    }
    std::free(allocation);

    if (libcxxSysconf(kAndroidScPageSize) == 0x1000 &&
        libcxxSysconf(kAndroidScProcessorsOnline) == 4) {
        passed |= 1ULL << 4;
    }

    androidSetAbortMessage("RR3-v8");
    if (std::strcmp(gAbortMessage, "RR3-v8") == 0) {
        passed |= 1ULL << 5;
    }

    errno = 0;
    if (libcxxFchmodAt(123, "unused", 0, 0) == -1 && errno == ENOTSUP) {
        passed |= 1ULL << 6;
    }
    errno = 0;
    if (libcxxUtimensAt(123, "unused", nullptr, 0) == -1 && errno == ENOTSUP) {
        passed |= 1ULL << 7;
    }

    return passed;
}

uint64_t libcxxSyscallSelfTest() {
    uint64_t passed = 0;
    const long firstThreadId = libcxxSyscall(kAarch64SysGetTid);
    const long secondThreadId = libcxxSyscall(kAarch64SysGetTid);
    if (firstThreadId > 0 && firstThreadId == secondThreadId) {
        passed |= 1ULL << 0;
    }

    alignas(4) int32_t futexWord = 5;
    errno = 0;
    if (libcxxSyscall(kAarch64SysFutex, &futexWord,
                      kFutexWait | kFutexPrivateFlag, 6,
                      static_cast<const struct timespec*>(nullptr), nullptr, 0) == -1 &&
        errno == EAGAIN) {
        passed |= 1ULL << 1;
    }
    if (libcxxSyscall(kAarch64SysFutex, &futexWord,
                      kFutexWake | kFutexPrivateFlag, 1,
                      static_cast<const struct timespec*>(nullptr), nullptr, 0) == 0) {
        passed |= 1ULL << 2;
    }
    // The indefinite-wait branch is covered by futexStartIndefiniteWaitProbe,
    // which runs off this thread: its verdict is written to the log because a
    // bit here would either change main.cpp's printed mask or put a
    // self-test that can park the boot back on the critical path.
    futexStartIndefiniteWaitProbe();
    // Baseline the counters before the guest exists, so a later summary is
    // directly comparable against "the game issued no futex traffic at all".
    futexLogCounts();
    return passed;
}

uint64_t bionicStdioSelfTest() {
    uint64_t passed = 0;
    auto* guestStdin = reinterpret_cast<std::FILE*>(bionicStandardFilesStorage);
    auto* guestStdout = reinterpret_cast<std::FILE*>(
        bionicStandardFilesStorage + kBionicFileObjectSize);
    auto* guestStderr = reinterpret_cast<std::FILE*>(
        bionicStandardFilesStorage + 2 * kBionicFileObjectSize);

    if (reinterpret_cast<uintptr_t>(guestStdout) - reinterpret_cast<uintptr_t>(guestStdin) ==
            kBionicFileObjectSize &&
        reinterpret_cast<uintptr_t>(guestStderr) - reinterpret_cast<uintptr_t>(guestStdin) ==
            2 * kBionicFileObjectSize) {
        passed |= 1ULL << 0;
    }
    if (bionicFwrite("", 0, 0, guestStdout) == 0 && bionicFflush(guestStdout) == 0) {
        passed |= 1ULL << 1;
    }
    if (bionicFprintf(guestStderr, "") == 0) {
        passed |= 1ULL << 2;
    }
    return passed;
}

} // namespace rr3
