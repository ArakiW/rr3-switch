#include "rr3/recovery_guard.hpp"
#include "rr3/libcxx_shims.hpp"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <switch.h>

extern "C" {
alignas(16) unsigned char __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* dump);
}

namespace {
FsFile g_crashFile{};
bool g_crashFileOpen = false;
unsigned int g_handlingCrash = 0;
char g_crashText[2048]{};
size_t g_crashLength = 0;
// Handle of the thread that is currently inside the exception handler, so the
// second one in can name the first instead of both claiming the same registers.
unsigned int g_crashFaultingThread = 0;

// Serialises every writer of the shared g_crashFile / g_crashFileOpen /
// g_crashLength trio: guestTracedAbort, guestTracedExit and guestTracedRaise
// can all be entered by two guest threads at once, and they interleave into one
// FsFile. This is a libnx Mutex, the same primitive pthread_bridge.cpp's
// gRegistryLock already takes on guest paths, and deliberately NOT a std::mutex:
// an earlier attempt to put a std::mutex on a guest-visible path here starved
// the emulator's scheduler and burned 187 s of CPU.
Mutex g_crashFileMutex{};

// ---------------------------------------------------------------------------
// Cached append handles for appendGuestBootLine().
//
// Measured: the archived run 20261003-170207-percall-io recorded 911 filesystem
// HLE round-trips (565 on guest-view.log, 282 on guest-boot.progress), and the
// count was byte-identical across three consecutive runs because it is a pure
// function of the deterministic guest start-up sequence. The per-line
// fopen("a") is what produces one of those round-trips per line, and 120+ call
// sites put it on GL entry points (per draw call), per JNI call and per symbol
// resolution.
//
// Fixed-size table, no dynamic container: this file stays allocation-light and
// -fno-exceptions clean. Append mode ONLY -- see writeGuestBootProgressImpl for
// why a truncate-mode handle can never live here.
// ---------------------------------------------------------------------------
constexpr size_t kGuestLogCacheSlots = 8;
constexpr size_t kGuestLogPathMax = 192;

struct GuestLogCacheSlot {
    char path[kGuestLogPathMax];
    std::FILE* file;
    bool used;
};

GuestLogCacheSlot g_logCache[kGuestLogCacheSlots];
unsigned int g_logCacheVictim = 0;
unsigned long long g_logCacheOpens = 0;
unsigned long long g_logCacheReuses = 0;
unsigned long long g_logCacheReopens = 0;
unsigned int g_logCacheAnnounced = 0;

// One lock for the whole cache, not one per slot: the critical section is a
// single fprintf + fflush with no allocation, so per-slot locking would only
// add a lock-ordering question. See the thread-safety block in
// appendGuestBootLine for why this is a libnx Mutex and not something cheaper.
Mutex g_logCacheMutex{};

// Caller holds g_logCacheMutex. Finds the slot for `path`, or claims a free one,
// or evicts the round-robin victim. An eviction fclose is lossless: the handle
// is "a", so a later reopen continues at EOF.
GuestLogCacheSlot* logCacheSlotLocked(const char* path) {
    for (size_t index = 0; index < kGuestLogCacheSlots; ++index) {
        GuestLogCacheSlot& slot = g_logCache[index];
        if (slot.used && std::strcmp(slot.path, path) == 0) {
            return &slot;
        }
    }
    size_t chosen = kGuestLogCacheSlots;
    for (size_t index = 0; index < kGuestLogCacheSlots; ++index) {
        if (!g_logCache[index].used) {
            chosen = index;
            break;
        }
    }
    if (chosen == kGuestLogCacheSlots) {
        chosen = g_logCacheVictim;
        g_logCacheVictim = static_cast<unsigned int>((chosen + 1) % kGuestLogCacheSlots);
        GuestLogCacheSlot& victim = g_logCache[chosen];
        if (victim.file != nullptr) {
            std::fclose(victim.file);
        }
        victim.used = false;
    }
    GuestLogCacheSlot& slot = g_logCache[chosen];
    std::strncpy(slot.path, path, kGuestLogPathMax - 1);
    slot.path[kGuestLogPathMax - 1] = '\0';
    slot.file = nullptr;
    slot.used = true;
    return &slot;
}

void crashText(const char* text) {
    while (*text != '\0' && g_crashLength < sizeof(g_crashText)) {
        g_crashText[g_crashLength++] = *text++;
    }
}

void crashHex(const char* name, u64 value) {
    crashText(name);
    crashText("=");
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (g_crashLength < sizeof(g_crashText)) {
            g_crashText[g_crashLength++] = digits[(value >> shift) & 15];
        }
    }
    crashText("\n");
}
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* dump) {
    // __nx_exceptiondump is a PROCESS-GLOBAL object (libnx's weak BSS symbol,
    // which this file does not override). If two guest threads fault, the
    // second one overwrites pc/lr/cpu_gprs[] before this handler has read them,
    // so the register block below can describe a different thread than the one
    // that died. The registers are what they are; what this adds is the
    // faulting thread's own identity next to them -- u64 tpidr and the kernel
    // Handle are read on the faulting thread's own stack, so they always belong
    // to the thread that actually faulted -- plus an explicit verdict.
    //
    // Honest limit: g_handlingCrash can only see a SECOND HANDLER that is
    // already running. A thread that faulted, overwrote the global dump, and
    // was killed by the kernel before ever entering here is invisible to this
    // guard, so the normal-path verdict below is "best-effort", not
    // "verified". Nothing in here proves the registers are this thread's.
    const unsigned int concurrentFault =
        __atomic_exchange_n(&g_handlingCrash, 1U, __ATOMIC_RELAXED);
    u64 tpidr = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
    __atomic_store_n(&g_crashFaultingThread, threadGetCurHandle(),
                     __ATOMIC_RELAXED);
    if (concurrentFault != 0) {
        // Still never returns (svcExitProcess below), but write the short
        // record first: a double fault is otherwise completely invisible, and
        // it is the one case where the register block is known to be wrong.
        g_crashLength = 0;
        crashText("RR3 exception-v1\nstatus=concurrent-fault\n");
        crashHex("fault_thread", threadGetCurHandle());
        crashHex("fault_tpidr_el0", tpidr);
        crashHex("first_fault_thread",
                 __atomic_load_n(&g_crashFaultingThread, __ATOMIC_RELAXED));
        crashText("thread_attribution=unverified\n");
        svcOutputDebugString(g_crashText, g_crashLength);
        // TryLock, never Lock: this runs on a kernel exception stack, and the
        // thread holding the lock may itself be the one that is faulting. If the
        // lock is busy the other writer is already recording a termination and
        // the process is going down regardless; parking here would be worse.
        if (g_crashFileOpen && mutexTryLock(&g_crashFileMutex)) {
            fsFileWrite(&g_crashFile, 0, g_crashText, g_crashLength, FsWriteOption_Flush);
            mutexUnlock(&g_crashFileMutex);
        }
        svcExitProcess();
    }
    g_crashLength = 0;
    crashText("RR3 exception-v1\nstatus=exception\n");
    crashHex("handler", reinterpret_cast<uintptr_t>(&__libnx_exception_handler));
    crashHex("error_desc", dump->error_desc);
    crashHex("fault_thread", threadGetCurHandle());
    crashHex("fault_tpidr_el0", tpidr);
    crashHex("pc", dump->pc.x);
    crashHex("lr", dump->lr.x);
    crashHex("sp", dump->sp.x);
    crashHex("fp", dump->fp.x);
    crashHex("far", dump->far.x);
    crashHex("esr", dump->esr);
    crashHex("pstate", dump->pstate);
    for (unsigned int index = 0; index < 29; ++index) {
        char name[] = {'x', static_cast<char>('0' + index / 10),
                       static_cast<char>('0' + index % 10), '\0'};
        crashHex(name, dump->cpu_gprs[index].x);
    }
    crashText("thread_attribution=best-effort-global-dump\n");
    svcOutputDebugString(g_crashText, g_crashLength);
    if (g_crashFileOpen && mutexTryLock(&g_crashFileMutex)) {
        fsFileWrite(&g_crashFile, 0, g_crashText, g_crashLength, FsWriteOption_Flush);
        mutexUnlock(&g_crashFileMutex);
    }
    svcExitProcess();
}

namespace rr3 {

bool prepareGuestCrashCapture() {
    if (g_crashFileOpen) {
        return false;
    }
    FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    if (sd == nullptr) {
        return false;
    }
    char path[128]{};
    std::snprintf(path, sizeof(path), "/switch/rr3-native-rehost.crash-%016llx.txt",
                  static_cast<unsigned long long>(armGetSystemTick()));
    if (R_FAILED(fsFsCreateFile(sd, path, 0, 0)) ||
        R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &g_crashFile))) {
        return false;
    }
    androidSetAbortMessage(nullptr);
    g_crashFileOpen = true;
    constexpr char armed[] = "RR3 termination-v2\nstatus=armed\n";
    if (R_FAILED(fsFileWrite(&g_crashFile, 0, armed, sizeof(armed) - 1, FsWriteOption_Flush))) {
        finishGuestCrashCapture();
        return false;
    }
    return true;
}

void finishGuestCrashCapture() {
    // Guest workers routinely outlive the probe (see guestPthreadQuiesceAll),
    // so this can run while a guest thread is still inside
    // recordGuestTermination. Same mutex, same reason.
    mutexLock(&g_crashFileMutex);
    if (g_crashFileOpen) {
        g_crashFileOpen = false;
        fsFileClose(&g_crashFile);
    }
    mutexUnlock(&g_crashFileMutex);
}

[[noreturn]] void guestTracedAbort() {
    recordGuestTermination("abort", 0, reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::abort();
}

[[noreturn]] void guestTracedExit(int status) {
    recordGuestTermination("exit", static_cast<unsigned long long>(status), reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    std::exit(status);
}

int guestTracedRaise(int signal) {
    recordGuestTermination("raise", static_cast<unsigned long long>(signal), reinterpret_cast<unsigned long long>(__builtin_return_address(0)));
    return std::raise(signal);
}
void recordGuestTermination(const char* kind, unsigned long long value,
                            unsigned long long caller) {
    // Two guest threads reaching abort/raise at the same time both used to read
    // g_crashFileOpen, then write the same g_crashFile handle concurrently and
    // shred each other's record. Serialised on the libnx Mutex above; held
    // across the whole body, including the file write, because the interleaving
    // that matters is two fsFileWrite calls sharing one cursor.
    mutexLock(&g_crashFileMutex);
    if (!g_crashFileOpen) {
        mutexUnlock(&g_crashFileMutex);
        return;
    }
    char text[2048]{};
    size_t size = 0;
    const auto append = [&](const char* input) {
        while (*input != '\0' && size < sizeof(text)) text[size++] = *input++;
    };
    const auto hex = [&](const char* name, unsigned long long number) {
        append(name);
        constexpr char digits[] = "0123456789abcdef";
        for (int shift = 60; shift >= 0; shift -= 4) {
            if (size < sizeof(text)) text[size++] = digits[(number >> shift) & 15];
        }
        append("\n");
    };
    append("RR3 termination-v2\nstatus=termination-request\nkind=");
    append(kind != nullptr ? kind : "unknown");
    append("\n");
    hex("caller=", caller);
    hex("value=", value);
    append("abort_message_begin\n");
    append(guestAbortMessage());
    append("\nabort_message_end\n");
    svcOutputDebugString(text, size);
    while (size < sizeof(text)) text[size++] = ' ';
    text[sizeof(text) - 1] = '\n';
    fsFileWrite(&g_crashFile, 0, text, size, FsWriteOption_Flush);
    mutexUnlock(&g_crashFileMutex);
}


bool recoveryMarkerExists(const char* path) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

bool beginGuardedAction(const char* path, const char* action) {
    return beginGuardedActionSnapshot(path, action, nullptr);
}

bool beginGuardedActionSnapshot(const char* path, const char* action,
                                const char* snapshot) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "pending=%s\n",
                 action != nullptr && action[0] != '\0' ? action : "unknown");
    if (snapshot != nullptr && snapshot[0] != '\0') {
        std::fprintf(file, "%s\n", snapshot);
    }
    std::fprintf(file, "clear_only_after_success=1\n");
    const bool flushed = std::fflush(file) == 0;
    const bool synced = flushed && fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return synced && closed;
}

bool completeGuardedAction(const char* path) {
    if (path == nullptr) {
        return false;
    }
    if (!recoveryMarkerExists(path)) {
        return true;
    }
    return std::remove(path) == 0;
}

// Shared body of the two progress writers. `syncCard` is the only difference
// between them; see the rationale above appendGuestBootLine for why the
// blocked-wait breadcrumb must not pay for a whole-card sync.
//
// This writer is deliberately NOT cached, and its per-call fopen("w") stays.
// The truncate is the contract, not an accident: the header declares this file
// as the "last-reached phase/ordinal" breadcrumb, and readGuestBootProgress
// returns the file from its FIRST byte, so accumulating every write would make
// the reader show the first phase ever written instead of the current one, and
// would destroy exactly the recovery evidence the truncate exists to preserve.
// Only append-mode handles can be cached and reused without loss; a cached "a"
// handle that is closed and reopened still continues at EOF.
//
// So the 282 guest-boot.progress round-trips in the archived run stay. That is
// the honest accounting: 565 of the 911 are removable here, 282 are not.
static bool writeGuestBootProgressImpl(const char* path, const char* phase,
                                       unsigned long long ordinal,
                                       unsigned long long target,
                                       const char* details, bool syncCard) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "phase=%s ordinal=%llu target=%016llx%s%s\n",
                 phase != nullptr && phase[0] != '\0' ? phase : "unknown",
                 ordinal, target, details != nullptr ? " " : "",
                 details != nullptr ? details : "");
    const bool flushed = std::fflush(file) == 0;
    // Short-circuited exactly as before: fsync still runs only after a
    // successful flush, and never at all when syncCard is false.
    const bool durable = !syncCard || fsync(fileno(file)) == 0;
    const bool closed = std::fclose(file) == 0;
    return flushed && durable && closed;
}

bool writeGuestBootProgress(const char* path, const char* phase,
                            unsigned long long ordinal,
                            unsigned long long target,
                            const char* details) {
    return writeGuestBootProgressImpl(path, phase, ordinal, target, details,
                                      true);
}

// Byte-for-byte the same line in the same file, without the whole-card fsync.
// The stage-9 phase checkpoints stay durable; the blocked-wait breadcrumb uses
// this one, because it is the only progress write that can fire twice a second
// per blocked guest thread and therefore the only one that was turning a
// diagnostic into an emulated-SDMC stall. fclose() still flushes to the
// filesystem, so a device run still shows the last blocked primitive; what is
// lost is only durability across a power cut, which a "this thread is stuck"
// breadcrumb does not need.
bool writeGuestBootProgressNoSync(const char* path, const char* phase,
                                 unsigned long long ordinal,
                                 unsigned long long target,
                                 const char* details) {
    return writeGuestBootProgressImpl(path, phase, ordinal, target, details,
                                      false);
}

// The running guest log. Deliberately NOT fsync'd.
//
// fsync() on an emulated SDMC card is a whole-card sync, and this function is
// on the hot path: the census emits ~250 lines per 4 s sweep, i.e. roughly 60
// full card syncs per second, all on the same device the guest is using for
// its own asset reads and writes. Measured consequence: the guest's own file
// I/O inside onViewRenderJNI stalled, and the main thread ended up parked in a
// kernel sync request with its PC unreadable (census-pcburst: 512/512 sentinel
// while the other five threads read ~100% fine). fclose() already flushes the
// bytes to the filesystem; fsync only adds durability across a power loss,
// which a diagnostic breadcrumb log does not need.
//
// The durability that DOES matter is kept: writeRecoveryMarker() and
// writeGuestBootProgress() below still fsync, because the recovery marker is
// what gates the next launch after a crash.
//
// Cached handle, one fopen("a") per path instead of one per line. Same libnx
// Mutex class of primitive as g_crashFileMutex above, for the same reason: a
// std::mutex on a guest-visible path starved the emulator's scheduler and
// burned 187 s of CPU (measured, noted above g_crashFileMutex). The lock is
// held across the lookup, the fwrite and the flush, so two threads reaching
// this from the main thread, from a pthread_bridge guest worker or from a raw
// threadCreate worker (samplerWorker, alertPumpWorker) can never share one
// stdio cursor.
//
// Why a libnx Mutex and not an atomic spinlock: the spinlock is genuinely one
// cheaper atomic, but its worst case is a holder descheduled by the guest while
// every other thread burns its core spinning. This file already has a measured
// CPU-climb pathology caused by exactly that shape of problem (see the census
// host-CPU numbers quoted in pthread_bridge.cpp:329), and the Switch is
// multi-core with guest workers that get preempted. An uncontended libnx
// Mutex is a single uncontended atomic too; it just does not spin. The saving
// from spinning is not worth an unbounded worst case on a hot path.
//
// Accepted interleaving: none WITHIN a line. Line ORDER across threads is
// still nondeterministic, exactly as it was when each caller had its own
// O_APPEND handle. A line is either written whole or dropped whole; the one
// visible regression is that a line whose fflush fails is now LOST (see the
// error policy below) where before it survived in a handle that fclose might
// still flush. That is the intended trade for a diagnostic log: a dropped
// line is visible as a gap, a torn line is not recoverable at all.
//
// Reopen policy: the cached handle is dropped on ferror() or a failed fflush,
// which is also what happens when the SD card is pulled out from under an open
// handle. The slot keeps the path with file == nullptr, so the NEXT call
// reopens and this one returns false. The function can therefore never latch
// into returning false forever, and a card that comes back recovers on the
// next line. While a card is genuinely gone this costs one failed fopen per
// line -- the old behaviour, not worse.
bool appendGuestBootLine(const char* path, const char* phase,
                         unsigned long long ordinal,
                         unsigned long long target,
                         const char* details) {
    if (path == nullptr) {
        return false;
    }
    // A path longer than the table can hold is not cached at all; it falls
    // through to the old one-shot open/close so it can never be truncated.
    if (std::strlen(path) >= kGuestLogPathMax) {
        std::FILE* once = std::fopen(path, "a");
        if (once == nullptr) {
            return false;
        }
        std::fprintf(once, "phase=%s ordinal=%llu target=%016llx%s%s\n",
                     phase != nullptr && phase[0] != '\0' ? phase : "unknown",
                     ordinal, target, details != nullptr ? " " : "",
                     details != nullptr ? details : "");
        const bool onceFlushed = std::fflush(once) == 0;
        const bool onceClosed = std::fclose(once) == 0;
        return onceFlushed && onceClosed;
    }
    mutexLock(&g_logCacheMutex);
    GuestLogCacheSlot* slot = logCacheSlotLocked(path);
    if (slot->file == nullptr) {
        slot->file = std::fopen(path, "a");
        if (slot->file == nullptr) {
            mutexUnlock(&g_logCacheMutex);
            return false;
        }
        g_logCacheOpens = __atomic_add_fetch(&g_logCacheOpens, 1ULL, __ATOMIC_RELAXED);
        // One startup line, in the same format, in the same file, written on the
        // first line that gets a handle. It is the ONLY line this function adds
        // on its own account: per-line self-logging is what is being fixed here.
        if (__atomic_exchange_n(&g_logCacheAnnounced, 1U, __ATOMIC_RELAXED) == 0) {
            std::fprintf(slot->file,
                         "phase=append-log-cache ordinal=%llu target=%016llx "
                         "slots=%u mode=a lock=libnx-mutex w-mode=uncached "
                         "reopen=ferror-or-fflush-fail\n",
                         0ULL, 0ULL,
                         static_cast<unsigned int>(kGuestLogCacheSlots));
            std::fflush(slot->file);
        }
    } else {
        g_logCacheReuses = __atomic_add_fetch(&g_logCacheReuses, 1ULL, __ATOMIC_RELAXED);
    }
    std::FILE* file = slot->file;
    std::fprintf(file, "phase=%s ordinal=%llu target=%016llx%s%s\n",
                 phase != nullptr && phase[0] != '\0' ? phase : "unknown",
                 ordinal, target, details != nullptr ? " " : "",
                 details != nullptr ? details : "");
    const bool flushed = std::fflush(file) == 0;
    bool ok = flushed;
    if (!ok || std::ferror(file) != 0) {
        // Still no fsync: reopening is a fresh fopen, not a card sync.
        std::fclose(file);
        slot->file = nullptr;
        g_logCacheReopens =
            __atomic_add_fetch(&g_logCacheReopens, 1ULL, __ATOMIC_RELAXED);
        ok = false;
    }
    mutexUnlock(&g_logCacheMutex);
    return ok;
}

void guestBootLogCacheStats(unsigned long long& opens, unsigned long long& reuses,
                            unsigned long long& reopens,
                            unsigned int& slots) {
    // Intended caller is the existing periodic census/sweep point in
    // pthread_bridge.cpp, not a new timer: read the counters there and drop the
    // reuse/open ratio into the census-sweep details string. Snapshot is not
    // mutually consistent with itself under concurrency, which is fine for a
    // monotonic-shaped counter triple.
    opens = __atomic_load_n(&g_logCacheOpens, __ATOMIC_RELAXED);
    reuses = __atomic_load_n(&g_logCacheReuses, __ATOMIC_RELAXED);
    reopens = __atomic_load_n(&g_logCacheReopens, __ATOMIC_RELAXED);
    slots = static_cast<unsigned int>(kGuestLogCacheSlots);
}

bool readGuestBootProgress(const char* path, char* out, unsigned long capacity) {
    if (out != nullptr && capacity > 0) {
        out[0] = '\0';
    }
    if (path == nullptr || out == nullptr || capacity == 0) {
        return false;
    }
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    const size_t read = std::fread(out, 1, capacity - 1, file);
    out[read] = '\0';
    std::fclose(file);
    // Trim a single trailing newline so the string prints cleanly in the UI.
    for (size_t index = read; index > 0; --index) {
        if (out[index - 1] == '\n' || out[index - 1] == '\r') {
            out[index - 1] = '\0';
        } else {
            break;
        }
    }
    return read > 0;
}

void clearGuestBootProgress(const char* path) {
    if (path != nullptr && recoveryMarkerExists(path)) {
        std::remove(path);
    }
}

} // namespace rr3
