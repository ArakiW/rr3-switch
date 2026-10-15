#include "rr3/pthread_bridge.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/root_pc_sampler.hpp"
#include "rr3/tls_probe.hpp"
#include "rr3/guest_view_trace.hpp"
#include "rr3/elf_preflight.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <switch.h>
#include <unistd.h>

namespace rr3 {

// Forward declaration so guestPthreadCreate can tell the bridge's own reaper
// (a host thread) apart from a real guest start routine.
void* reapDetachedThreadWorker(void*);

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
    // Wait/notify ledger. The interesting number is waits - (signals +
    // broadcasts): a waiter that was never signalled is the whole question
    // this instrumentation exists to answer, so it is tracked per object
    // instead of being inferred from a global import tally.
    uint64_t waits{};
    uint64_t signals{};
    const void* lastWaitReturn{};
    uint32_t waitersPeak{};
    // Mutex ownership ledger. Populated only for gMutexes, and deliberately
    // kept in the HOST side table rather than read out of the guest struct:
    // the bridge never interprets guest pthread_mutex_t bytes, and it still
    // does not here. Without this, a mutex whose owner thread died holding it
    // is indistinguishable from one that is merely busy, and every waiter on
    // it spins or parks forever with nothing in the log to say why.
    //
    // ownerKey is censusKey() (TPIDRRO_EL0): stable for the whole guest
    // thread's life, readable from the exit path, unique per live host thread.
    // ownerOrdinal is a write-once copy so a report still names the thread
    // after its census entry has been recycled.
    uintptr_t ownerKey{};
    uint32_t ownerOrdinal{};
    const void* acquireSite{};
    uint64_t acquireTick{};
    uint64_t acquires{};
    // Real contention, accumulated only inside the EBUSY retry loop of
    // guestPthreadMutexLock. `waits` counts REQUESTS -- noteMutexWait runs
    // unconditionally on entry -- so it is not a contention measure and must
    // never be read as one.
    uint64_t blockedSpins{};
    uint64_t worstSpins{};
    bool held{};
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

// ---------------------------------------------------------------------------
// Per-guest-thread activity census.
//
// The question this answers is "which guest thread stops making progress".
// Every guest pthread the bridge starts gets a counter that is bumped from the
// intercepted imports the bridge already owns, plus its start-routine address
// (seen at creation) and the last import it reached. A ~4 s sweep prints the
// whole table, so a thread whose counter stops climbing while another's keeps
// going is the frozen one.
//
// Identity: guest threads run inside GuestTlsScope, which overwrites
// TPIDR_EL0 with a fake Android TLS block for their whole life. A host
// thread_local would read that fake block and a write would corrupt the guest
// canary at +0x28, so the per-thread key is TPIDRRO_EL0 -- the register
// libnx's own armGetTls() reads, which GuestTlsScope never writes and which is
// unique per live host thread.
// ---------------------------------------------------------------------------

constexpr size_t kCensusCapacity = 256;
constexpr size_t kCensusFastSets = 512;
// The 8-slot ring only ever showed the last four libc++ guard calls, which is
// not enough to see the decision that stopped the thread. 512 entries covers a
// whole import burst, and the ordered dump below prints it symbol-first.
constexpr size_t kCensusReturnRing = 512;
// Short tail kept in the sweep snapshot; the full history stays in the live
// per-thread entry so the BSS cost is one 512-entry ring, not two.
constexpr size_t kCensusTail = 8;
constexpr uint64_t kCensusSweepSeconds = 4;
constexpr uint64_t kCensusStallSeconds = 8;
// PC histogram slots per thread. Deliberately tiny: the question is "is this
// thread executing guest code, and if so where", not a profile.
constexpr size_t kCensusPcSlots = 4;
// pc values below this are not code. The existing whole-process sampler reads
// back 0x4 on Eden for every foreign thread, so a small pc is the platform's
// "no usable context" answer, not a guest address.
constexpr uint64_t kCensusPcSentinel = 0x1000;
// The PC verdict is stable once a thread has stopped moving, so it is
// published rarely rather than every sweep.
//
// The cadence is a MEASURED wall clock, not a pass count, and the reason is
// that a pass is not a constant. censusReporterWorker (below) sleeps
// svcSleepThread(500000000LL) per iteration, so 500 ms is a FLOOR on the pass
// period, not the period: the iteration also pauses every live guest thread
// and reads its context, and for the root thread it additionally runs a
// 512-sample censusPcBurst.
//
// The pass body is now MEASURED rather than assumed. On
// 20261004-010009-fpwalk-idle, ordinal 4 reported idle_ms=960767920499, which
// decodes to exactly 2^64 - 128751616 ticks at the 19.2 MHz Switch counter
// (censusMillis is ticks*1000/freq, and the multiply does not overflow at that
// magnitude, so the value is the wrapped subtraction itself). That is the
// thread's lastTick sitting 6.706 s AHEAD of the pass's nowTick -- see the
// idleMs comment in censusSampleThreadPcs -- so one pass body spans at least
// 6.7 s of wall clock. Most of that is the per-thread
// svcSetThreadActivity(Paused)/svcGetThreadContext3/Runnable round trip
// against threads that are running guest code on this emulator, which is a
// scheduler cost and not a tunable literal: one pause per thread per pass is
// already the minimum, so there is nothing to trim on the plain pass. A pass
// that also fires a walk adds censusPcBurst's 512 iterations, i.e. 1536 more
// syscalls against the ROOT thread, and that is the one dial left if the
// cadence still proves too slow (kCensusPcBurstSamples 512 -> 64 keeps the
// live/sentinel split and costs 1/8 as much). It is not needed to clear the
// bar below, so it is not touched.
//
// With a ~6.7 s body the real period is ~7.2 s, so 150 s buys ~20 passes --
// which is what pcStable >= 3 needs and 18x what the archived run managed.
// kCensusPcPublishMs = two sweep periods, so a census-pc line lands at most
// once per 8 s of real time no matter how slow the passes are. The pass-count
// term is kept ONLY as an upper bound for the opposite failure (passes so fast
// that ms rounding never fires); whichever of the two comes first wins.
constexpr uint64_t kCensusPcPublishMs = 8000;
constexpr unsigned kCensusPcPublishEvery = 8;

// ---------------------------------------------------------------------------
// Per-round thread-PC report.
//
// Why a second report instead of widening the census-pc histogram above: the
// histogram folds a thread's pc into a kCensusPcSlots-wide LIFETIME ring. Once
// four distinct addresses are in it, a later pc has no free slot and is
// dropped on the floor (entry.pcTop is only appended to while a slot is
// empty), and the fixed stride hides everything between publishes. That shape
// answers "which host address does this thread spend its life at", which is
// what came back -- and it cannot answer "where is the thread that is
// faulting 947k times a second". So the CURRENT pc and lr are reported per
// thread per pass, in full, never deduplicated, and the publish gate below
// gives a guest-image hit priority over the fixed stride.
//
//   kPcGuestBudget  guest-image lines for one thread in a whole run
//   kPcGuestStride  fixed gap between two of them, in PASSES
//
// The gap used to GROW with every line already printed -- max(2, lines), so
// 24 lines landed on passes 1,3,6,10,... That is a doubling schedule, and its
// arithmetic assumed 0.5 s/pass, which the archive contradicts: at the real
// cadence the gap outruns the run and the thread publishes a handful of lines
// and then never again. A fixed stride has no arithmetic to get wrong. It is
// 4 passes, so a thread gets its 24-line budget spread evenly and the
// counter can actually accumulate. After the budget is spent the thread keeps
// its kCensusPcPublishEvery line for the rest of the run, so no thread ever
// goes permanently silent.
// ---------------------------------------------------------------------------
constexpr uint32_t kPcGuestBudget = 24;
constexpr uint64_t kPcGuestStride = 4;

// The spin verdict, as a number and not a vibe.
//
//   spinning=yes    pc read ok, pc inside a guest image, the pc has been
//                   BIT-IDENTICAL across >= kSpinStablePasses consecutive
//                   passes, and the thread's bridge-import count did not move
//                   between the last two passes.
//   spinning=no     pc read ok, pc inside a guest image, pc_stable < threshold
//   spinning=unset  pc read failed, pc is the platform sentinel, or the pc is
//                   not inside any guest image -- in which case there is
//                   nothing to have a verdict about
//
// 6 passes is 5 intervals at ONE guest instruction with no bridge traffic.
// The wall time of that is NOT 2.5 s: see kCensusPcPublishMs -- a pass is a
// 500 ms sleep floor plus the pass body, so the real figure is several
// intervals longer. Both halves are needed: pc alone would flag a thread
// parked inside a guest wait loop, and imports alone would miss a thread
// spinning purely in guest code, which is exactly the shape being hunted (an
// in-place self-move of guest .rodata text touches no imported symbol).
constexpr uint32_t kSpinStablePasses = 6;

// On Eden this verdict is STRUCTURALLY UNREACHABLE, and that is a platform
// fact, not a tuning problem: spinning=yes requires censusIsGuestImage(pcRc),
// and 5 of 5 samples in 20261004-005039-percensus classified pc_img=host-nro.
// Eden's svcGetThreadContext3 hands back the host NRO's pc for a guest thread,
// so `spinning=unset` is the honest and permanent answer here. The rule is
// kept unchanged and correct so it means something again on a platform that
// reports a real pc -- it is the trigger, not the fix, that had to move.
// Frame walks for flagged threads, per thread and for the whole run. A walk is
// NOT ~40 log lines: censusFpWalk emits censusRegisterDump (up to 68: 4 framing
// + 29 reg-class + up to 34 deref + 1 deref-end) plus censusGuardScan (up to
// ~120: 48 stuck + 64 live + framing) plus the chain itself, so budget a walk
// at ~200 lines. It runs only AFTER the thread has been resumed, so it can
// never extend a pause, and both caps bound it no matter how many threads flag.
constexpr uint32_t kSpinWalkBudget = 3;
constexpr unsigned kSpinWalkGlobal = 8;

// ---------------------------------------------------------------------------
// Stall walk: the frame chain for a thread whose pc the platform will not give
// us. This is the replacement trigger for the unreachable spin rule.
//
// Why the spin rule could not do this job: it required censusIsGuestImage(pc),
// and on Eden every guest thread's pc classifies host-nro, so `spinning` was
// permanently unset and the walk it gated never ran -- once, in 150 s. The
// chain, though, was always reachable, because the x29 walk reads the STACK
// (which the same context read returns correctly) and not the pc.
//
// The predicate, as numbers, all four required:
//   1. ctxOk && !sentinel    the register read succeeded, so there is a real
//                            context and a real sp/fp/lr to walk
//   2. importsDelta == 0     no bridge-import progress since the previous pass
//   3. pcStable >= 3         the pc has been BIT-IDENTICAL for 3 consecutive
//                            passes, so the thread is parked and its stack
//                            is not moving under the read
//   4. idleMs >= 50          no import for 50 ms, in measured wall clock
//
// 2, 3 and 4 are the honest substitute for the guest-pc half of the old rule:
// a parked thread is characterised by NOT MOVING, and the pc is a perfectly
// good movement witness whether it lives in the NRO or in a guest image. 1
// is the only real guard -- a failed read has no stack to walk.
//
// Being straight about what this does NOT discriminate: on this platform
// every interesting thread and every idle thread satisfies all four. The five
// measured lines (ordinals 0,1,2,4,5) all have imports_delta=0, idle_ms
// 60..503 and a repeated pc. Nothing available distinguishes them, so the
// bounded answer is used instead of a pretend-discriminating one: walk them
// ALL, once each, under a hard global cap. With 4 non-root guest threads in
// the archived run that is 4 walks and every one of them gets its chain.
//
// The cap is deliberately tight. A walk is not ~40 lines: censusRegisterDump
// alone emits up to 68 (30 reg-class + 34 deref + 4 framing) and
// censusGuardScan up to ~120 (48 stuck + 64 live + framing), so budget a walk
// at ~200 lines and treat kStallWalkGlobal as the only dial that moves the
// volume. One walk per thread is the point: the chain is what is missing, and
// the second sample of an unchanged chain is not worth 200 lines.
constexpr uint32_t kStallWalkStablePasses = 3;
constexpr uint64_t kStallWalkIdleMs = 50;
constexpr uint32_t kStallWalkBudget = 1;   // per thread, whole run
constexpr unsigned kStallWalkGlobal = 4;   // whole run
// captureGuestStartupPlan() is NOT free -- it appends one guest-view log line
// (fopen/fprintf/fsync/fclose) on every call. The pass needs the plan once,
// not once per thread, so it is cached and refreshed on this cadence: 60
// passes. The "= 30 s" this comment used to claim assumed 0.5 s/pass and is
// not worth restating as a number now that the pass period is known to vary;
// what is bounded is the CALL COUNT, which is what the fsync cost follows:
// 1 call per pass, plus one per refresh, and never one per thread. The images
// do not move once the rebase is done, so a cached copy is not a stale one.
//
// "never one per thread" used to be true of the PASS and false of the WALK:
// censusFpWalk, censusRegisterDump, censusGuardScan and censusPcBurst all
// called captureGuestStartupPlan() directly, so one frame walk cost three
// fsyncs of its own on top of the pass's, and censusDumpRing and
// censusLibcxxSyncProbe still do (censusDumpRing runs from censusSweep, which
// can run on a guest import thread, and the cache is a function-local static
// with no lock -- routing that through the cache would be a data race, not an
// optimisation). The four walk-path sites now read the cache, which the same
// pass has already primed. With kStallWalkGlobal (4) plus kSpinWalkGlobal (8)
// plus kFpWalkRepeats (3) that was up to 45 fsyncs on the emulated SD card for
// walks that are supposed to be cheap.
constexpr uint64_t kPlanRefreshPasses = 60;

// A thread that stopped advancing is dumped once, in full, instead of adding
// 512 addresses to every 4 s sweep line for the rest of the run. The bound is
// on the TAIL (see censusDumpRing): the leading history is hundreds of
// identical mutex pairs, and the freeze is at the end.
constexpr size_t kCensusDumpLines = 220;
// A thread past this lifetime import count has its ring dumped once even if it
// never stalls. Real guest work does not reach this in a short run; a spin does.
constexpr uint64_t kCensusSpinRingImports = 250000;

struct CensusEntry {
    uintptr_t key{};
    uintptr_t startRoutine{};
    uint64_t creatorOrdinal{};
    uint64_t imports{};
    uint64_t firstTick{};
    uint64_t lastTick{};
    uint64_t reportedImports{};
    const void* lastReturn{};
    // In-flight bridge breadcrumb. Depth is incremented on entry to a bridge
    // import and decremented on exit, so a non-zero depth at sweep time means
    // the thread is parked INSIDE this bridge. A zero depth alongside a
    // non-zero import count means every bridge call the thread ever made
    // returned, i.e. it is executing guest code and this bridge is not
    // holding it.
    uint32_t bridgeDepth{};
    // Monotonic count of bridge entries for this thread. Paired with
    // bridgeDepth this is a self-checking control: a non-zero enters with
    // depth==0 proves the enter path really did fire for this thread and that
    // every call returned, so a zero depth is a measurement and not a dead
    // detector.
    uint64_t bridgeEnters{};
    const char* bridgeSymbol{};
    uintptr_t recentReturns[kCensusReturnRing]{};
    const char* recentSymbols[kCensusReturnRing]{};
    const char* lastSymbol{};
    uint32_t ordinal{};
    uint32_t recentCount{};
    bool used{};
    bool attached{};
    bool root{};
    bool exited{};
    bool host{};
    bool stallReported{};
    bool watchNextImport{};
    bool ringDumped{};
    // Per-thread PC classification. Each thread records its own kernel handle
    // at attach time (threadGetCurHandle), which lets the reporter pause it and
    // read its context directly -- libnx's threadPause/threadResume need a
    // Thread* that only exists for threads this NRO created, and every guest
    // thread here is a bare pthread.
    //
    // The sentinel count is a first-class result, not a failure: it separates
    // "executing guest code at pc X" from "this platform does not hand out
    // another thread's pc". On Eden the whole-process sampler reads back 0x4
    // for 1272 of 1280 samples, so that distinction is the whole experiment.
    uint32_t kernelHandle{};
    uint32_t pcSamples{};
    uint32_t pcSentinel{};
    uint32_t pcPauseFail{};
    uint32_t pcCtxFail{};
    uint32_t pcTop[kCensusPcSlots]{};
    uint32_t pcTopHits[kCensusPcSlots]{};
    // Per-round pc state for the current-pc report above. Deliberately 64-bit
    // where the value IS an address: a guest pc is 0x2107...... and the
    // 32-bit pcTop ring above truncates that, which is why the current pc is
    // reported from these and not out of the ring.
    uint64_t pcPrev{};        // pc seen on the previous pass
    uint32_t pcStable{};      // consecutive passes with an identical pc
    uint64_t pcLastImports{}; // entry.imports at the previous pass
    uint64_t pcLastGuestPass{};  // pass that last published a guest-image line
    uint32_t pcGuestLines{};  // guest-image lines published (vs kPcGuestBudget)
    uint32_t pcSpinWalks{};   // spin walks spent (vs kSpinWalkBudget)
    // Publish bookkeeping for the measured-cadence gate. Both are needed
    // because the pass COUNT and the wall clock disagree: the tick is what
    // actually bounds the log when passes are slow, the pass delta bounds it
    // when they are fast. Appended at the END of the pc block on purpose --
    // censusNoteImportFrom is the ~265k calls/second path and only ever reads
    // imports/lastSymbol/lastTick/lastReturn/recent*, so growing the struct
    // here leaves every hot field at its current offset.
    uint64_t pcLastPublishTick{};   // censusTick() at the last published line
    uint64_t pcLastPublishPass{};   // pass index at the last published line
    uint32_t pcStallWalks{};        // stall walks spent (vs kStallWalkBudget)
    // The same two fields, for the LINK REGISTER.
    //
    // Why this exists, in one line: the pc of a thread blocked in host or
    // kernel code lands in no image we know, so it reads as the sentinel and
    // pcStable resets to 0 every pass -- which is exactly the thread the
    // stall walk below exists to look at. The lr does not have that problem:
    // in run 20261009-204518-destructorgate, 2317 of 3101 lr_ring tokens
    // (74.7%) resolved inside libRealRacing3 while the pc field for the same
    // threads read pc=0x4/pc_img=sentinel. A stable, guest-image lr across
    // passes is therefore usable evidence that a thread is parked at one
    // guest call site, and it is the only field that is.
    //
    // Appended here for the same reason as pcLastPublishTick above: everything
    // censusNoteImportFrom touches keeps its offset.
    uint64_t lrPrev{};        // lr seen on the previous pass
    uint32_t lrStable{};      // consecutive passes with an identical lr
    // Repeats a few times, spaced out, rather than once. One shot is not
    // enough: the main thread also stalls briefly during boot (a run caught it
    // mid-filesystem, with x28 holding the path string "smdc:/w..."), and a
    // one-shot latch burns its single sample on whichever stall comes first.
    // The frame-2 wedge is permanent, so a later sample is guaranteed to land
    // on it. Three samples 15 s apart is ~110 lines for the whole run.
    uint32_t fpwalkCount{};
    uint64_t fpwalkTick{};
};

struct CensusFastSlot {
    uintptr_t key{};
    CensusEntry* entry{};
};

struct CensusSnapshot {
    uintptr_t startRoutine{};
    uint64_t creatorOrdinal{};
    uint64_t imports{};
    uint64_t delta{};
    uint64_t firstTick{};
    uint64_t lastTick{};
    // Deliberately no ring copy: the live CensusEntry already holds the full
    // 512-entry history, and duplicating it for all 256 census slots would
    // cost megabytes of BSS for no extra information. censusDumpRing reads the
    // live ring under gCensusLock instead.
    uintptr_t recentReturns[kCensusTail]{};
    const char* recentSymbols[kCensusTail]{};
    const char* lastSymbol{};
    uint32_t ordinal{};
    uint32_t recentCount{};
    uint32_t bridgeDepth{};
    uint64_t bridgeEnters{};
    const char* bridgeSymbol{};
    bool root{};
    bool exited{};
    bool host{};
    bool ringDumped{};
};

constexpr const char* kCensusLogPath = "sdmc:/switch/rr3-native-rehost.guest-view.log";

CensusEntry gCensus[kCensusCapacity]{};
CensusFastSlot gCensusFast[kCensusFastSets]{};
CensusSnapshot gCensusSnapshot[kCensusCapacity]{};
Mutex gCensusLock{};
Mutex gCensusSnapLock{};
Mutex gCensusLogLock{};
uint64_t gCensusTickFreq{};
uint64_t gCensusSweepTick{};
uint64_t gCensusFirstTick{};
unsigned int gCensusSweeps{};
unsigned int gCensusReported{};
uint32_t gCensusNextOrdinal{};
unsigned gCensusPcPass{};
unsigned gCensusSpinWalks{};   // whole-run cap for flagged-thread walks
unsigned gCensusStallWalks{};  // whole-run cap for the pc-free stall walk
bool gCensusArmed{};
bool gCensusReporterStarted{};

uint64_t censusTick() {
    return armGetSystemTick();
}

uint64_t censusTickFreq() {
    if (gCensusTickFreq == 0) {
        gCensusTickFreq = armGetSystemTickFreq();
    }
    return gCensusTickFreq != 0 ? gCensusTickFreq : 1;
}

uint64_t censusMillis(uint64_t ticks) {
    return (ticks * 1000ULL) / censusTickFreq();
}

uintptr_t censusKey() {
    // TPIDRRO_EL0 survives GuestTlsScope; TPIDR_EL0 does not.
    uintptr_t value{};
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}

bool censusInsideGuestScope() {
    // GuestTlsScope's block is a 0x10000 stack array, so a live guest scope
    // always has TPIDR_EL0 inside the caller's own stack frame range.
    uintptr_t tp{};
    uintptr_t sp{};
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    return tp >= sp && tp < sp + 0x20000;
}

CensusEntry* censusFindSlow(uintptr_t key) {
    for (CensusEntry& entry : gCensus) {
        if (entry.used && entry.key == key) {
            return &entry;
        }
    }
    return nullptr;
}

CensusEntry* censusLookup(uintptr_t key) {
    const CensusFastSlot& slot = gCensusFast[(key >> 6) & (kCensusFastSets - 1)];
    return slot.key == key ? slot.entry : censusFindSlow(key);
}

// ---------------------------------------------------------------------------
// Batched log writes.
//
// appendGuestBootLine() costs one fopen("a") + fflush + fsync + fclose per line.
// The census used to call it once per "census-ord" and up to kCensusDumpLines
// (220) times per ring dump, so a 4 s sweep could issue ~250 fsyncs at the
// guest-view log on the SD card. That is our own diagnostic, not the game, and
// it was large enough to show up as a linear host-CPU climb over a 120 s run
// (13.7 s -> 80.7 s of CPU) with the log growing 48 KB -> 143 KB in lockstep.
//
// The batch formats the identical bytes into one buffer and pays for a single
// open/fsync/close per sweep instead. Lock order is unchanged and still
// gCensusSnapLock > gCensusLock > gCensusLogLock, so batching adds no cycle.
//
// The only threads that log outside a sweep are the per-thread lifecycle
// breadcrumbs (bind/start/exit/stall/watch), which are one-shot and rare, so
// they still write through directly and keep their prompt-flush behaviour.
// ---------------------------------------------------------------------------
constexpr size_t kCensusBatchBytes = 32768;
char gCensusBatch[kCensusBatchBytes];
size_t gCensusBatchLength{};
unsigned gCensusBatchDepth{};
// When the batch was opened, so an UNWOUND scope can be detected. See
// censusLog.
uint64_t gCensusBatchOpenedTick{};
// A batch that is still open this long after it was opened is not being
// written by a thread that is going to return. censusLog spills it anyway (see
// below), so a stuck sweep costs one extra fsync per second instead of
// costing the run every census line it would have produced. One second is far
// below the 4 s sweep period, so a healthy run never trips it: the batch is
// closed by its own scope long before.
constexpr uint64_t kCensusBatchMaxOpenMs = 1000;

void censusWriteRaw(const char* bytes, size_t length) {
    if (length == 0) {
        return;
    }
    std::FILE* file = std::fopen(kCensusLogPath, "a");
    if (file == nullptr) {
        return;
    }
    std::fwrite(bytes, 1, length, file);
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);
}

// Caller holds gCensusLogLock.
void censusFlushLocked() {
    if (gCensusBatchLength == 0) {
        return;
    }
    const size_t length = gCensusBatchLength;
    gCensusBatchLength = 0;
    censusWriteRaw(gCensusBatch, length);
}

// Caller holds gCensusLogLock. Flushing on overflow keeps a pathologically
// large sweep correct at the cost of extra writes, which is the right trade:
// the bounded ring summary below means a normal sweep never comes close.
void censusAppendLocked(const char* bytes, size_t length) {
    if (length + gCensusBatchLength > sizeof(gCensusBatch)) {
        censusFlushLocked();
    }
    if (length > sizeof(gCensusBatch)) {
        censusWriteRaw(bytes, length);
        return;
    }
    std::memcpy(gCensusBatch + gCensusBatchLength, bytes, length);
    gCensusBatchLength += length;
}

// RAII so no early return in a sweep can leave the batch open and swallow every
// later census line.
struct CensusBatchScope {
    CensusBatchScope() {
        mutexLock(&gCensusLogLock);
        if (gCensusBatchDepth == 0) {
            gCensusBatchOpenedTick = censusTick();
        }
        ++gCensusBatchDepth;
        mutexUnlock(&gCensusLogLock);
    }
    ~CensusBatchScope() {
        mutexLock(&gCensusLogLock);
        if (gCensusBatchDepth > 0) {
            --gCensusBatchDepth;
        }
        if (gCensusBatchDepth == 0) {
            censusFlushLocked();
        }
        mutexUnlock(&gCensusLogLock);
    }
    CensusBatchScope(const CensusBatchScope&) = delete;
    CensusBatchScope& operator=(const CensusBatchScope&) = delete;
};

void censusLog(const char* phase, unsigned long long ordinal,
               unsigned long long target, const char* details) {
    // appendGuestBootLine is an unsynchronised fopen/fprintf/fsync, so census
    // lines from concurrent threads interleave without this.
    // The line bytes are formatted here exactly as appendGuestBootLine formats
    // them, so the on-disk format is unchanged; only the number of opens and
    // fsyncs changed. See the batch note above censusWriteRaw.
    char line[640];
    const int written = __builtin_snprintf(
        line, sizeof(line), "phase=%s ordinal=%llu target=%016llx%s%s\n",
        phase != nullptr && phase[0] != '\0' ? phase : "unknown",
        ordinal, target, details != nullptr ? " " : "",
        details != nullptr ? details : "");
    if (written <= 0) {
        return;
    }
    const size_t length =
        static_cast<size_t>(written) < sizeof(line)
            ? static_cast<size_t>(written) : sizeof(line) - 1;
    mutexLock(&gCensusLogLock);
    if (gCensusBatchDepth > 0) {
        censusAppendLocked(line, length);
        // The batch is open, so the bytes above went to RAM. Normally the owning
        // scope closes and flushes them within the same pass. If that scope's
        // thread is stuck -- which is exactly what a wedged guest thread inside
        // censusSweep is -- the depth counter never returns to 0 and EVERY
        // later census line, from every thread, is appended to a buffer that is
        // only ever written when it overflows 32 KB. That is not a crash, it is
        // worse: the census goes completely silent while the process looks
        // healthy, and the run ends with no lines and no explanation. It is why
        // 20261004-010009-fpwalk-idle shows 60003 bytes frozen from t=10 s with
        // not one census-sweep line in 150 s.
        //
        // So the open window is bounded, and on expiry the buffer is spilled
        // rather than trusted. Order is preserved (the line is appended first,
        // then everything is written in one go), the stuck scope keeps its
        // depth, and the next line still lands on disk. The cost is one mrs per
        // census line -- censusLog is not on the import hot path.
        if (censusMillis(censusTick() - gCensusBatchOpenedTick)
            >= kCensusBatchMaxOpenMs) {
            censusFlushLocked();
        }
    }
    else {
        censusWriteRaw(line, length);
    }
    mutexUnlock(&gCensusLogLock);
}

CensusEntry* censusReserveLocked() {
    for (CensusEntry& entry : gCensus) {
        if (!entry.used) {
            entry = CensusEntry{};
            entry.used = true;
            entry.lastSymbol = "(none)";
            entry.ordinal = gCensusNextOrdinal++;
            return &entry;
        }
    }
    return nullptr;
}

void censusPublish(uintptr_t key, CensusEntry* entry) {
    gCensusFast[(key >> 6) & (kCensusFastSets - 1)] = CensusFastSlot{key, entry};
}

// Binds the calling thread if it is not tracked yet. Every thread is recorded,
// including host ones: a host thread that keeps importing is useful evidence
// that the census itself is still alive and the log is still being written.
CensusEntry* censusBindCaller(const char* role) {
    const uintptr_t key = censusKey();
    CensusEntry* existing = censusLookup(key);
    if (existing != nullptr) {
        return existing;
    }
    const bool inScope = censusInsideGuestScope();
    mutexLock(&gCensusLock);
    CensusEntry* entry = censusReserveLocked();
    if (entry != nullptr) {
        entry->key = key;
        entry->attached = true;
        entry->host = !inScope;
        entry->root = true;
        entry->firstTick = censusTick();
        entry->lastTick = entry->firstTick;
        // Read on the thread itself: the handle is per-thread, and asking any
        // other thread would just return that other thread's handle.
        entry->kernelHandle = static_cast<uint32_t>(threadGetCurHandle());
        censusPublish(key, entry);
    }
    mutexUnlock(&gCensusLock);
    if (entry == nullptr) {
        return nullptr;
    }
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "role=%s key=0x%llx in_guest_scope=%d start=0x0",
                       role, (unsigned long long)key, inScope ? 1 : 0);
    censusLog( "census-bind", entry->ordinal,
                        (unsigned long long)key, details);
    return entry;
}

CensusEntry* censusReserveChild(uintptr_t start, uintptr_t argument,
                                uint64_t creatorOrdinal, bool hostOwned,
                                uintptr_t creatorReturn) {
    if (!gCensusArmed) {
        return nullptr;
    }
    mutexLock(&gCensusLock);
    CensusEntry* entry = censusReserveLocked();
    if (entry != nullptr) {
        entry->startRoutine = start;
        entry->creatorOrdinal = creatorOrdinal;
    }
    mutexUnlock(&gCensusLock);
    if (entry == nullptr) {
        return nullptr;
    }
    // creatorReturn is the link register of whoever called into the guest's
    // pthread_create binding. It is captured by guestPthreadCreate itself and
    // passed in: read one frame deeper, the link register would only name
    // censusReserveChild and the field would be a host address, which is the
    // trap that made the earlier register probes useless.
    //
    // The guest's PLT slot for pthread_create is bound straight to
    // guestPthreadCreate (RR3_SHIM in symbol_resolver.cpp), so for a thread
    // created by guest code this is the guest's own call site and no C++
    // trampoline sits in between. Host creators (the bridge reaper, the TLS
    // self-test) legitimately report a host address here and land in
    // lr_img=host, which is why the raw form is kept next to the mapped one.
    //
    // The mapping is the load-bias route the sampler histogram and the JNI gate
    // lines already use, not a second one: guest VA = loadBias + ELF VA, and
    // only the load-bias form is the address Ghidra and the known anchors speak
    // in. resolveMainImageLoadBias() is the same call the sampler caches in
    // g_samplerMainBias.
    const uintptr_t bias = resolveMainImageLoadBias();
    const bool inMainImage = bias != 0 && creatorReturn > bias;
    // Same load-bias mapping as lr_elfva above, applied to the start routine the
    // guest handed us. start= is a runtime pointer and cannot be compared with
    // the ELF addresses every other line in this project is written in, so the
    // guest start routine was the one field of this line that could not be
    // checked offline against .text. It is what decides whether the thread body
    // is a real guest function at all, so it is reported in the same form.
    // A start routine outside the main image (a host trampoline, or a routine in
    // another guest DSO such as libc++_shared.so) is reported as start_img=other
    // with start_elfva=0 rather than silently biased.
    const bool startInMainImage = bias != 0 && start > bias;
    char details[256];
    __builtin_snprintf(details, sizeof(details),
                       "start=0x%llx start_elfva=0x%llx start_img=%s"
                       " arg=0x%llx creator=%llu host_owned=%d"
                       " lr=0x%llx lr_elfva=0x%llx lr_img=%s",
                       (unsigned long long)start,
                       (unsigned long long)(startInMainImage ? start - bias : 0ULL),
                       startInMainImage ? "libRealRacing3.so" : "other",
                       (unsigned long long)argument,
                       (unsigned long long)creatorOrdinal, hostOwned ? 1 : 0,
                       (unsigned long long)creatorReturn,
                       (unsigned long long)(inMainImage
                                                ? creatorReturn - bias : 0ULL),
                       inMainImage ? "libRealRacing3.so" : "host");
    censusLog( "guest-thread", entry->ordinal,
                        (unsigned long long)start, details);
    return entry;
}

void censusAttach(CensusEntry* entry) {
    if (entry == nullptr) {
        return;
    }
    const uintptr_t key = censusKey();
    mutexLock(&gCensusLock);
    entry->key = key;
    entry->attached = true;
    entry->firstTick = censusTick();
    entry->lastTick = entry->firstTick;
    // See censusBindCaller: the handle must be read by the thread it names.
    entry->kernelHandle = static_cast<uint32_t>(threadGetCurHandle());
    censusPublish(key, entry);
    mutexUnlock(&gCensusLock);
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "tpidrro=0x%llx start=0x%llx creator=%llu in_guest_scope=%d",
                       (unsigned long long)key,
                       (unsigned long long)entry->startRoutine,
                       (unsigned long long)entry->creatorOrdinal,
                       censusInsideGuestScope() ? 1 : 0);
    censusLog( "guest-thread-start", entry->ordinal,
                        (unsigned long long)key, details);
}

void censusDetach(CensusEntry* entry) {
    if (entry == nullptr) {
        return;
    }
    mutexLock(&gCensusLock);
    const uint64_t imports = entry->imports;
    const uint64_t lifetimeMs = censusMillis(censusTick() - entry->firstTick);
    const char* lastSymbol = entry->lastSymbol;
    entry->exited = true;
    mutexUnlock(&gCensusLock);
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "imports=%llu life_ms=%llu last=%s start=0x%llx",
                       (unsigned long long)imports,
                       (unsigned long long)lifetimeMs,
                       lastSymbol != nullptr ? lastSymbol : "(none)",
                       (unsigned long long)entry->startRoutine);
    censusLog( "guest-thread-exit", entry->ordinal,
                        imports, details);
}

// Bounded ring summary. The old dump printed kCensusDumpLines (220) lines per
// frozen thread, and a stalled thread re-dumps on every sweep that finds it
// still frozen, so one stuck thread could emit hundreds of lines a sweep. That
// was the single largest contributor to both the host CPU climb and the log
// growth this instrumentation is meant to make cheap.
//
// What actually decides "where is this thread stuck" is two things, and both
// survive a summary: the top distinct (symbol, call site) pairs, which is the
// shape of the call history, and the last few entries in order, which is where
// the thread was when it stopped. Everything between is repetition.
//
// The exhaustive 220-line dump is still available, off by default, behind
// RR3_CENSUS_RING_FULL=1 or "ring_full=1" in sdmc:/switch/rr3-census.conf.
constexpr size_t kCensusRingTop = 8;
constexpr uint32_t kCensusRingTail = 12;
// Bounded aggregation: past this many distinct keys new keys are dropped rather
// than growing the scan. A stale thread's history is overwhelmingly repeated
// mutex/futex pairs, so the bound is not what limits the answer.
constexpr size_t kCensusRingBuckets = 32;
constexpr const char* kCensusConf = "sdmc:/switch/rr3-census.conf";

struct CensusRingBucket {
    const char* symbol;
    uintptr_t address;
    uint32_t count;
    // Index of this key's most recent occurrence, used to break count ties.
    // A frozen thread's 512-entry window is usually 512 *distinct* call sites,
    // so every count is 1 and rank-by-count alone would surface the oldest
    // entries -- the part of the history least likely to explain the freeze.
    uint32_t lastIndex;
};

int gCensusRingFullResolved;
bool gCensusRingFull;

bool censusConfRingFull() {
    std::FILE* file = std::fopen(kCensusConf, "r");
    if (file == nullptr) {
        return false;
    }
    char line[128];
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\r'
            || *cursor == '\0') {
            continue;
        }
        if (std::strncmp(cursor, "ring_full=", 10) == 0
            && cursor[10] == '1') {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

// Kill switch for the whole census, for the one experiment that matters most:
// does the guest's second onViewRenderJNI return once nothing of ours is
// touching the emulated SD card? The census writes ~250 lines per 4 s sweep
// (open + append + close each) and the guest is doing its own file I/O in the
// render path on the same card, so a starved card read looks exactly like a
// guest that hangs by itself. Enable with `census=off` in the census conf, or
// RR3_CENSUS=0. Everything downstream already early-returns on !gCensusArmed,
// so gating censusArm() is enough to go quiet.
bool censusConfDisabled() {
    std::FILE* file = std::fopen(kCensusConf, "r");
    if (file == nullptr) {
        return false;
    }
    char line[128];
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }
        if (*cursor == '#' || *cursor == '\n' || *cursor == '\r'
            || *cursor == '\0') {
            continue;
        }
        if (std::strncmp(cursor, "census=", 7) == 0
            && (cursor[7] == '0'
                || (cursor[7] == 'o' && cursor[8] == 'f'
                    && cursor[9] == 'f' && (cursor[10] == '\n'
                                            || cursor[10] == '\r'
                                            || cursor[10] == '\0')))) {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
}

int gCensusDisabledResolved;
bool gCensusDisabled;

bool censusDisabled() {
    if (gCensusDisabledResolved == 0) {
        gCensusDisabledResolved = 1;
        const char* env = std::getenv("RR3_CENSUS");
        if (env != nullptr && env[0] == '0' && env[1] == '\0') {
            gCensusDisabled = true;
        } else {
            gCensusDisabled = censusConfDisabled();
        }
    }
    return gCensusDisabled;
}

bool censusRingFullDump() {
    if (gCensusRingFullResolved == 0) {
        gCensusRingFullResolved = 1;
        const char* env = std::getenv("RR3_CENSUS_RING_FULL");
        gCensusRingFull = (env != nullptr && env[0] == '1' && env[1] == '\0')
            ? true
            : censusConfRingFull();
    }
    return gCensusRingFull;
}

// Attributes a runtime address to the guest image that owns it and returns the
// offset inside that image. Shared by the import ring and the frame-pointer walk
// so the two can never disagree about where an image starts.
//
// Order matters and is the whole point: attribute to an image FIRST, subtract
// SECOND. The ring holds return addresses from EVERY guest image, not just the
// main one, and subtracting the main image bias from an address that lives in
// libc++_shared.so produces a 2.25 MB "offset" for a 1.01 MB library -- a
// number that can only be wrong, and it survived a long time because
// 0x223A934 looked plausible. The soname is returned so a bad attribution
// stays visible instead of hiding inside a plausible number.
uintptr_t censusImageOffsetOf(const GuestStartupPlan& plan, uintptr_t address,
                              const char** soname) {
    *soname = "host";
    for (uint64_t i = 0; i < plan.imageCount && i < kGuestStartupMaximumImages; ++i) {
        const GuestStartupImage& img = plan.images[i];
        const uintptr_t base = static_cast<uintptr_t>(img.base);
        const uintptr_t span = static_cast<uintptr_t>(img.imageBytes);
        if (base == 0 || span == 0 || address < base || address - base >= span) {
            continue;
        }
        *soname = img.soname[0] != '\0' ? img.soname : "guest";
        return address - base;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Frame-pointer walk: the last instrument, for the case the PC cannot reach.
//
// Why the PC is not enough: on Eden svcGetThreadContext3 answers Result 0 and
// returns pc = 0x4 for a guest thread, in 213 of 220 samples, while the same
// dump returns a plausible guest lr (0x21...) and a plausible stack sp (0x10...).
// The registers around the PC look real; the PC field specifically does not.
// So the call stack is not read from the PC, it is read from the stack.
//
// Why a frame-pointer walk is legitimate here, established by inspecting the
// guest image rather than assumed: libRealRacing3.so is compiled WITH frame
// pointers. A raw instruction-bit census of its 31.6 MB of executable text
// finds 25431 `stp x29, x30, [sp, #-n]!` prologues, matched 1:1 by 25415
// `mov x29, sp`, against 439460 `bl` sites -- and zero `str x30, [sp, #k]`
// leaf saves and zero post-index `stp x29, x30, [sp], #n`, i.e. no competing
// non-frame-pointer prologue style anywhere in the image. libc++_shared.so is
// the same (898/898). So the chain is not a hope; it is a property of the
// binary.
//
// The layout, from that prologue:
//     stp x29, x30, [sp, #-N]!   ;  [sp]   = caller's x29
//     mov x29, sp                 ;  [sp+8] = this frame's return address
// so one outward step is: lr = [fp + 8]; next fp = [fp].
//
// Two safety rules, both load-bearing:
//   * every read is gated on svcQueryMemory, so a garbage fp costs one syscall
//     and faults nothing. This walks memory it did not allocate.
//   * the walk is one-shot per thread and runs only on a thread that has
//     stopped advancing. A frozen thread's stack does not change, so re-walking
//     it 240 times a minute would only burn the CPU the guest needs, and a
//     moving thread's stack would be torn under the read.
//
// No std::mutex is taken here: this runs from the census reporter thread, but
// the thread being walked is the main guest thread, and a lock on that path has
// wedged runs on this emulator before by starving the scheduler.
// ---------------------------------------------------------------------------
constexpr uint32_t kFpWalkMaxFrames = 32;
// A thread stack is at most a few hundred KB. 2 MB of slack above sp is far
// past any real frame chain and still small enough that a runaway cursor cannot
// wander into the heap.
constexpr uintptr_t kFpWalkWindow = 0x200000;
constexpr uint64_t kFpWalkIdleMs = 2000;
constexpr uint32_t kFpWalkRepeats = 3;
constexpr uint64_t kFpWalkRepeatMs = 15000;

// The NRO's own runtime load base. _start is the ELF entry point at vaddr 0, so
// its runtime address is the base, and every host address becomes a link-time
// vaddr that a linker map can name. Cached because it is constant and this is on
// a path that runs while a guest thread is paused.
extern "C" void _start();

uintptr_t censusNroBase() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(&_start);
    return base;
}

// Reads one guest word with a mapping check first. svcQueryMemory answers for an
// unmapped address instead of letting the load trap, which is the difference
// between "the walk stopped" and "the run died".
bool censusReadStackWord(uintptr_t address, uint64_t* out) {    if ((address & 7) != 0) {
        return false;
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        return false;
    }
    if ((info.perm & Perm_R) == 0) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(*out));
    return true;
}

// ---------------------------------------------------------------------------
// Full register dump for the frozen root thread.
//
// Why this and not just the callee-saved set: the frame-pointer chain yields
// ANCESTORS, and a callee-saved register set yields the values of whichever
// frame last wrote them. Neither names the instruction the thread is sitting
// on. The only registers that do are the caller-saved x0..x18, because a leaf
// leaves its arguments and intermediates exactly where the call put them --
// which is how you read "the call target was X" when the pc field is the
// 0x4 sentinel. So the dump is complete: all 29 GPRs, fp, lr, sp, pc, psr.
//
// Every sample, no top-N. The walk is already bounded (kFpWalkRepeats every
// kFpWalkRepeatMs, root thread only), so completeness costs a fixed ~68 lines
// for this function and buys the ability to say "no register points into X"
// instead of "the registers we happened to print do not point into X". That is
// this function's share of a ~200-line walk, not the whole of it.
//
// Attribution is censusImageOffsetOf's rule, verbatim: attribute to an image
// FIRST, subtract SECOND. A second rule here could disagree with the ring and
// the walk, which is exactly how 0x223A934 survived as an offset for a
// 1.01 MB library.
// ---------------------------------------------------------------------------

// The Cloudcell init body, as localised by the frame walk. Kept as named
// constants so the dump itself can answer "is any register inside it" instead
// of leaving that arithmetic to whoever reads the log afterwards.
//
// The extent is taken from the function itself, not from the first localisation:
// frame 0's caller branches to 0xcd1014, and this function's own epilogue and
// error paths run to 0xcd3638 (the cold blocks at 0xcd35f0..0xcd3638, reached by
// b.hs out of the SSO-length checks). The old pair (0xcd0c34, 0xcd1754) stopped
// 0x640 bytes short of the real end, so the hit test reported hits=0 for a
// register that was demonstrably inside the body (x30 = 0xcd1d98). A range that
// is too small does not fail loudly -- it reports a confident zero.
constexpr uintptr_t kCcInitBodyLo = 0xcd1014;
constexpr uintptr_t kCcInitBodyHi = 0xcd3638;

// A non-pc register below this is a small integer, not an address, so it is
// never worth a mapping query. kCensusPcSentinel (0x1000) is the right cut for
// the pc field and far too low for the rest: x2 = 0x83ff is a flag or a length,
// and answering "other-mapping" for it invents a mapping that does not exist.
constexpr uintptr_t kCensusRegImmMax = 0x10000;

struct RegisterClass {
    const char* kind;
    uintptr_t off;
    uint32_t memType;    // kernel-reported memory type, valid when kind
    uintptr_t mapBase;   // is "other-mapping"
};

// The mapping a run of addresses lives in, from the kernel rather than from a
// guessed range. Guessing is how a thread stack and a guest allocation both
// end up labelled "host code": the NRO base is 0x80..., this thread's stack is
// 0x108..., and the guest's own low mappings are 0x2102... -- all three sit
// between the NRO base and the lowest guest image, so "above the NRO base"
// proves nothing. svcQueryMemory answers with the real extent, so the question
// is asked once instead of guessed per register.
struct CensusMapping {
    uintptr_t lo;
    uintptr_t hi;
    bool valid;
};

const CensusMapping censusMapOf(uintptr_t address, uint32_t* memType) {
    CensusMapping m{0, 0, false};
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (memType != nullptr) {
        *memType = 0;
    }
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        return m;
    }
    m.lo = static_cast<uintptr_t>(info.addr);
    m.hi = m.lo + static_cast<uintptr_t>(info.size);
    m.valid = m.hi > m.lo;
    if (memType != nullptr) {
        *memType = info.type & 0xFF;
    }
    return m;
}

// The NRO's own mapping, resolved once. Only ever asked for the NRO base, so
// the memory type it reports is not interesting -- just the extent.
const CensusMapping& censusNroMapping() {
    static const CensusMapping m = censusMapOf(censusNroBase(), nullptr);
    return m;
}

// Classifies one sampled value. Order is load-bearing and mirrors the rule the
// ring and the walk already use: guest image first, because censusImageOffsetOf
// is the single attribution rule and a second one could disagree with it. Then
// the NRO's own mapping, which is exact. Then this thread's stack, which is
// exact. Everything else is reported as an unclassified mapping WITH its
// kernel-reported type, so an address that nobody has explained yet stays
// visible instead of being rounded into a category it does not fit.
RegisterClass censusClassifyRegister(const GuestStartupPlan& plan,
                                     uintptr_t nroBase, uintptr_t sp,
                                     uintptr_t value, bool isPc) {
    RegisterClass rc{"other", value};
    if (value == 0) {
        rc.kind = "zero";
        rc.off = 0;
        return rc;
    }
    if (isPc) {
        if (value < kCensusPcSentinel) {
            rc.kind = "sentinel";
            rc.off = value;
            return rc;
        }
    }
    else if (value < kCensusRegImmMax) {
        rc.kind = "imm";
        rc.off = value;
        return rc;
    }
    const char* soname = "host";
    const uintptr_t off = censusImageOffsetOf(plan, value, &soname);
    if (soname != nullptr && soname[0] != '\0'
        && std::strcmp(soname, "host") != 0) {
        rc.kind = soname;
        rc.off = off;
        return rc;
    }
    const CensusMapping& nro = censusNroMapping();
    if (nro.valid && value >= nro.lo && value < nro.hi) {
        rc.kind = "host-nro";
        rc.off = nroBase != 0 ? value - nroBase : 0;
        return rc;
    }
    if (sp != 0 && value >= sp && value < sp + kFpWalkWindow) {
        rc.kind = "stack";
        rc.off = value - sp;
        return rc;
    }
    // Unexplained so far: name the mapping the kernel reports rather than
    // guessing. One syscall, and only for the addresses nothing else claimed.
    // The raw memory type is carried through to the log line because the
    // question it answers -- heap? thread-local? a mapped file? -- is the whole
    // point of classifying an address nobody expected.
    uint32_t memType = 0;
    const CensusMapping m = censusMapOf(value, &memType);
    if (m.valid) {
        rc.memType = memType;
        rc.mapBase = m.lo;
        // The NRO is not one mapping: the loader splits it into code-static,
        // code-mutable, rodata and data, so the single query on the entry
        // point finds only the first of those and every other host code
        // address falls through here. Any Code-typed mapping that is not inside
        // a guest image is the NRO's own code -- the guest images were already
        // attributed above, by the plan, so this test needs no NRO size and
        // cannot claim a guest address.
        const bool hostCode = memType == MemType_CodeStatic
            || memType == MemType_CodeMutable
            || memType == MemType_ModuleCodeStatic
            || memType == MemType_ModuleCodeMutable;
        if (hostCode) {
            rc.kind = "host-nro";
            rc.off = nroBase != 0 ? value - nroBase : 0;
        }
        else {
            rc.kind = "other-mapping";
            rc.off = value - m.lo;
        }
        return rc;
    }
    rc.kind = "unmapped";
    return rc;
}


// Is this address inside a loaded guest image?
//
// censusClassifyRegister answers with a guest SONAME (libRealRacing3.so,
// libc++_shared.so, ...) when the address falls inside a loaded image, and
// with one of the fixed host/other words below otherwise. The words are listed
// explicitly rather than tested with "soname != host": that looser test is
// precisely the rule that once produced a plausible-looking offset for an
// address that was in neither image, and it survived a long time because the
// number looked fine.
//
// The guest ELF virtual address is then rc.off, which is address - image base
// -- identical to (address - loadBias) for that image, so it feeds straight
// into a disassembler. This is censusImageOffsetOf's arithmetic, not a second
// one.
bool censusIsGuestImage(const RegisterClass& rc) {
    return rc.kind != nullptr
        && std::strcmp(rc.kind, "host") != 0
        && std::strcmp(rc.kind, "host-nro") != 0
        && std::strcmp(rc.kind, "stack") != 0
        && std::strcmp(rc.kind, "other-mapping") != 0
        && std::strcmp(rc.kind, "unmapped") != 0
        && std::strcmp(rc.kind, "other") != 0
        && std::strcmp(rc.kind, "zero") != 0
        && std::strcmp(rc.kind, "sentinel") != 0
        && std::strcmp(rc.kind, "imm") != 0;
}

// The startup plan, captured at most once per kPlanRefreshPasses passes.
//
// Not a cache for speed -- captureGuestStartupPlan() fsyncs a log line on every
// single call, so calling it per thread per pass would put ~2 SD-card fsyncs
// per second on the census path for the whole run. Touched only from the
// reporter thread, so no lock is involved and nothing guest-visible is
// blocked.
const GuestStartupPlan& censusCachedPlan() {
    static GuestStartupPlan cached{};
    static uint64_t capturedAtPass = 0;
    static bool havePlan = false;
    const uint64_t pass = gCensusPcPass;
    if (!havePlan || pass < capturedAtPass
        || pass - capturedAtPass >= kPlanRefreshPasses) {
        cached = captureGuestStartupPlan();
        capturedAtPass = pass;
        havePlan = true;
    }
    return cached;
}

void censusRegisterDump(uint32_t ordinal, const ThreadContext& context) {
    // censusCachedPlan, NOT captureGuestStartupPlan: the latter fsyncs one
    // guest-view log line per call, and a frame walk calls this, censusFpWalk
    // and censusGuardScan -- three uncached captures, three SD-card fsyncs, for
    // one walk. All three are reached only from censusSampleThreadPcs, which
    // has already primed this cache at the top of the same pass, so this is a
    // struct copy and not a re-capture. See the note on kPlanRefreshPasses.
    const GuestStartupPlan plan = censusCachedPlan();
    const uintptr_t nroBase = censusNroBase();
    const uintptr_t sp = static_cast<uintptr_t>(context.sp);
    const uintptr_t pc = static_cast<uintptr_t>(context.pc.x);

    // Values first, unclassified, so the log still answers the question if the
    // classifier below is ever wrong: a raw dump cannot be argued with.
    char line[512];
    static const char* const kNames[29] = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
        "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
        "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27",
        "x28"};
    for (int first = 0; first < 29; first += 10) {
        int written = 0;
        written += __builtin_snprintf(line + written, sizeof(line) - written,
                                      "%s", first == 0 ? "" : " ");
        for (int r = first; r < 29 && r < first + 10; ++r) {
            written += __builtin_snprintf(
                line + written, sizeof(line) - written, "%s=0x%llx ",
                kNames[r],
                (unsigned long long)context.cpu_gprs[r].x);
        }
        censusLog("census-regs", ordinal,
                  static_cast<unsigned long long>(first), line);
    }
    __builtin_snprintf(line, sizeof(line),
                       "x29=0x%llx x30=0x%llx sp=0x%llx pc=0x%llx psr=0x%x",
                       (unsigned long long)context.fp,
                       (unsigned long long)context.lr,
                       (unsigned long long)context.sp, (unsigned long long)pc,
                       context.psr);
    censusLog("census-regs-special", ordinal, 0, line);

    // One line per register, then the question. The Cloudcell hit test is
    // answered inline so a reader never has to re-derive it.
    uint32_t hits = 0;
    for (int r = 0; r < 29; ++r) {
        const uintptr_t value =
            static_cast<uintptr_t>(context.cpu_gprs[r].x);
        const RegisterClass rc =
            censusClassifyRegister(plan, nroBase, sp, value, false);
        const bool inBody = std::strcmp(rc.kind, "libRealRacing3.so") == 0
            && rc.off >= kCcInitBodyLo && rc.off <= kCcInitBodyHi;
        __builtin_snprintf(line, sizeof(line),
                           "%s=0x%llx kind=%s off=0x%llx mtype=0x%x%s",
                           kNames[r], (unsigned long long)value, rc.kind,
                           (unsigned long long)rc.off, rc.memType,
                           inBody ? " CCNNEXT-BODY" : "");
        censusLog("census-reg-class", ordinal, r, line);
        if (inBody) {
            ++hits;
        }
    }
    struct Special { const char* name; uintptr_t value; bool isPc; };
    const Special specials[5] = {
        {"x29-fp", static_cast<uintptr_t>(context.fp), false},
        {"x30-lr", static_cast<uintptr_t>(context.lr), false},
        {"sp", static_cast<uintptr_t>(context.sp), false},
        {"pc", pc, true},
        {"psr", static_cast<uintptr_t>(context.psr), false}};
    for (size_t i = 0; i < 5; ++i) {
        const RegisterClass rc = censusClassifyRegister(
            plan, nroBase, sp, specials[i].value, specials[i].isPc);
        const bool inBody = std::strcmp(rc.kind, "libRealRacing3.so") == 0
            && rc.off >= kCcInitBodyLo && rc.off <= kCcInitBodyHi;
        __builtin_snprintf(line, sizeof(line),
                           "%s=0x%llx kind=%s off=0x%llx mtype=0x%x%s",
                           specials[i].name,
                           (unsigned long long)specials[i].value, rc.kind,
                           (unsigned long long)rc.off, rc.memType,
                           inBody ? " CCNNEXT-BODY" : "");
        censusLog("census-reg-class", ordinal, 29 + i, line);
        if (inBody) {
            ++hits;
        }
    }
    __builtin_snprintf(line, sizeof(line),
                       "registers=34 body=[0x%llx,0x%llx] hits=%u nrobase=0x%llx",
                       (unsigned long long)kCcInitBodyLo,
                       (unsigned long long)kCcInitBodyHi, hits,
                       (unsigned long long)nroBase);
    censusLog("census-reg-body", ordinal, 0, line);

    // ------------------------------------------------------------------------
    // What each pointer points AT.
    //
    // A register value on its own only says where the thread is. The word behind
    // it says what it is waiting for, which is the actual question: the freeze
    // is consistent with either "polling a word that never changes" or "spinning
    // on registers alone", and only the dereference separates them.
    //
    // Every read is gated on svcQueryMemory (censusReadStackWord), so an address
    // nobody can explain costs one syscall and faults nothing. The pointed-to
    // value is then classified by the SAME censusClassifyRegister, so a pointer
    // into a guest .bss word reports as "libRealRacing3.so off=..." rather than
    // as an unexplained 0x21... number, and a pointer into the heap keeps its
    // kernel-reported memory type. No second attribution rule.
    //
    // Bounded: at most 34 guarded reads, and only for values that could have a
    // word behind them at all. A zero, an immediate, the pc sentinel and an
    // unmapped address have no word, and asking anyway would only ever print a
    // line that says "no".
    struct Deref { const char* name; uintptr_t value; bool isPc; };
    Deref derefs[34];
    for (int r = 0; r < 29; ++r) {
        derefs[r] = {kNames[r], static_cast<uintptr_t>(context.cpu_gprs[r].x),
                     false};
    }
    derefs[29] = {"x29-fp", static_cast<uintptr_t>(context.fp), false};
    derefs[30] = {"x30-lr", static_cast<uintptr_t>(context.lr), false};
    derefs[31] = {"sp", static_cast<uintptr_t>(context.sp), false};
    derefs[32] = {"pc", pc, true};
    derefs[33] = {"psr", static_cast<uintptr_t>(context.psr), false};
    uint32_t derefed = 0;
    uint32_t zeroWords = 0;
    for (int i = 0; i < 34; ++i) {
        const Deref& d = derefs[i];
        if (d.value == 0 || d.value < kCensusRegImmMax
            || (d.isPc && d.value < kCensusPcSentinel)) {
            continue;
        }
        const RegisterClass host = censusClassifyRegister(plan, nroBase, sp,
                                                          d.value, d.isPc);
        if (std::strcmp(host.kind, "unmapped") == 0
            || std::strcmp(host.kind, "sentinel") == 0
            || std::strcmp(host.kind, "imm") == 0
            || std::strcmp(host.kind, "zero") == 0) {
            continue;
        }
        uint64_t word = 0;
        const bool have = censusReadStackWord(d.value, &word);
        const RegisterClass wc = censusClassifyRegister(
            plan, nroBase, sp, static_cast<uintptr_t>(word), false);
        __builtin_snprintf(line, sizeof(line),
                           "%s=0x%llx -> [0x%llx]=%s0x%llx vkind=%s voff=0x%llx "
                           "vmtype=0x%x",
                           d.name, (unsigned long long)d.value,
                           (unsigned long long)d.value,
                           have ? "" : "UNREADABLE/",
                           (unsigned long long)word, wc.kind,
                           (unsigned long long)wc.off, wc.memType);
        censusLog("census-deref", ordinal, i, line);
        ++derefed;
        if (have && word == 0) {
            ++zeroWords;
        }
    }
    // The headline the acceptance test is actually after, stated in one place:
    // how many registers point at a readable word, and how many of THOSE words
    // are zero. A spin on a zero word and a spin on registers alone differ
    // exactly here.
    __builtin_snprintf(line, sizeof(line),
                       "deref=%u zero_words=%u", derefed, zeroWords);
    censusLog("census-deref-end", ordinal, 0, line);
}

// ---------------------------------------------------------------------------
// __cxa_guard state probe: has any function-local static's initialisation guard
// been left mid-flight?
//
// Why this instrument exists at all: every host-side wait the census can see is
// accounted for and idle. The guest issues no futex syscalls, no bridge condvar
// is ever waited on, and no bridge mutex is held or orphaned. The one
// synchronisation object in the guest that NONE of that covers is the Itanium
// ABI __cxa_guard: it is not a bridge-tracked object at all, it is a plain word
// in .bss that the guest's libc++ pokes directly. A guard stuck mid-init would
// be invisible to every other instrument in this file, and a thread that died
// holding one would wedge every later taker of that static with no trace.
//
// The layout is read from the guest's own code, not assumed. libc++_shared.so
// exports __cxa_guard_acquire at 0xe38f8 and __cxa_guard_release at 0xe3a54, and
// their own instructions pin every field:
//
//   0xe3910  ldarb w8, [x0]         ; byte 0
//   0xe3914  tst   w8, #0xff
//   0xe3918  b.eq  0xe3924          ; byte0 == 0 -> already done, return 0
//   0xe3938  ldrb  w23, [x19, #1]    ; byte 1 = state
//   0xe396c  tbz   w8, #1, ...      ; bit1 of byte1 clear -> take the guard
//   0xe3970  orr   w8, w8, #4       ; bit2 = "a waiter exists"
//   0xe3988  cmp   w8, #1
//   0xe398c  b.ne  0xe3998
//   0xe3990  mov   w19, wzr          ; byte1 == 1 -> DONE, do not initialise
//   0xe39a8  mov   w8, #2
//   0xe39ac  str   w20, [x19, #4]    ; owner thread id
//   0xe39b0  strb  w8, [x19, #1]     ; byte1 = 2 -> IN PROGRESS, owned by us
//   0xe3a70  stlrb w21, [x0]         ; release: byte0 = 1
//   0xe3a88  strb  w21, [x19, #1]    ; release: byte1 = 1 (done)
//
// So, per 8-byte guard word:
//   byte 0        fast-path hint; 0 means "known done, skip the lock"
//   byte 1        0 = uninitialised, 1 = DONE, 2 = IN PROGRESS, bit2 = waiters
//   dword at +4   the owning thread, held while byte1 == 2
//
// This is the NEW libc++ encoding and it INVERTS the state numbering the classic
// Itanium ABI write-up uses. Here 1 is done and 2 is in progress; treating "state
// 1" as "in progress" would report every healthy guard in the process as stuck,
// which is why the encoding is taken from the disassembly above and not from the
// ABI document.
//
// One more thing the same disassembly settles, and it is load-bearing: both
// __cxa_guard_acquire (0xe3930) and __cxa_guard_release (0xe3a7c) call
// pthread_mutex_lock on a SINGLE process-wide mutex at libc+++0x100048 before
// doing anything else, and acquire additionally spins on a condvar at
// libc+++0x100070. So a wedged guard presents as a thread parked inside
// pthread_mutex_lock, which the census already attributes per thread via
// bridgeDepth/bridgeSymbol -- see the thread table this same function prints.
//
// Which words are guards is not guessed either. __cxa_guard_acquire/release are
// imports in both guest images, so each call site is a `bl` to a PLT stub
// resolved through .rela.plt, and the guard address is the x0 that the
// instruction sequence immediately before it materialises. Two shapes occur and
// both are resolved statically:
//   adrp xN, page ; add xN, xN, #off ; mov x0, xN      (direct)
//   adrp x0, page ; ldr  x0, [x0, #off]                (through a pointer slot)
// The second form is 46% of the call sites and is why a plain "find add x0"
// scan resolves barely half of them. Offline that pass resolved 1500 of 1541
// call sites in libRealRacing3.so and 259 of 260 in libc++_shared.so, yielding
// 414 + 32 = 446 distinct guard words, embedded below as offsets.
//
// A blind sweep of .bss was rejected, with numbers: the in-progress test is
// (byte1 & 7) == 2, which one word in eight satisfies by chance, so sweeping
// libRealRacing3.so's 0x1acfb9-byte writable span would emit roughly 3500
// hits per pass and bury the answer. Probing the 446 addresses that the
// disassembly proves are guards has no false positives at all.
//
// No lock is taken while a guest thread is stopped: this runs from the census
// reporter after the sweep has already resumed the thread, exactly like
// censusFpWalk. gCensusLock is held only for the few instructions it takes to
// copy the thread table, and it is the same lock the reporter already takes
// four times a second.
// ---------------------------------------------------------------------------

// clang-format off
// GENERATED, do not hand-edit. Regenerate with .build/guard/gentables.py.
static const uint32_t kCensusGuardOffsets[] = {
#include "rr3/census_guard_offsets.inc"
};
struct CensusGuardImage {
    const char* soname;
    uint32_t rwLo;
    uint32_t rwHi;
    uint32_t guardFirst;
    uint32_t guardCount;
};
static const CensusGuardImage kCensusGuardImages[] = {
    {"libRealRacing3.so", 0x01e1aae0u, 0x01fc7a99u, 0u, 414u},
    {"libc++_shared.so", 0x000ef5f0u, 0x00100b70u, 414u, 32u},
    {"libfmodex.so", 0x00135000u, 0x00188538u, 0u, 0u},
    {"libNimble.so", 0x00001d50u, 0x00002f78u, 0u, 0u},
};
// clang-format on

// Hard output bound. The scan is per fpwalk, and fpwalk is already bounded to
// three samples 15 s apart, so this cannot grow with run length.
constexpr uint32_t kCensusGuardMaxReported = 48;
// Same bound for the guards that completed. Observed maximum is 52 across all
// four images, so this never truncates in practice; the cap exists so that a
// future run where the guest gets much further still cannot flood the log.
constexpr uint32_t kCensusGuardLiveMaxReported = 64;

// Reads one guard word with a mapping check first, so a bad offset costs one
// syscall and faults nothing. 4-byte aligned rather than 8, because six of the
// 414 libRealRacing3.so guards are only 4-aligned: they sit in .data among
// 4-byte objects, so the compiler promised no more than that even though the
// guard itself is 8 bytes wide. An earlier pass read them through
// censusReadStackWord, whose `(address & 7)` test dropped them, and the six
// silently missing guards are exactly the kind of hole that hides the one that
// matters. censusReadStackWord keeps its 8-byte rule for the frame walk, whose
// arithmetic depends on it.
bool censusReadGuardWord(uintptr_t address, uint64_t* out) {
    if ((address & 3) != 0) {
        return false;
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        return false;
    }
    if ((info.perm & Perm_R) == 0) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(*out));
    return true;
}

// The state byte, decoded with the encoding read out of libc++ above. The name
// is the answer to "is this stuck", so it is spelled out rather than folded into
// a comparison at the call site.
const char* censusGuardStateName(uint8_t stateByte) {
    switch (stateByte & 0x7) {
        case 0: return "uninit";
        case 1: return "DONE";
        case 2: return "IN-PROGRESS";
        default: return "reserved";
    }
}

// True when x0 is the address of a guard word in any image's table, i.e. the
// register IS a guard pointer rather than a code or object pointer.
const CensusGuardImage* censusGuardOwnerOf(uintptr_t address,
                                           uint32_t* offsetOut) {
    for (size_t i = 0; i < sizeof(kCensusGuardImages) / sizeof(kCensusGuardImages[0]); ++i) {
        const CensusGuardImage& img = kCensusGuardImages[i];
        for (uint32_t g = 0; g < img.guardCount; ++g) {
            if (kCensusGuardOffsets[img.guardFirst + g] == address) {
                *offsetOut = address;
                return &img;
            }
        }
    }
    return nullptr;
}

void censusGuardScan(uint32_t ordinal, const ThreadContext& context) {
    const GuestStartupPlan plan = censusCachedPlan();
    char line[320];

    // The guard-relevant registers, asked directly: is any x0..x30/sp actually
    // holding a guard address? censusRegisterDump prints every register raw, but
    // "is this one of the 446 guards" is a question only this table can answer,
    // and answering it inline is what turns a 34-line dump into a verdict.
    uint32_t registerHits = 0;
    for (int r = 0; r < 29; ++r) {
        const uintptr_t value = static_cast<uintptr_t>(context.cpu_gprs[r].x);
        if (value < 0x10000) {
            continue;
        }
        for (uint64_t i = 0; i < plan.imageCount && i < kGuestStartupMaximumImages; ++i) {
            const GuestStartupImage& image = plan.images[i];
            const uintptr_t base = static_cast<uintptr_t>(image.base);
            if (base == 0 || value < base) {
                continue;
            }
            const uintptr_t offset = value - base;
            if (offset != 0 && offset < 0x40000000) {
                uint32_t unused = 0;
                const CensusGuardImage* owner = censusGuardOwnerOf(offset, &unused);
                if (owner != nullptr) {
                    __builtin_snprintf(line, sizeof(line),
                                       "x%d=0x%llx GUARD-ADDRESS img=%s off=0x%llx",
                                       r, (unsigned long long)value, owner->soname,
                                       (unsigned long long)offset);
                    censusLog("guard-reg", ordinal, static_cast<uint32_t>(r), line);
                    ++registerHits;
                }
            }
        }
    }
    __builtin_snprintf(line, sizeof(line),
                       "registers=29 guard_address_hits=%u guards_known=%u",
                       registerHits,
                       static_cast<unsigned>(sizeof(kCensusGuardOffsets) / sizeof(uint32_t)));
    censusLog("guard-reg-end", ordinal, 0, line);

    // The guard words themselves, image by image, using each image's own loader
    // base from the plan. Attribution is not re-derived: the base comes from
    // captureGuestStartupPlan, the same source censusImageOffsetOf reads, and the
    // cross-check line below prints what that function makes of the same address
    // so a disagreement is visible instead of silent.
    uint32_t reported = 0;
    uint32_t liveReported = 0;
    uint32_t totalInProgress = 0;
    for (uint64_t i = 0; i < plan.imageCount && i < kGuestStartupMaximumImages; ++i) {
        const GuestStartupImage& image = plan.images[i];
        const CensusGuardImage* table = nullptr;
        for (size_t t = 0; t < sizeof(kCensusGuardImages) / sizeof(kCensusGuardImages[0]); ++t) {
            if (std::strcmp(kCensusGuardImages[t].soname, image.soname) == 0) {
                table = &kCensusGuardImages[t];
                break;
            }
        }
        if (table == nullptr) {
            __builtin_snprintf(line, sizeof(line),
                               "img=%s base=0x%llx bytes=0x%llx NO-GUARD-TABLE",
                               image.soname, (unsigned long long)image.base,
                               (unsigned long long)image.imageBytes);
            censusLog("guard-image", ordinal, static_cast<uint32_t>(i), line);
            continue;
        }
        uint32_t uninit = 0, done = 0, inProgress = 0, other = 0, unreadable = 0;
        const uintptr_t base = static_cast<uintptr_t>(image.base);
        for (uint32_t g = 0; g < table->guardCount; ++g) {
            const uint32_t offset = kCensusGuardOffsets[table->guardFirst + g];
            uint64_t word = 0;
            if (!censusReadGuardWord(base + offset, &word)) {
                ++unreadable;
                continue;
            }
            const uint8_t byte0 = static_cast<uint8_t>(word & 0xFF);
            const uint8_t stateByte = static_cast<uint8_t>((word >> 8) & 0xFF);
            const uint32_t owner = static_cast<uint32_t>(word >> 32);
            switch (stateByte & 0x7) {
                case 0: ++uninit; break;
                case 1: ++done; break;
                case 2: ++inProgress; break;
                default: ++other; break;
            }
            if ((stateByte & 0x7) != 2) {
                continue;
            }
            ++totalInProgress;
            if (reported >= kCensusGuardMaxReported) {
                continue;
            }
            ++reported;
            const char* soname = "host";
            const uintptr_t cross = censusImageOffsetOf(plan, base + offset, &soname);
            __builtin_snprintf(line, sizeof(line),
                               "img=%s off=0x%08x addr=0x%llx byte0=0x%02x "
                               "state=0x%02x(%s) waiters=%u owner=0x%08x "
                               "crosscheck=%s+0x%llx",
                               image.soname, offset,
                               (unsigned long long)(base + offset), byte0,
                               stateByte, censusGuardStateName(stateByte),
                               (stateByte >> 2) & 1u, owner, soname,
                               (unsigned long long)cross);
            censusLog("guard-stuck", ordinal, reported, line);
        }
        // The guards that DID run, so "nothing is stuck" is a statement about a
        // population that demonstrably initialised rather than about 446 words
        // that were never touched. Only states 1 and 2 qualify: the 366 uninitialised
        // ones are untouched statics and say nothing either way. libc++ has just
        // four of these and they are exactly the ones the root thread's import
        // ring is sitting in, so the count is small and the lines are worth having.
        for (uint32_t g = 0; g < table->guardCount; ++g) {
            const uint32_t offset = kCensusGuardOffsets[table->guardFirst + g];
            uint64_t word = 0;
            if (!censusReadGuardWord(base + offset, &word)) {
                continue;
            }
            const uint8_t stateByte = static_cast<uint8_t>((word >> 8) & 0xFF);
            const uint32_t state = stateByte & 0x7;
            if (state != 1 && state != 2) {
                continue;
            }
            if (liveReported >= kCensusGuardLiveMaxReported) {
                break;
            }
            ++liveReported;
            __builtin_snprintf(line, sizeof(line),
                               "img=%s off=0x%08x addr=0x%llx byte0=0x%02x "
                               "state=0x%02x(%s) waiters=%u owner=0x%08x",
                               image.soname, offset,
                               (unsigned long long)(base + offset),
                               static_cast<unsigned>(word & 0xFF), stateByte,
                               censusGuardStateName(stateByte),
                               (stateByte >> 2) & 1u,
                               static_cast<unsigned>(word >> 32));
            censusLog("guard-done", ordinal, liveReported, line);
        }
        __builtin_snprintf(line, sizeof(line),
                           "img=%s base=0x%llx rw=[0x%08x,0x%08x) guards=%u "
                           "uninit=%u done=%u IN-PROGRESS=%u reserved=%u "
                           "unreadable=%u",
                           image.soname, (unsigned long long)image.base,
                           table->rwLo, table->rwHi, table->guardCount, uninit,
                           done, inProgress, other, unreadable);
        censusLog("guard-image", ordinal, static_cast<uint32_t>(i), line);
    }
    __builtin_snprintf(line, sizeof(line),
                       "images=%llu stuck=%u reported=%u cap=%u",
                       (unsigned long long)plan.imageCount, totalInProgress,
                       reported, kCensusGuardMaxReported);
    censusLog("guard-end", ordinal, 0, line);

    // The thread cross-reference. Copied out under gCensusLock and then reported
    // with no lock held; the question is whether any live thread is still
    // cycling __cxa_guard, and whether a thread that was inside the Cloudcell
    // init frame has since exited while a guard still names it as owner.
    struct Row {
        uint32_t ordinal;
        uint32_t creator;
        uint32_t depth;
        uint64_t enters;
        uint64_t idleMs;
        const char* symbol;
        bool root;
        bool exited;
        bool guardTraffic;
    };
    Row rows[kCensusCapacity];
    size_t rowCount = 0;
    const uint64_t now = censusTick();
    mutexLock(&gCensusLock);
    for (CensusEntry& entry : gCensus) {
        if (!entry.used || rowCount >= sizeof(rows) / sizeof(rows[0])) {
            continue;
        }
        Row& row = rows[rowCount++];
        row.ordinal = entry.ordinal;
        row.creator = static_cast<uint32_t>(entry.creatorOrdinal);
        row.depth = entry.bridgeDepth;
        row.enters = entry.bridgeEnters;
        row.idleMs = censusMillis(now - entry.lastTick);
        row.symbol = entry.lastSymbol;
        row.root = entry.root;
        row.exited = entry.exited;
        row.guardTraffic = false;
        for (uint32_t k = 0; k < entry.recentCount; ++k) {
            if (entry.recentSymbols[k] != nullptr
                && std::strstr(entry.recentSymbols[k], "cxa_guard") != nullptr) {
                row.guardTraffic = true;
                break;
            }
        }
    }
    mutexUnlock(&gCensusLock);

    uint32_t liveGuard = 0, exitedGuard = 0, live = 0, parkedInMutex = 0;
    for (size_t r = 0; r < rowCount; ++r) {
        const Row& row = rows[r];
        if (!row.exited) {
            ++live;
        }
        if (row.guardTraffic) {
            if (row.exited) {
                ++exitedGuard;
            } else {
                ++liveGuard;
            }
        }
        if (!row.exited && row.depth > 0 && row.symbol != nullptr
            && std::strcmp(row.symbol, "pthread_mutex_lock") == 0) {
            ++parkedInMutex;
        }
        __builtin_snprintf(line, sizeof(line),
                           "ord=%u creator=%u root=%d exited=%d idle_ms=%llu "
                           "inbridge=%u/%s enters=%llu guard_ring=%d",
                           row.ordinal, row.creator, row.root ? 1 : 0,
                           row.exited ? 1 : 0, (unsigned long long)row.idleMs,
                           row.depth, row.symbol != nullptr ? row.symbol : "(none)",
                           (unsigned long long)row.enters,
                           row.guardTraffic ? 1 : 0);
        censusLog("guard-thread", ordinal, row.ordinal, line);
    }
    __builtin_snprintf(line, sizeof(line),
                       "threads=%zu live=%u guard_ring_live=%u guard_ring_exited=%u "
                       "parked_in_mutex_lock=%u",
                       rowCount, live, liveGuard, exitedGuard, parkedInMutex);
    censusLog("guard-thread-end", ordinal, 0, line);
}

void censusFpWalk(uint32_t ordinal, const ThreadContext& context,
                  const uintptr_t startRoutine, const char* role) {
    // Cached: see censusRegisterDump. A walk is three of these (here, in
    // censusRegisterDump, in censusGuardScan) and each uncaptured plan is one
    // fsync on the emulated SD card.
    const GuestStartupPlan plan = censusCachedPlan();
    const uintptr_t sp = static_cast<uintptr_t>(context.sp);
    const uintptr_t fp = static_cast<uintptr_t>(context.fp);
    const uintptr_t lr = static_cast<uintptr_t>(context.lr);

    // The complete register set, before anything reads the stack. A frozen
    // thread has not executed an instruction since the wedge, so these are the
    // live values of the innermost frame.
    censusRegisterDump(ordinal, context);

    // Guard state, on the same bounded cadence. Runs after the register dump
    // because the register arm of the guard probe needs the same context, and
    // before the stack walk because a stuck guard, if there is one, is a better
    // explanation for the wedge than anything the walk will find below it.
    censusGuardScan(ordinal, context);

    // The premise, printed raw and unedited: is [sp] a stack address and is
    // [sp+8] a code address? If these two do not pair up, nothing below them
    // can be trusted and the whole instrument is closed.
    uint64_t slot[6]{};
    const uintptr_t probes[6] = {sp, sp + 8, sp + 16, fp, fp + 8, fp + 16};
    bool readable[6]{};
    for (size_t i = 0; i < 6; ++i) {
        readable[i] = censusReadStackWord(probes[i], &slot[i]);
    }
    char raw[384];
    __builtin_snprintf(raw, sizeof(raw),
        "start=0x%llx role=%s sp=0x%llx fp=0x%llx lr=0x%llx"
        " [sp]=0x%llx/%d [sp+8]=0x%llx/%d [sp+16]=0x%llx/%d"
        " [fp]=0x%llx/%d [fp+8]=0x%llx/%d [fp+16]=0x%llx/%d",
        (unsigned long long)startRoutine, role != nullptr ? role : "?",
        (unsigned long long)sp, (unsigned long long)fp,
        (unsigned long long)lr,
        (unsigned long long)slot[0], readable[0] ? 1 : 0,
        (unsigned long long)slot[1], readable[1] ? 1 : 0,
        (unsigned long long)slot[2], readable[2] ? 1 : 0,
        (unsigned long long)slot[3], readable[3] ? 1 : 0,
        (unsigned long long)slot[4], readable[4] ? 1 : 0,
        (unsigned long long)slot[5], readable[5] ? 1 : 0);
    censusLog("fpwalk-raw", ordinal, 0, raw);

    // The callee-saved registers, because the frame-pointer chain only yields
    // ANCESTORS and therefore cannot name the function the thread is actually
    // inside. For a frozen thread these are the live values of the innermost
    // frame: it has not run a single instruction since the wedge, so its `this`
    // and callee-saved pointers are still sitting in x19..x28. One extra line,
    // and it is the line that turns "some virtual call" into a named callee.
    char callee[320];
    __builtin_snprintf(callee, sizeof(callee),
        "x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx"
        " x25=0x%llx x26=0x%llx x27=0x%llx x28=0x%llx",
        (unsigned long long)context.cpu_gprs[19].x,
        (unsigned long long)context.cpu_gprs[20].x,
        (unsigned long long)context.cpu_gprs[21].x,
        (unsigned long long)context.cpu_gprs[22].x,
        (unsigned long long)context.cpu_gprs[23].x,
        (unsigned long long)context.cpu_gprs[24].x,
        (unsigned long long)context.cpu_gprs[25].x,
        (unsigned long long)context.cpu_gprs[26].x,
        (unsigned long long)context.cpu_gprs[27].x,
        (unsigned long long)context.cpu_gprs[28].x);
    censusLog("fpwalk-callee-saved", ordinal, 0, callee);

    // Follow the indirection the stuck call actually performs, so the answer is
    // "the callee is in image X at offset Y" rather than "there was a blr".
    //
    // The shape is fixed by the disassembly of the stuck site, not guessed:
    //     ldr x0, [x28]                                  ; a global holding a
    //                                                    service pointer
    //     ldr x8, [x0] ; ldr x8, [x8, #0x90] ; blr x8     ; vtable slot 18
    //     mov x24, x0                                    ; the RECEIVER, not
    //                                                    the return value
    //     ldr x8, [x24] ; ldr x8, [x8, #0x160] ; blr x8  ; slot 44, never
    //                                                    returns
    // So there is exactly ONE dereference from x28 to the object and ONE from
    // the object to the vtable. An earlier version of this walked two levels
    // and read a vtable slot out of unrelated memory; the count matters, so it
    // is spelled out here rather than left to a chain of `read(read(...))`.
    uint64_t service = 0, vtable = 0;
    const uint64_t x28 = context.cpu_gprs[28].x;
    const bool haveService =
        censusReadStackWord(static_cast<uintptr_t>(x28), &service);
    const bool haveVtable = haveService
        && censusReadStackWord(static_cast<uintptr_t>(service), &vtable);
    uint64_t slot90 = 0, slot160 = 0;
    const bool have90 = haveVtable
        && censusReadStackWord(static_cast<uintptr_t>(vtable) + 0x90, &slot90);
    const bool have160 = haveVtable
        && censusReadStackWord(static_cast<uintptr_t>(vtable) + 0x160, &slot160);
    // The NRO's own load base, so a host address becomes a link-time vaddr that
    // nm can name. _start is at vaddr 0 in the link, so its runtime address IS
    // the base. Without this every host frame is an unnameable 0x80... number,
    // and the base moves between runs, which is exactly what makes a hardcoded
    // guess wrong twice.
    const uintptr_t nroBase = censusNroBase();
    const auto hostOffset = [nroBase](uintptr_t address) -> uintptr_t {
        return address >= nroBase ? address - nroBase : 0;
    };
    const char* img90 = "host";
    const char* img160 = "host";
    const uintptr_t off90 = have90
        ? censusImageOffsetOf(plan, static_cast<uintptr_t>(slot90), &img90) : 0;
    const uintptr_t off160 = have160
        ? censusImageOffsetOf(plan, static_cast<uintptr_t>(slot160), &img160) : 0;
    char vcall[352];
    __builtin_snprintf(vcall, sizeof(vcall),
        "nro=0x%llx x28=0x%llx service=0x%llx/%d vtable=0x%llx/%d"
        " [vt+0x90]=0x%llx/%d img=%s off=0x%llx nrooff=0x%llx"
        " [vt+0x160]=0x%llx/%d img=%s off=0x%llx nrooff=0x%llx",
        (unsigned long long)nroBase,
        (unsigned long long)x28, (unsigned long long)service,
        haveService ? 1 : 0,
        (unsigned long long)vtable, haveVtable ? 1 : 0,
        (unsigned long long)slot90, have90 ? 1 : 0, img90,
        (unsigned long long)off90,
        (unsigned long long)hostOffset(static_cast<uintptr_t>(slot90)),
        (unsigned long long)slot160, have160 ? 1 : 0, img160,
        (unsigned long long)off160,
        (unsigned long long)hostOffset(static_cast<uintptr_t>(slot160)));
    censusLog("fpwalk-vcall", ordinal, 0, vcall);

    // Seed from x29 when it is usable, because the saved sp is only the frame
    // base while the thread sits mid-function: a leaf or a point before the
    // prologue has sp != fp. Fall back to sp, which is right whenever the
    // thread is inside a function that has already set up its frame.
    const uintptr_t lowBound = sp > 0x1000 ? sp - 0x1000 : 0;
    const uintptr_t highBound = sp + kFpWalkWindow;
    const bool fpUsable = fp >= lowBound && fp < highBound && (fp & 7) == 0;
    uintptr_t cursor = fpUsable ? fp : sp;
    char seedLine[192];
    __builtin_snprintf(seedLine, sizeof(seedLine),
        "seed=%s cursor=0x%llx window=[0x%llx,0x%llx) images=%llu max=%u",
        fpUsable ? "fp" : "sp", (unsigned long long)cursor,
        (unsigned long long)lowBound, (unsigned long long)highBound,
        (unsigned long long)plan.imageCount, kFpWalkMaxFrames);
    censusLog("fpwalk-begin", ordinal, 0, seedLine);

    uint32_t frames = 0;
    const char* stop = "frame-limit";
    char line[320];
    for (; frames < kFpWalkMaxFrames; ++frames) {
        if (cursor < lowBound || cursor >= highBound || (cursor & 7) != 0) {
            stop = "out-of-window";
            break;
        }
        uint64_t callerFp = 0;
        uint64_t returnAddress = 0;
        if (!censusReadStackWord(cursor, &callerFp)
            || !censusReadStackWord(cursor + 8, &returnAddress)) {
            stop = "unreadable";
            break;
        }
        if (callerFp == 0) {
            stop = "null-fp";
            break;
        }
        // Frames grow upward as the chain unwinds, so a chain that fails to
        // strictly increase has left the stack -- that one test catches a
        // repeated fp, a backward pointer, and a wild fp in one go.
        if (callerFp <= cursor) {
            stop = "non-increasing";
            break;
        }
        const char* soname = "host";
        const uintptr_t offset =
            censusImageOffsetOf(plan, static_cast<uintptr_t>(returnAddress),
                                &soname);
        __builtin_snprintf(line, sizeof(line),
                           "i=%u fp=0x%llx lr=0x%llx img=%s off=0x%llx",
                           frames, (unsigned long long)cursor,
                           (unsigned long long)returnAddress, soname,
                           (unsigned long long)offset);
        censusLog("fpwalk-frame", ordinal, frames, line);
        cursor = static_cast<uintptr_t>(callerFp);
    }
    __builtin_snprintf(line, sizeof(line),
                       "frames=%u stop=%s cursorEnd=0x%llx window=0x%llx",
                       frames, stop, (unsigned long long)cursor,
                       (unsigned long long)(highBound - lowBound));
    censusLog("fpwalk-end", ordinal, frames, line);
}

// Prints one frozen thread's import history: the top distinct call sites by
// count plus the last kCensusRingTail entries in order. The address is printed
// both raw and as a libRealRacing3.so ELF virtual address, because the raw
// value is a rebased runtime address that changes every launch and cannot be
// cross-referenced against Ghidra.
void censusDumpRing(uint32_t ordinal) {
    mutexLock(&gCensusLock);
    CensusEntry* target = nullptr;
    for (CensusEntry& entry : gCensus) {
        if (entry.used && entry.ordinal == ordinal) {
            target = &entry;
            break;
        }
    }
    if (target == nullptr || target->ringDumped) {
        mutexUnlock(&gCensusLock);
        return;
    }
    target->ringDumped = true;
    const uint32_t count = target->recentCount;
    const uint64_t imports = target->imports;
    const uintptr_t start = target->startRoutine;
    const uint64_t firstMs = censusMillis(censusTick() - target->lastTick);
    const bool full = censusRingFullDump();
    // Copy under the lock: the writer thread is frozen, but a host thread that
    // happens to match this ordinal must not mutate the ring mid-dump.
    uintptr_t returns[kCensusReturnRing]{};
    const char* symbols[kCensusReturnRing]{};
    for (uint32_t i = 0; i < count; ++i) {
        returns[i] = target->recentReturns[i];
        symbols[i] = target->recentSymbols[i];
    }
    mutexUnlock(&gCensusLock);

    // The census ring holds return addresses from EVERY guest image, not just
    // the main one. See censusImageOffsetOf for why the attribution has to
    // happen before the subtraction, and why the lambda is a one-line delegate
    // rather than a second copy of the rule.
    const GuestStartupPlan ringPlan = captureGuestStartupPlan();
    const auto ringOffsetOf = [ringPlan](uintptr_t address, const char** soname) -> uintptr_t {
        return censusImageOffsetOf(ringPlan, address, soname);
    };
    const uint32_t dumped = count < kCensusDumpLines
        ? count : static_cast<uint32_t>(kCensusDumpLines);
    const uint32_t first = count - dumped;
    const uint32_t tailStart = count > kCensusRingTail
        ? count - kCensusRingTail : 0;

    // Aggregate the whole ring, not just the dumped tail: the call-shape answer
    // comes from the repeated pairs, and those are at the head.
    CensusRingBucket buckets[kCensusRingBuckets]{};
    size_t bucketCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        size_t slot = bucketCount;
        for (; slot < bucketCount; ++slot) {
            if (buckets[slot].symbol == symbols[i]
                && buckets[slot].address == returns[i]) {
                break;
            }
        }
        if (slot == bucketCount) {
            if (bucketCount == kCensusRingBuckets) {
                continue;
            }
            buckets[bucketCount].symbol = symbols[i];
            buckets[bucketCount].address = returns[i];
            buckets[bucketCount].count = 0;
            buckets[bucketCount].lastIndex = 0;
            ++bucketCount;
        }
        ++buckets[slot].count;
        buckets[slot].lastIndex = i;
    }
    // Selection sort: at most kCensusRingBuckets^2/2 comparisons, once per
    // frozen thread per run. Not worth anything cleverer. Most frequent first,
    // and among equals the most recently seen.
    for (size_t i = 0; i < bucketCount; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < bucketCount; ++j) {
            if (buckets[j].count > buckets[best].count
                || (buckets[j].count == buckets[best].count
                    && buckets[j].lastIndex > buckets[best].lastIndex)) {
                best = j;
            }
        }
        if (best != i) {
            const CensusRingBucket swap = buckets[i];
            buckets[i] = buckets[best];
            buckets[best] = swap;
        }
    }
    const size_t topCount = bucketCount < kCensusRingTop
        ? bucketCount : kCensusRingTop;

    char details[224];
    __builtin_snprintf(details, sizeof(details),
                       "start=0x%llx imports=%llu entries=%u idle_ms=%llu "
                       "order=oldest-first mode=%s",
                       (unsigned long long)start,
                       (unsigned long long)imports, count,
                       (unsigned long long)firstMs,
                       full ? "full" : "summary");
    censusLog("census-ring-begin", ordinal, imports, details);

    if (full) {
        // The tail is what matters: the freeze happens at the END of the
        // history, and the leading entries are hundreds of identical mutex
        // lock/unlock pairs that push the decisive calls out of the log.
        for (uint32_t i = 0; i < dumped; ++i) {
            const uintptr_t address = returns[first + i];
            const char* soname = "host";
            const uintptr_t offset = ringOffsetOf(address, &soname);
            __builtin_snprintf(details, sizeof(details),
                               "i=%u sym=%s lr=%llx img=%s off=0x%llx",
                               first + i,
                               symbols[first + i] != nullptr ? symbols[first + i]
                                                            : "(null)",
                               (unsigned long long)address, soname,
                               (unsigned long long)offset);
            censusLog("census-ring", ordinal, i, details);
        }
    } else {
        for (size_t i = 0; i < topCount; ++i) {
            const uintptr_t address = buckets[i].address;
            const char* soname = "host";
            const uintptr_t offset = ringOffsetOf(address, &soname);
            __builtin_snprintf(details, sizeof(details),
                               "rank=%zu sym=%s lr=%llx img=%s off=0x%llx n=%u",
                               i,
                               buckets[i].symbol != nullptr ? buckets[i].symbol
                                                            : "(null)",
                               (unsigned long long)address, soname,
                               (unsigned long long)offset,
                               buckets[i].count);
            censusLog("census-ring-top", ordinal, i, details);
        }
        for (uint32_t i = tailStart; i < count; ++i) {
            const uintptr_t address = returns[i];
            const char* soname = "host";
            const uintptr_t offset = ringOffsetOf(address, &soname);
            __builtin_snprintf(details, sizeof(details),
                               "i=%u sym=%s lr=%llx img=%s off=0x%llx", i,
                               symbols[i] != nullptr ? symbols[i] : "(null)",
                               (unsigned long long)address, soname,
                               (unsigned long long)offset);
            censusLog("census-ring-tail", ordinal, i - tailStart, details);
        }
    }

    __builtin_snprintf(details, sizeof(details),
                       "entries=%u dumped=%u mode=%s top=%zu tail=%u",
                       count, full ? dumped : count,
                       full ? "full" : "summary", topCount,
                       count - tailStart);
    censusLog("census-ring-end", ordinal, count, details);
}

void censusWaitLedger(unsigned int sweep);
void censusMutexLedger(unsigned int sweep);
// Defined further down; the sweep calls it for every non-exited, zero-delta
// thread because a stopped thread can no longer reach it from the import path.
void censusNoteStall(CensusEntry* entry);

void censusSweep(uint64_t now) {
    // Single-flight, and it NEVER blocks. See censusMaybeSweep for why the
    // blocking version was fatal to the pass cadence. The lock is taken first,
    // before the batch scope, so a caller that loses the race leaves no trace.
    if (!mutexTryLock(&gCensusSnapLock)) {
        return;     // another sweep owns it; that one will log its own lines
    }
    // Everything below (sweep line, wait ledger, one census-ord per thread, and
    // the bounded ring summaries) is buffered and written with a single
    // open/fsync/close instead of one per line.
    CensusBatchScope batch;
    mutexLock(&gCensusLock);
    gCensusSweepTick = now;
    ++gCensusSweeps;
    const uint64_t elapsedMs = censusMillis(now - gCensusFirstTick);
    size_t count = 0;
    uint64_t total = 0;
    size_t moving = 0;
    uint64_t frozenKeys[kCensusCapacity]{};
    size_t frozenCount = 0;
    for (const CensusEntry& entry : gCensus) {
        if (!entry.used) {
            continue;
        }
        // A thread that stopped importing can never reach censusNoteStall
        // through the import-return path it used to be called from: that path
        // is gated on `(imports & 0x3f) == 0` INSIDE the return handler. The
        // one thread we most need to stall-report is exactly the one that
        // stopped importing, so the detector was structurally blind to it.
        // Collect the keys here, under the lock; report them after it is
        // released (censusNoteStall takes gCensusLock itself).
        if (!entry.exited && entry.imports == entry.reportedImports &&
            frozenCount < kCensusCapacity) {
            frozenKeys[frozenCount++] = entry.key;
        }
        CensusSnapshot& snap = gCensusSnapshot[count];
        snap.ordinal = entry.ordinal;
        snap.startRoutine = entry.startRoutine;
        snap.creatorOrdinal = entry.creatorOrdinal;
        snap.imports = entry.imports;
        snap.delta = entry.imports - entry.reportedImports;
        snap.firstTick = entry.firstTick;
        snap.lastTick = entry.lastTick;
        snap.recentCount = entry.recentCount < kCensusTail
            ? entry.recentCount : kCensusTail;
        for (uint32_t i = 0; i < snap.recentCount; ++i) {
            // Snapshot the TAIL of the live ring, not its head.
            const uint32_t offset = entry.recentCount - snap.recentCount;
            snap.recentReturns[i] = entry.recentReturns[offset + i];
            snap.recentSymbols[i] = entry.recentSymbols[offset + i];
        }
        snap.lastSymbol = entry.lastSymbol;
        snap.bridgeDepth = entry.bridgeDepth;
        snap.bridgeEnters = entry.bridgeEnters;
        snap.bridgeSymbol = entry.bridgeSymbol;
        snap.root = entry.root;
        snap.exited = entry.exited;
        snap.host = entry.host;
        snap.ringDumped = entry.ringDumped;
        if (!entry.exited && snap.delta > 0) {
            ++moving;
        }
        total += entry.imports;
        if (count < kCensusCapacity) {
            ++count;
        }
    }
    const unsigned int sweep = gCensusSweeps;
    gCensusReported += static_cast<unsigned int>(count);
    mutexUnlock(&gCensusLock);

    // Sized for the full 8-entry ring plus the inbridge breadcrumb; at 192 the
    // last ring address was being truncated.
    char details[384];
    __builtin_snprintf(details, sizeof(details),
                       "elapsed_ms=%llu threads=%zu moving=%zu total_imports=%llu "
                       "sweep_lines=%llu", (unsigned long long)elapsedMs, count,
                       moving, (unsigned long long)total,
                       (unsigned long long)gCensusReported);    censusLog( "census-sweep", sweep, total, details);
    censusWaitLedger(sweep);
    censusMutexLedger(sweep);
    for (size_t i = 0; i < frozenCount; ++i) {
        mutexLock(&gCensusLock);
        CensusEntry* target = censusLookup(frozenKeys[i]);
        mutexUnlock(&gCensusLock);
        if (target != nullptr) {
            censusNoteStall(target);
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const CensusSnapshot& snap = gCensusSnapshot[i];
        // Clamped, for the same reason and with the same fix as the pc pass's
        // idleMs: `now` was sampled once at the top of the sweep, and
        // snap.lastTick is written by the import path with no lock, so a thread
        // that reached an import during this sweep has a lastTick NEWER than
        // `now`. The old guard tested lastTick >= firstTick, which is a
        // "has it ever imported" test and says nothing about which side of
        // `now` it falls on, so it let the wrap straight through. This asks the
        // only question the subtraction needs answered.
        const uint64_t idleMs = (snap.lastTick != 0 && now > snap.lastTick)
            ? censusMillis(now - snap.lastTick) : 0;
        char ring[192]{};
        size_t ringLength = 0;
        for (uint32_t r = 0; r < snap.recentCount && r < kCensusTail; ++r) {
            const int written = __builtin_snprintf(
                ring + ringLength, sizeof(ring) - ringLength, "%s%llx",
                r == 0 ? "" : " ",
                (unsigned long long)snap.recentReturns[r]);
            if (written <= 0) {
                break;
            }
            ringLength += static_cast<size_t>(written);
            if (ringLength >= sizeof(ring) - 1) {
                break;
            }
        }
        __builtin_snprintf(details, sizeof(details),
                           "start=0x%llx creator=%llu delta=%llu idle_ms=%llu "
                           "last=%s inbridge=%u/%s enters=%llu exited=%d host=%d "
                           "root=%d lr_ring=%s",
                           (unsigned long long)snap.startRoutine,
                           (unsigned long long)snap.creatorOrdinal,
                           (unsigned long long)snap.delta,
                           (unsigned long long)idleMs,
                           snap.lastSymbol != nullptr ? snap.lastSymbol : "(none)",
                           snap.bridgeDepth,
                           snap.bridgeSymbol != nullptr ? snap.bridgeSymbol : "guest",
                           (unsigned long long)snap.bridgeEnters,
                           snap.exited ? 1 : 0, snap.host ? 1 : 0,
                           snap.root ? 1 : 0, ring);
        censusLog( "census-ord", snap.ordinal,
                            snap.imports, details);
        // A thread that stopped advancing is the one whose call chain explains
        // the freeze. Dump its whole ring once, oldest first, so the ordered
        // sequence is in the log instead of only the last four addresses.
        if (!snap.exited && snap.delta == 0 && snap.imports > 0) {
            censusDumpRing(snap.ordinal);
        }
        // A thread that never stalls can still be the one burning the CPU, and
        // the frozen-only rule above meant the highest-delta thread in the run
        // never printed its symbol names. Once per thread, at a threshold well
        // below any real work load, so the cycle can be read as symbols instead
        // of guessed at from raw addresses.
        if (!snap.ringDumped && snap.imports >= kCensusSpinRingImports) {
            censusDumpRing(snap.ordinal);
        }
    }
    mutexLock(&gCensusLock);
    for (CensusEntry& entry : gCensus) {
        if (entry.used) {
            entry.reportedImports = entry.imports;
        }
    }
    mutexUnlock(&gCensusLock);
    mutexUnlock(&gCensusSnapLock);
}

void censusMaybeSweep() {
    if (!gCensusArmed) {
        return;
    }
    const uint64_t now = censusTick();
    if (gCensusSweepTick == 0) {
        gCensusSweepTick = now;
        gCensusFirstTick = now;
        return;
    }
    if (now - gCensusSweepTick < kCensusSweepSeconds * censusTickFreq()) {
        return;
    }
    // Deliberately the LAST thing this function can do, and it cannot block.
    //
    // This is called from two places, and that is what made it lethal. One is
    // censusReporterWorker, the other is censusNoteImportFrom on every 64th
    // import of every thread -- a guest-visible path, at thousands of calls per
    // second. censusSweep used to take gCensusSnapLock unconditionally, so:
    //
    //   1. the first caller past the 4 s gate entered censusSweep and, whatever
    //      it then blocked on inside, never came out;
    //   2. it held gCensusSnapLock (censusSweep) and/or a batch scope for the
    //      rest of the process;
    //   3. every later caller queued on that lock -- including the reporter's
    //      own censusMaybeSweep() call, which sits BEFORE censusSampleThreadPcs
    //      in the reporter loop, so the PC pass stopped being reached at all.
    //
    // Measured consequence on 20261004-010009-fpwalk-idle: 2 PC passes in 150 s
    // (4 census-pc lines, every one samples=1, i.e. every one a thread's FIRST
    // sample) and ZERO census-sweep lines in a 150 s run -- a sweep that could
    // not have been skipped by the 4 s gate, called ~4000 times a second by the
    // import path. The reporter was not starved of CPU and the pass body was
    // not looping; it was parked on a lock owned by a sweep that never
    // returned.
    //
    // The fix is to make losing the race a SKIP rather than a wait. The sweep
    // rate that matters (4 s) is orders of magnitude slower than the attempt
    // rate, so dropping attempts costs nothing, and no caller -- guest thread
    // or reporter -- can now be blocked by the census.
    censusSweep(now);
}

// Defined below, next to the mutex ledger it reads. Forward-declared here so
// the bounded stall path can call it without pulling the registry table up.
void censusLibcxxSyncProbe(uint32_t ordinal);

void censusNoteStall(CensusEntry* entry) {
    const uint64_t now = censusTick();
    // Clamped, same as the two idle_ms above and for the same reason: this is
    // called from censusSweep with a `now` from the top of the sweep, against
    // an entry whose lastTick any thread's import path may have advanced past
    // it in the meantime. On the import path itself (censusNoteImportFrom) the
    // same thread has just written lastTick, so that caller could not wrap --
    // this one can.
    const uint64_t idleMs = censusMillis(
        (entry->lastTick != 0 && now > entry->lastTick)
            ? now - entry->lastTick : 0);
    if (idleMs < kCensusStallSeconds * 1000ULL) {
        return;
    }
    mutexLock(&gCensusLock);
    if (entry->stallReported || entry->exited) {
        mutexUnlock(&gCensusLock);
        return;
    }
    entry->stallReported = true;
    const uint64_t imports = entry->imports;
    const uintptr_t start = entry->startRoutine;
    const char* lastSymbol = entry->lastSymbol;
    const uint64_t creatorOrdinal = entry->creatorOrdinal;
    const uint32_t recentCount = entry->recentCount;
    // Only the tail is needed on this one-line summary; the full ordered
    // history is censusDumpRing's job, and copying 512 entries onto the stack
    // of whichever guest thread stalled would be needlessly expensive.
    const uint32_t tailCount = recentCount < kCensusTail
        ? recentCount : kCensusTail;
    const uint32_t tailOffset = recentCount - tailCount;
    uintptr_t recent[kCensusTail]{};
    for (uint32_t i = 0; i < tailCount; ++i) {
        recent[i] = entry->recentReturns[tailOffset + i];
    }
    mutexUnlock(&gCensusLock);
    char ring[192]{};
    size_t ringLength = 0;
    for (uint32_t r = 0; r < tailCount; ++r) {
        const int written = __builtin_snprintf(ring + ringLength,
            sizeof(ring) - ringLength, "%s%llx", r == 0 ? "" : " ",
            (unsigned long long)recent[r]);
        if (written <= 0) {
            break;
        }
        ringLength += static_cast<size_t>(written);
        if (ringLength >= sizeof(ring) - 1) {
            break;
        }
    }
    char details[320];
    __builtin_snprintf(details, sizeof(details),
                       "start=0x%llx creator=%llu imports=%llu idle_ms=%llu last=%s "
                       "lr_ring=%s",
                       (unsigned long long)start,
                       (unsigned long long)creatorOrdinal,
                       (unsigned long long)imports, (unsigned long long)idleMs,
                       lastSymbol != nullptr ? lastSymbol : "(none)", ring);
    censusLog( "census-stall", entry->ordinal, imports,
                        details);
}

// ---------------------------------------------------------------------------
// Wedge-time pc burst.
//
// Why this exists, and why the pc sweep cannot answer the question on its own.
// The sweep accumulates a thread's top pcs over its WHOLE life into four slots,
// so "svcSendSyncRequest appeared twice" cannot be dated: it may be from boot,
// long before the wedge. And at the observed hit rate -- 212 of 220 root samples
// came back as the 0x4 sentinel -- the sweep's ~1.8 Hz cadence barely samples
// the one moment that matters. The burst trades breadth for density, and only
// fires when the idle gate has already decided the thread has stopped advancing
// (so it measures the wedge, not the boot).
//
// It also settles a comparison the sweep cannot. Threads that are genuinely
// parked in a libnx wrapper DO report a pc (the sweep shows every non-root
// thread sitting at svcSleepThread+4 / svcSendSyncRequest+4 for 200+ samples).
// So a root that reports the sentinel is in a different state from "parked in a
// libnx wrapper": either it is executing JIT-compiled guest code, or it is in a
// kernel transition. Those two have opposite fixes, and the burst's live-sample
// count separates them.
//
// Bounded and cheap: 512 iterations, two syscalls each, fired 3 times per run
// (kFpWalkRepeats) and only for the root thread. The pause is released BEFORE
// anything is logged or classified, so a logging failure cannot leave the main
// guest thread parked -- that mistake turns "frozen" into "wedged".
//
// Every distinct pc is attributed with censusClassifyRegister, so a hit inside a
// guest image is reported as an image offset and a hit inside the NRO as a
// link-time vaddr that nm can name. Same attribution rule as the ring, the walk
// and the register dump; a second rule here is how a wrong offset survives.
// ---------------------------------------------------------------------------
constexpr uint32_t kCensusPcBurstSamples = 512;
constexpr uint32_t kCensusPcBurstSlots = 8;

void censusPcBurst(uint32_t ordinal, uint32_t rawHandle) {
    const Handle handle = static_cast<Handle>(rawHandle);
    // Cached, and it has to be: this runs inside the same pass that called
    // censusCachedPlan() a moment ago, so the uncaptured form was paying a
    // guest-view fsync for a plan it already had in hand.
    const GuestStartupPlan plan = censusCachedPlan();
    const uintptr_t nroBase = censusNroBase();
    uintptr_t pcs[kCensusPcBurstSlots]{};
    uint32_t hits[kCensusPcBurstSlots]{};
    uint32_t slots = 0;
    uint32_t sentinel = 0, live = 0, ctxFail = 0, pauseFail = 0;
    for (uint32_t i = 0; i < kCensusPcBurstSamples; ++i) {
        if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) {
            ++pauseFail;
            continue;
        }
        ThreadContext context;
        std::memset(&context, 0, sizeof(context));
        const Result got = svcGetThreadContext3(&context, handle);
        if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Runnable))) {
            return;     // cannot safely keep touching this thread
        }
        if (R_FAILED(got)) {
            ++ctxFail;
            continue;
        }
        const uintptr_t pc = static_cast<uintptr_t>(context.pc.x);
        if (pc < kCensusPcSentinel) {
            ++sentinel;
            continue;
        }
        ++live;
        uint32_t s = 0;
        for (; s < slots; ++s) {
            if (pcs[s] == pc) {
                break;
            }
        }
        if (s == slots && slots < kCensusPcBurstSlots) {
            pcs[slots] = pc;
            ++slots;
        }
        if (s < kCensusPcBurstSlots) {
            ++hits[s];
        }
    }
    char detail[384];
    size_t at = 0;
    for (uint32_t s = 0; s < slots; ++s) {
        const RegisterClass rc = censusClassifyRegister(
            plan, nroBase, 0, static_cast<uintptr_t>(pcs[s]), false);
        const int written = __builtin_snprintf(
            detail + at, sizeof(detail) - at, "%s0x%llx:%u(%s+0x%llx)",
            s == 0 ? "" : " ", (unsigned long long)pcs[s], hits[s], rc.kind,
            (unsigned long long)rc.off);
        if (written <= 0) {
            break;
        }
        at += static_cast<size_t>(written);
    }
    char line[640];
    __builtin_snprintf(line, sizeof(line),
                       "samples=%u sentinel=%u live=%u ctx_fail=%u pause_fail=%u "
                       "slots=%u %s",
                       kCensusPcBurstSamples, sentinel, live, ctxFail, pauseFail,
                       slots, detail);
    censusLog("census-pcburst", ordinal, 0, line);
}

// ---------------------------------------------------------------------------
// Stack-frame dump for the DeviceInfo teardown.
//
// THE QUESTION. census-deinfo-bounds (below, in censusSampleThreadPcs) reads
// B at [fp-0x48] and E at [fp-0x40] and reports span = 0x25a9fc, which is not
// a multiple of 48, so the contiguous-storage destructor at 0xcd2abc cannot
// terminate. The explanation on offer is that the container was never
// constructed and the destructor is walking stack residue this frame never
// wrote. That explanation is currently an ARGUMENT FROM THE ARITHMETIC, not
// from memory: B and E are two words, and a container's construction state is
// not two words. Dumping the frame is what turns it into a thing that can be
// looked at -- are [fp-0x60..fp-0x20] zero, garbage, or a structure?
//
// WHY IT IS BUDGETED LIKE THIS. The frame is 0x5e0 = 1504 bytes. Printing all
// of it on every hit is 94 log lines per sample on an emulated SD card, and a
// probe that changes the timing of what it measures is worse than no probe. So
// three parts, 11 byte rows and 2 lines per dump:
//
//   [fp-0x80, fp-0x10]  112 bytes in full. Covers B and E plus the 24-byte
//                       libc++ string slots above them and the words below.
//                       This is where the answer lives.
//   [sp, sp+0x40]        64 bytes in full. Not for content -- to confirm sp is
//                       the frame bottom the arithmetic assumed. If those
//                       bytes are not plausible stack, fp - sp == 0x5e0 is not
//                       the frame size and B/E were read out of the wrong
//                       frame, which would invalidate the span entirely.
//   [sp, fp) remainder   one bit per 64-byte block: all-zero or not. Enough to
//                       say "the lower two thirds of this frame was never
//                       touched", which is the positive form of the
//                       never-constructed claim.
//
// Every read goes through censusReadStackWord, i.e. svcQueryMemory first. The
// two B/E words above are raw dereferences, which is safe only because they are
// two words 0x5e0 apart in the middle of a frame that has already been proven
// to exist; 1504 bytes of raw dereference is a different proposition.
// ---------------------------------------------------------------------------

// 0x5e0 / 64 = 23.5, so the last block of an observed frame is short and is
// classified on the bytes that are actually there.
constexpr size_t kFrameBlockBytes = 64;
// The block classification is a uint64 bitmask, so 64 blocks = 4096 bytes is the
// ceiling. The observed frame is 1504 (24 blocks). Anything larger is reported
// UNUSABLE with zero bytes read rather than silently clipped, because a clipped
// frame cannot answer a question about the bytes that were cut off.
constexpr uintptr_t kFrameDumpMaxBytes = 4096;
// Hits that actually dump. This gate is inside the block that already requires
// pc in the FDE range, so it is a rate limit on something already rare.
// Down from 4 to 2: the dump now covers the WHOLE frame (0x5e0 / 16 = 188 rows
// per hit), and two identical dumps already establish that the words are
// stable, while four would double the log for no extra information.
constexpr unsigned kFrameDumpHits = 2;

// kFrameHeadLo is where the dump starts at the top of the frame; the pass runs
// from there all the way down to sp, so kFrameHeadHi and kFrameFootBytes are
// gone. kFrameHeadLo is still used by the selftest for its row offsets.
constexpr intptr_t kFrameHeadLo = -0x80;

// Written only by the reporter thread: censusSampleThreadPcs has exactly one
// caller (censusReporter). A plain unsigned is therefore correct here and
// matches every other one-shot flag in this file. Deliberately NOT std::atomic
// and deliberately no lock of any kind -- this sits on the guest-facing census
// path, where a lock has already cost this project 187 s of CPU for nothing.
unsigned gCensusFrameDumps = 0;
bool gCensusFrameDumpCapped = false;

// Renders 8 bytes as 16 hex characters, in MEMORY order.
//
// The bytes come from memcpy, not from shifting the word. A shift-based render
// is a byte-order assumption, and getting that assumption backwards turns
// deadbeef into efbeadde -- which still LOOKS like a recognisable pattern, so
// any check of the form "is DE AD BE EF in this line" would have passed it. The
// self-test below caught exactly this, on the host, before any of it ran on
// hardware; it is the reason the comparison there is an exact string equality
// against an independently rendered expectation rather than a substring hunt.
//
// A word the probe could NOT read renders as ???????? and never as 16 zeros.
// This is not cosmetics: an all-zero word and an unreadable word are otherwise
// the same 16 characters in the log, and the whole reason this dump exists is
// to tell "the frame is empty" apart from "the frame was not read". A dumper
// that collapses those two is the failure mode being designed out.
void censusHexWord(char* out, uint64_t value, bool readable) {
    if (!readable) {
        for (int i = 0; i < 16; ++i) {
            out[i] = '?';
        }
        out[16] = '\0';
        return;
    }
    static const char kHex[] = "0123456789abcdef";
    unsigned char raw[8];
    std::memcpy(raw, &value, sizeof(raw));
    for (int i = 0; i < 8; ++i) {
        out[i * 2] = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0x0Fu];
    }
    out[16] = '\0';
}

// THE DUMPER. census-deinfo-frame and census-deinfo-frame-selftest both go
// through this function and nothing else, which is what makes the self-test a
// statement about the real dump rather than about a second formatter.
//
// `offsetFromFp` is always measured from fp, never from sp and never absolute,
// so a row can be located in the frame without every line also carrying an
// address. `rendered`, when non-null, receives the exact 32 characters that
// were logged; the self-test compares it against its own expectation and
// nothing else uses it.
void censusFrameDumpRow(const char* phase, unsigned long long ordinal,
                        unsigned hit, long long offsetFromFp,
                        uintptr_t address, uint64_t w0, bool r0,
                        uint64_t w1, bool r1, char* rendered) {
    char hex[33];
    censusHexWord(hex, w0, r0);
    censusHexWord(hex + 16, w1, r1);
    char line[224];
    __builtin_snprintf(line, sizeof(line),
                       "hit=%u off=0x%llx addr=0x%llx bytes=%s",
                       hit, offsetFromFp,
                       (unsigned long long)address, hex);
    if (rendered != nullptr) {
        std::memcpy(rendered, hex, sizeof(hex));
    }
    censusLog(phase, ordinal, 0, line);
}

// ---------------------------------------------------------------------------
// Self-test. Runs ONCE, at censusArm, before the first real frame can be read.
//
// THE DISCIPLINE. A probe that reports "the frame contains nothing" is
// indistinguishable from a probe whose dumper is broken, and this project has
// already retracted conclusions that came from exactly that confusion. So the
// dumper is pointed at a buffer filled here with a recognisable poison pattern,
// and the output is compared against an expectation built INDEPENDENTLY of the
// formatter. If the poison is not in the log, every negative reading from
// census-deinfo-frame is void -- not "probably void", void.
//
// The pattern is not a repeated constant. 0xDEADBEEF with an incrementing
// counter in every 8th byte means a row that is reordered, truncated, or
// zero-padded cannot pass, and the counter makes the row's POSITION visible in
// the log rather than merely its content.
//
// Two implementations of the rendering, on purpose: selftestExpectedWord below
// does not call censusHexWord, because a self-test that reuses the code it is
// testing cannot fail.
// ---------------------------------------------------------------------------
void censusFrameDumpSelftest() {
    static const unsigned char kPoison[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    enum : unsigned { kPoisonBytes = 32, kPoisonCounterBase = 0x40 };
    unsigned char buf[kPoisonBytes];
    for (unsigned i = 0; i < kPoisonBytes; ++i) {
        buf[i] = ((i & 7u) == 4u)
            ? static_cast<unsigned char>(kPoisonCounterBase + (i >> 3))
            : kPoison[i & 3u];
    }
    // Second implementation of the hex rendering, deliberately not sharing code
    // with censusHexWord. It takes bytes in memory order and emits big-endian
    // hex, exactly as censusHexWord does for a word read from memory; if the two
    // ever disagree about byte order the comparison below fails.
    const auto expectedWord = [](char* out, const unsigned char* bytes) {
        static const char kHex[] = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) {
            const unsigned b = bytes[i];
            out[i * 2] = kHex[(b >> 4) & 0xFu];
            out[i * 2 + 1] = kHex[b & 0xFu];
        }
        out[16] = '\0';
    };

    // Row 0: buf[0..16) -> deadbeef40adbeef / deadbeef41adbeef is row 1. Read
    // through memcpy, not a reinterpret_cast: buf is byte-aligned and casting it
    // to uint64_t* is undefined behaviour even where it happens to work.
    char want[33];
    char got[33];
    got[0] = '\0';
    bool ok = true;

    unsigned char first[16];
    std::memcpy(first, buf, sizeof(first));
    expectedWord(want, first);
    expectedWord(want + 16, first + 8);
    uint64_t w0 = 0, w1 = 0;
    std::memcpy(&w0, buf, sizeof(w0));
    std::memcpy(&w1, buf + 8, sizeof(w1));
    censusFrameDumpRow("census-deinfo-frame-selftest", 0, 1,
                       static_cast<long long>(kFrameHeadLo), 0, w0, true, w1, true,
                       got);
    const bool rowOk = std::strcmp(got, want) == 0;
    ok = ok && rowOk;

    unsigned char second[16];
    std::memcpy(second, buf + 16, sizeof(second));
    expectedWord(want, second);
    expectedWord(want + 16, second + 8);
    got[0] = '\0';
    std::memcpy(&w0, buf + 16, sizeof(w0));
    std::memcpy(&w1, buf + 24, sizeof(w1));
    censusFrameDumpRow("census-deinfo-frame-selftest", 0, 2,
                       static_cast<long long>(kFrameHeadLo) + 16, 0,
                       w0, true, w1, true, got);
    const bool secondOk = std::strcmp(got, want) == 0;
    ok = ok && secondOk;

    // The unreadable rendering gets its own check, because it is the one that
    // matters most: an unmapped stack is exactly the situation in which this
    // dump is asked "is there anything there?", and 32 question marks is the
    // whole difference between "nothing" and "could not look".
    got[0] = '\0';
    for (int i = 0; i < 32; ++i) {
        want[i] = '?';
    }
    want[32] = '\0';
    censusFrameDumpRow("census-deinfo-frame-selftest", 0, 3,
                       static_cast<long long>(kFrameHeadLo) + 32, 0,
                       0, false, 0, false, got);
    const bool unreadableOk = std::strcmp(got, want) == 0;
    ok = ok && unreadableOk;

    char line[320];
    __builtin_snprintf(line, sizeof(line),
                       "poison=%02x%02x%02x%02x%02x%02x%02x%02x/%02x%02x%02x%02x%02x%02x%02x%02x"
                       " row0_match=%d row1_match=%d unreadable_match=%d "
                       "verdict=%s note=negative_readings_from_census-deinfo-frame_"
                       "are_only_valid_when_this_says_SELFTEST_PASS",
                       buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6],
                       buf[7], buf[8], buf[9], buf[10], buf[11], buf[12], buf[13],
                       buf[14], buf[15],
                       rowOk ? 1 : 0, secondOk ? 1 : 0, unreadableOk ? 1 : 0,
                       ok ? "SELFTEST_PASS" : "SELFTEST_FAIL");
    censusLog("census-deinfo-frame-selftest", 0, 0, line);
}

// Called from the one in-range hit of census-deinfo-bounds, so the rate limit
// sits behind that gate and not in front of it.
void censusFrameDump(unsigned long long ordinal, uintptr_t sp, uintptr_t fp,
                     uint64_t pcOff) {
    const unsigned hit = ++gCensusFrameDumps;
    char line[512];
    if (hit > kFrameDumpHits) {
        // Said once, so "the dump stopped" is distinguishable from "the gate
        // never fired". Without this the log just stops, and a silent stop is
        // indistinguishable from a probe that found nothing to say.
        if (!gCensusFrameDumpCapped) {
            gCensusFrameDumpCapped = true;
            __builtin_snprintf(line, sizeof(line),
                "hit=%u off=n/a addr=n/a CAPPED hits_limit=%u "
                "hits_total=%u note=silent_after_this_line",
                hit, kFrameDumpHits, gCensusFrameDumps);
            censusLog("census-deinfo-frame", ordinal, 0, line);
        }
        return;
    }

    const uintptr_t frameBytes = fp > sp ? (fp - sp) : 0;
    if (frameBytes == 0 || frameBytes > kFrameDumpMaxBytes) {
        __builtin_snprintf(line, sizeof(line),
            "hit=%u off=n/a addr=n/a UNUSABLE sp=0x%llx fp=0x%llx "
            "frame_bytes=0x%llx limit=0x%llx pc_off=0x%llx "
            "note=no_bytes_read verdict=FRAME_BOUNDS_NOT_USABLE",
            hit, (unsigned long long)sp, (unsigned long long)fp,
            (unsigned long long)frameBytes,
            (unsigned long long)kFrameDumpMaxBytes,
            (unsigned long long)pcOff);
        censusLog("census-deinfo-frame", ordinal, 0, line);
        return;
    }

    // WHOLE frame, every 16 bytes, one pass.
    //
    // This was head [fp-0x80, fp-0x10) plus foot [sp, sp+0x40) = 0xb0 of the
    // observed 0x5e0, i.e. 11.7%. The 0x520 in between was never read, and the
    // one slot the whole root-cause hypothesis rests on -- fp-0xC0, which is
    // STACK0-0xa8 and the {begin,end} pair the destructor reads -- sits in that
    // hole. `zero_mask` could not substitute for it either: it is a 64-byte
    // block bitmask and its origin was not stated in the log, so bit 1 could be
    // read two ways and neither was decidable from the line.
    //
    // So: dump all of it, and say what block_n's base address is, in the log.
    // Rows walk DOWN from fp-0x80 to sp so the head the previous runs already
    // covered still reads first, in the same order.
    const uintptr_t lowestRow = (fp - kFrameHeadLo) & ~static_cast<uintptr_t>(15);
    for (uintptr_t address = fp + static_cast<uintptr_t>(kFrameHeadLo);
         address >= sp && address >= lowestRow; address -= 16) {
        uint64_t w0 = 0, w1 = 0;
        const bool r0 = censusReadStackWord(address, &w0);
        const bool r1 = censusReadStackWord(address + 8, &w1);
        censusFrameDumpRow("census-deinfo-frame", ordinal, hit,
                           static_cast<long long>(
                               static_cast<intptr_t>(address) -
                               static_cast<intptr_t>(fp)),
                           address, w0, r0, w1, r1, nullptr);
    }
    // The old foot loop is gone: the pass above reaches sp, so keeping it would
    // print the same words twice and a repeated row reads as two independent
    // observations of one word.

    // Whole-frame classification. One pass, 64-byte granularity, accumulated by
    // OR: a block is "all zero" only if every readable word in it is zero, and a
    // block with any unreadable word is reported in unreadable_mask instead --
    // it is not counted as zero, because an unreadable block is not evidence of
    // emptiness.
    uint64_t zeroMask = 0;
    uint64_t unreadableMask = 0;
    const size_t blocks =
        static_cast<size_t>((frameBytes + kFrameBlockBytes - 1) / kFrameBlockBytes);
    for (size_t b = 0; b < blocks; ++b) {
        const uintptr_t base = sp + b * kFrameBlockBytes;
        const size_t remaining = static_cast<size_t>(fp - base);
        const size_t length =
            remaining < kFrameBlockBytes ? remaining : kFrameBlockBytes;
        uint64_t acc = 0;
        bool allReadable = true;
        for (size_t o = 0; o + 8 <= length; o += 8) {
            uint64_t word = 0;
            if (!censusReadStackWord(base + o, &word)) {
                allReadable = false;
                continue;
            }
            acc |= word;
        }
        if (!allReadable) {
            unreadableMask |= (1ULL << b);
        } else if (acc == 0) {
            zeroMask |= (1ULL << b);
        }
    }

    __builtin_snprintf(line, sizeof(line),
        "hit=%u off=SUMMARY addr=n/a sp=0x%llx fp=0x%llx frame_bytes=0x%llx "
        "blocks=%zu block_bytes=%zu zero_mask=0x%llx unreadable_mask=0x%llx "
        "rows=full_frame_16B_each stack0=0x%llx fp_minus_stack0=0x%llx "
        "pc_off=0x%llx "
        "note=zero_mask_bit_n_means_block_at_sp_plus_n*block_bytes_is_all_zero; "
        "this_function_has_x29_equals_STACK0_minus_0x60_so_an_fp_relative_off_o_"
        "is_STACK0_relative_o_plus_0x60",
        hit, (unsigned long long)sp, (unsigned long long)fp,
        (unsigned long long)frameBytes, blocks, kFrameBlockBytes,
        (unsigned long long)zeroMask, (unsigned long long)unreadableMask,
        (unsigned long long)(fp + 0x60ULL), 0x60ULL,
        (unsigned long long)pcOff);
    censusLog("census-deinfo-frame", ordinal, 0, line);
}

// Pauses every live census-tracked thread in turn and classifies where it is.
//
// Why this exists: the sweep already says a thread stopped advancing
// (delta == 0), but not whether it is spinning in guest code, parked in one of
// our bridges, or blocked. A per-thread pc answers that directly, and the
// contrast between a thread returning a real pc and one returning the
// platform's sentinel is the discriminator.
//
// Safety: the (ordinal, handle) list is snapshotted under gCensusLock and the
// lock is RELEASED before any thread is paused. Pausing a thread while holding
// a lock it might be waiting for is how a diagnostic deadlocks the guest.
// gCensusSnapLock > gCensusLock > gCensusLogLock still holds.
void censusSampleThreadPcs() {
    struct Target {
        uint32_t ordinal;
        uint32_t handle;
    };
    Target targets[kCensusCapacity];
    size_t count = 0;
    mutexLock(&gCensusLock);
    for (const CensusEntry& entry : gCensus) {
        if (!entry.used || !entry.attached || entry.exited
            || entry.kernelHandle == 0) {
            continue;
        }
        if (count < kCensusCapacity) {
            targets[count].ordinal = entry.ordinal;
            targets[count].handle = entry.kernelHandle;
            ++count;
        }
    }
    mutexUnlock(&gCensusLock);
    if (count == 0) {
        return;
    }

    ++gCensusPcPass;
    const uintptr_t bias = resolveMainImageLoadBias();
    // One tick read for the whole pass, used for the PASS-level bookkeeping
    // below (publish gate, fpwalk spacing). It is deliberately NOT used for the
    // idle figure: entry.lastTick is written by censusNoteImportFrom on the
    // import path with no lock, so a thread that imports while this pass is
    // walking the threads ahead of it ends up with a lastTick newer than this
    // one. Each thread re-reads its own tick below.
    //
    // (This comment used to claim armGetSystemTick was a syscall and justify
    // hoisting it out of the lock for that reason. It is `mrs x, cntpct_el0`,
    // a single register read -- libnx include/switch/arm/counter.h. The hoisting
    // is still right, but for the ordinary reason: do not put anything in a
    // critical section that the 255 other census slots can be waiting behind.)
    const uint64_t nowTick = censusTick();
    // Cached once per pass, not once per thread: captureGuestStartupPlan()
    // fsyncs a log line on every call. See censusCachedPlan.
    const GuestStartupPlan plan = censusCachedPlan();
    const uintptr_t nroBase = censusNroBase();
    const unsigned pass = gCensusPcPass;
    CensusBatchScope batch;
    for (size_t i = 0; i < count; ++i) {
        const Handle handle = static_cast<Handle>(targets[i].handle);
        const Result paused = svcSetThreadActivity(handle,
                                                   ThreadActivity_Paused);
        if (R_FAILED(paused)) {
            mutexLock(&gCensusLock);
            for (CensusEntry& entry : gCensus) {
                if (entry.used && entry.ordinal == targets[i].ordinal) {
                    ++entry.pcPauseFail;
                }
            }
            mutexUnlock(&gCensusLock);
            continue;
        }
        ThreadContext context;
        std::memset(&context, 0, sizeof(context));
        const Result got = svcGetThreadContext3(&context, handle);
        // Always resume before doing anything else: leaving a guest thread
        // paused turns "frozen" into "wedged".
        const Result resumed = svcSetThreadActivity(handle,
                                                    ThreadActivity_Runnable);
        if (R_FAILED(resumed)) {
            // This one thread can no longer be touched safely, so skip it --
            // but keep going. Returning here dropped every LATER thread in the
            // pass, which is how a single unresumable thread could silence the
            // report for the very threads being hunted.
            continue;
        }

        const uint64_t pc = static_cast<uint64_t>(context.pc.x);
        const uint64_t lr = static_cast<uint64_t>(context.lr);
        const uintptr_t sp = static_cast<uintptr_t>(context.sp);
        const bool ctxOk = !R_FAILED(got);
        const bool sentinel = !ctxOk || pc < kCensusPcSentinel;
        // Classify BEFORE taking gCensusLock. censusClassifyRegister can issue
        // svcQueryMemory, and a syscall under the census lock is exactly what
        // this path must never do. For a pc that lands in a guest image it
        // issues none at all -- the plan answers from memory.
        const RegisterClass pcRc = censusClassifyRegister(
            plan, nroBase, sp, static_cast<uintptr_t>(pc), true);
        const RegisterClass lrRc = censusClassifyRegister(
            plan, nroBase, sp, static_cast<uintptr_t>(lr), false);
        // The whole point of this report. A thread whose pc is NOT in a guest
        // image cannot be reported with an elfva, and is not silently given
        // one; a failed or sentinel read cannot either, and says so via
        // pc_ctx / verdict instead of printing a zero as if it were a pc.
        const bool pcGuest = ctxOk && !sentinel && censusIsGuestImage(pcRc);

        // ---- The DeviceInfo vector destructor: B and E, read at the only
        // instant they are meaningful. -------------------------------------
        //
        // Guest [0xcd1014, 0xcd3648) assembles the whole DeviceInfo table and
        // then tears it down. The teardown's element loop lives at
        // 0xcd2abc..0xcd2b08 and is a CONTIGUOUS-STORAGE destructor, not a
        // linked-list walk: there is no `next` pointer load anywhere, the
        // cursor moves by a constant 0x30 on both branches
        //   0xcd2ad4 / 0xcd2aec  sub x23, x21, #0x30
        // and the exit is textbook libc++ vector teardown
        //   0xcd2b00 ldur x0,[x29,#-0x48]   ; x0 = B
        //   0xcd2b04 stur x20,[x29,#-0x40]   ; E := B
        //   0xcd2b08 bl   operator delete
        //
        // So its termination is pure arithmetic: cursor(k) = E - 48k, which
        // reaches B iff E >= B and (E-B) % 48 == 0. Two words answer it.
        //
        // WHY HERE AND NOT A GUEST-CODE PROBE. Guest .text lives in a different
        // memory block and `svcSetMemoryPermission` fails on it -- see
        // guest_view_probe.cpp:2165, "NOP patch removed: svcSetMemoryPermission
        // fails on guest .text". Patching 0xcd2aa0 is therefore not available
        // on this port. The sampler already holds the target's full context at
        // exactly the right instant, and the frame pointer is stable for the
        // duration of the call, so B and E can simply be read here.
        //
        // RANGE TEST USES pcRc.off, never the raw pc: pc carries the load bias
        // and changes every run, off is the bias-free ELF offset.
        //
        // The plausibility guard is REPORTED, never silent: a run where the
        // guard trips says so on its own line, because "the probe found
        // nothing" and "the probe could not look" must not look the same.
        // The B/E read is gated on the pc landing in the function, and that gate
        // has a real cost: the sweep samples each thread a few times per run
        // while this function is a tiny fraction of execution, so a run can
        // produce ZERO lines. Every earlier estimate of that yield has been a
        // guess, and a guess about a probe's hit rate is how this file keeps
        // producing wrong conclusions. So the line is emitted for EVERY guest
        // sample, with the offset, and B/E are read only when in range: one run
        // then turns the hit rate into a measured number instead of an
        // assumption, and a run with zero in-range samples is now
        // distinguishable from a run where the gate never ran.
        static constexpr uint64_t kDevInfoFdeLo = 0xcd1014ULL;
        static constexpr uint64_t kDevInfoFdeHi = 0xcd3648ULL;
        if (pcGuest) {
            const bool inRange = pcRc.off >= kDevInfoFdeLo && pcRc.off <= kDevInfoFdeHi;
            char line[288];
            if (!inRange) {
                __builtin_snprintf(line, sizeof(line),
                                   "OUT-OF-RANGE pc_off=0x%llx (B/E not read)",
                                   (unsigned long long)pcRc.off);
                censusLog("census-deinfo-bounds", targets[i].ordinal, 0, line);
            } else {
            const uintptr_t fp = static_cast<uintptr_t>(context.fp);
            if (fp < 0x10000ULL || fp > 0x0000800000000000ULL) {
                __builtin_snprintf(line, sizeof(line),
                                   "GUARD-TRIPPED fp=0x%llx pc_off=0x%llx "
                                   "(frame implausible, B/E NOT read)",
                                   (unsigned long long)fp,
                                   (unsigned long long)pcRc.off);
                censusLog("census-deinfo-bounds", targets[i].ordinal, 0, line);
            } else {
                const uint64_t bWord = *reinterpret_cast<const uint64_t*>(fp - 0x48);
                const uint64_t eWord = *reinterpret_cast<const uint64_t*>(fp - 0x40);
                const bool ordered = eWord >= bWord;
                const uint64_t span = ordered ? (eWord - bWord) : 0;
                // 48 is NOT a power of two: `(span & 47)` is wrong here, and
                // writing it would be exactly the kind of near-miss that this
                // file keeps retracting.
                const bool strideOk = ordered && (span % 48ULL == 0ULL);
                __builtin_snprintf(line, sizeof(line),
                                   "fp=0x%llx sp=0x%llx fp_minus_sp=0x%llx "
                                   "expect_0x5e0=%d  B=0x%llx E=0x%llx "
                                   "span=0x%llx(%llu) E_ge_B=%d mod48=%d "
                                   "verdict=%s pc_off=0x%llx",
                                   (unsigned long long)fp,
                                   (unsigned long long)sp,
                                   (unsigned long long)(fp - sp),
                                   (fp - sp) == 0x5e0 ? 1 : 0,
                                   (unsigned long long)bWord,
                                   (unsigned long long)eWord,
                                   (unsigned long long)span,
                                   (unsigned long long)span,
                                   ordered ? 1 : 0, strideOk ? 1 : 0,
                                   (!ordered)   ? "E_LT_B_NEVER_TERMINATES"
                                   : strideOk    ? "TERMINATES"
                                                 : "SPAN_NOT_MULTIPLE_OF_48",
                                   (unsigned long long)pcRc.off);
                censusLog("census-deinfo-bounds", targets[i].ordinal, 0, line);
                // The frame itself, on the same in-range hit. B and E are two
                // words and they cannot distinguish "this container was built"
                // from "this container was never built"; the rest of the frame
                // can. Placed here rather than behind the verdict test so a
                // TERMINATING span still yields a frame too -- the dump exists
                // to say what the frame looked like, not only to confirm a
                // guess already made about it.
                censusFrameDump(targets[i].ordinal, sp, fp, pcRc.off);
            }
            }
        }
        uint32_t samples = 0, sentinelCount = 0, pauseFail = 0, ctxFail = 0;
        uint32_t top[kCensusPcSlots]{};
        uint32_t topHits[kCensusPcSlots]{};
        uintptr_t startRoutine = 0;
        uint64_t lastTick = 0;
        bool exited = false;
        bool wantFpWalk = false;
        bool wantSpinWalk = false;
        bool wantStallWalk = false;
        bool spin = false;
        uint32_t pcStable = 0;
        uint32_t lrStable = 0;
        uint32_t importsDelta = 0;
        bool publish = false;
        // A FRESH tick per thread, read outside gCensusLock, and it is the tick
        // the idle figure below is measured against. The pass-wide nowTick is
        // not: it is sampled once at the top of the pass, and entry.lastTick is
        // written by censusNoteImportFrom (censusNoteImportFrom, the
        // ~265k calls/second path) with no lock, so any thread that reaches an
        // import while this pass is walking the threads ahead of it ends up
        // with a lastTick NEWER than nowTick. `nowTick - lastTick` is unsigned,
        // so that does not read as a small negative -- it wraps to ~2^64 and
        // censusMillis turns it into a number with no unit. Measured on
        // 20261004-010009-fpwalk-idle: ordinal 4 reported idle_ms=
        // 960767920499, which is exactly 2^64 - 128751616 ticks at the 19.2 MHz
        // Switch counter, i.e. that thread's lastTick was 6.706 s AHEAD of the
        // pass's nowTick. Two things follow, and both are fixed below: the pass
        // body really is ~6.7 s long, and an unclamped subtraction turns a
        // thread that imported 2 ms ago into "idle since the epoch".
        // armGetSystemTick() is `mrs x, cntpct_el0` (libnx
        // include/switch/arm/counter.h), a single register read and not a
        // syscall, so one per thread per pass costs nothing measurable. Read
        // here, before the lock, so the critical section still contains no
        // register read the other 255 census slots can be waiting behind.
        const uint64_t threadTick = censusTick();
        // Filled in under the lock, printed after the unlock. The printed line
        // used to recompute this from nowTick a second time, which meant a
        // second copy of the underflow to keep fixed; there is now one.
        uint64_t idleMs = 0;
        mutexLock(&gCensusLock);
        for (CensusEntry& entry : gCensus) {
            if (!entry.used || entry.ordinal != targets[i].ordinal) {
                continue;
            }
            if (sentinel) {
                ++entry.pcSentinel;
            }
            else {
                bool placed = false;
                for (size_t s = 0; s < kCensusPcSlots && !placed; ++s) {
                    if (entry.pcTop[s] == static_cast<uint32_t>(pc)) {
                        ++entry.pcTopHits[s];
                        placed = true;
                    }
                    else if (entry.pcTop[s] == 0) {
                        entry.pcTop[s] = static_cast<uint32_t>(pc);
                        entry.pcTopHits[s] = 1;
                        placed = true;
                    }
                }
            }
            ++entry.pcSamples;
            samples = entry.pcSamples;
            sentinelCount = entry.pcSentinel;
            pauseFail = entry.pcPauseFail;
            ctxFail = R_FAILED(got) ? entry.pcCtxFail + 1 : entry.pcCtxFail;
            if (R_FAILED(got)) {
                entry.pcCtxFail = ctxFail;
            }
            for (size_t s = 0; s < kCensusPcSlots; ++s) {
                top[s] = entry.pcTop[s];
                topHits[s] = entry.pcTopHits[s];
            }
            startRoutine = entry.startRoutine;
            lastTick = entry.lastTick;
            exited = entry.exited;
            // The two numbers the spin rule is built from: how many passes in
            // a row the pc has been bit-identical, and whether the thread
            // touched the bridge since the previous pass. 64-bit pc: a guest
            // pc is 0x2107...... and does not fit the 32-bit ring above.
            //
            // First sample has no previous sample to difference against, so the
            // delta is 0 by definition. Left alone it would print the thread's
            // whole LIFETIME import count cast down to 32 bits -- a real
            // looking number meaning nothing, which is the exact failure mode
            // this file keeps warning about.
            if (samples == 1) {
                entry.pcLastImports = entry.imports;
                importsDelta = 0;
            }
            else {
                importsDelta = static_cast<uint32_t>(
                    entry.imports - entry.pcLastImports);
                entry.pcLastImports = entry.imports;
            }
            if (!sentinel && pc == entry.pcPrev) {
                ++entry.pcStable;
            }
            else {
                entry.pcPrev = sentinel ? 0 : pc;
                entry.pcStable = sentinel ? 0 : 1;
            }
            pcStable = entry.pcStable;
            // The mirror image of the rule above, and deliberately NOT keyed on
            // the sentinel: a blocked thread's lr is still its own. Reset only
            // when the context read itself failed, which is the one case where
            // the lr carries no information about the target either.
            if (!ctxOk) {
                entry.lrStable = 0;
            }
            else if (lr == entry.lrPrev) {
                ++entry.lrStable;
            }
            else {
                entry.lrPrev = lr;
                entry.lrStable = 1;
            }
            lrStable = entry.lrStable;
            // Stated rule, no heuristics: see kSpinStablePasses. unset means
            // "this instrument could not see where the thread is", which is a
            // different statement from "this thread is not spinning". On Eden
            // this is always "unset" -- see the note on kSpinStablePasses.
            spin = pcGuest && pcStable >= kSpinStablePasses
                && importsDelta == 0;
            // Publish gate, in priority order:
            //   1. the thread's first ever sample, so a new thread is never
            //      invisible;
            //   2. a guest-image pc, on a FIXED per-thread stride -- the old
            //      gap grew with every line printed, which is a doubling
            //      schedule whose arithmetic assumed a pass period the
            //      archive contradicts (see kPcGuestStride);
            //   3. otherwise a MEASURED wall-clock gate plus a pass-count
            //      ceiling, whichever comes first, so a thread whose budget
            //      is spent still reports for the rest of the run at a rate
            //      that does not depend on how long a pass happens to take.
            const bool guestDue = pcGuest
                && entry.pcGuestLines < kPcGuestBudget
                && (entry.pcGuestLines == 0
                    || pass - entry.pcLastGuestPass >= kPcGuestStride);
            publish = samples == 1 || guestDue
                || pass - entry.pcLastPublishPass >= kCensusPcPublishEvery
                || (entry.pcLastPublishTick != 0
                    && censusMillis(nowTick - entry.pcLastPublishTick)
                        >= kCensusPcPublishMs);
            if (publish) {
                entry.pcLastPublishPass = pass;
                entry.pcLastPublishTick = nowTick;
            }
            if (guestDue) {
                entry.pcLastGuestPass = pass;
                ++entry.pcGuestLines;
            }
            // idleMs is the measured wall clock since this thread last reached
            // a bridge import, read once and shared by the two walk gates below
            // and by the printed line. censusMillis is pure arithmetic on ticks
            // already in hand, so this adds no syscall here.
            //
            // Clamped, and it cannot be anything else. Two independent guards
            // because they answer different questions:
            //   lastTick == 0            the entry has never recorded an import,
            //                            so there is no base to measure from
            //                            and the honest answer is 0, not "now".
            //   threadTick <= lastTick   the base is not older than the reading.
            //                            After the per-thread re-read above this
            //                            should be unreachable, and that is the
            //                            point: it is the belt to the re-read's
            //                            braces. A wrap needs a tick source that
            //            moves backwards or a writer on a different base, and
            //            this returns 0 for both instead of ~1.8e19.
            // Clamping to 0 is the SAFE direction for both walk gates below:
            // they test `>=`, so an unset base fails the test and no walk is
            // spent on a thread we cannot date. Spending a walk on a wrapped
            // value is how a 200-line dump gets paid for a thread that
            // imported a millisecond ago.
            idleMs = censusMillis(
                (lastTick != 0 && threadTick > lastTick)
                    ? threadTick - lastTick : 0);
            // The frame-pointer walk is aimed at exactly one thread: the root,
            // i.e. the thread that armed the census and is also the one running
            // the frame loop, so the chain it produces is the chain
            // onViewRenderJNI is stuck inside. Every other guest thread is
            // either running (verdict=running, its stack is moving under the
            // read) or irrelevant, and walking all of them would cost hundreds
            // of lines to answer one question. kFpWalkIdleMs is the other half
            // of the gate: a root thread that is still advancing has no stable
            // stack to read.
            if (entry.fpwalkCount < kFpWalkRepeats && entry.root
                && idleMs >= kFpWalkIdleMs
                && (entry.fpwalkCount == 0
                    || censusMillis(nowTick - entry.fpwalkTick)
                        >= kFpWalkRepeatMs)) {
                ++entry.fpwalkCount;
                entry.fpwalkTick = nowTick;
                wantFpWalk = true;
            }
            // A thread that meets the spin rule gets the SAME x29 walk, so a
            // stack comes out and not just one address. Budgeted per thread and
            // per run, and it runs after the resume, so it cannot lengthen a
            // pause on the thread it is inspecting.
            if (spin && entry.pcSpinWalks < kSpinWalkBudget
                && gCensusSpinWalks < kSpinWalkGlobal) {
                ++entry.pcSpinWalks;
                ++gCensusSpinWalks;
                wantSpinWalk = true;
            }
            // The stall walk: the reachable replacement for the spin walk on a
            // platform that will not report a guest pc. Numeric predicate and
            // the reason the spin rule cannot be reused are at
            // kStallWalkStablePasses. Suppressed when wantFpWalk already fired
            // for this thread in this pass, so the root -- which has its own
            // three-sample walk plus censusPcBurst -- is never walked twice for
            // the same instant and paid for twice.
            //
            // TWO ADMISSIONS, and the second one is the point of this edit.
            //
            // The original predicate required `!sentinel && pcStable >= N`. Both
            // halves are consequences of the stall, not evidence against it: a
            // thread parked inside a host call has a pc in no image we can
            // classify, so it reads as the sentinel on every pass and pcStable
            // resets to 0 every pass. That gate therefore excluded precisely
            // the threads this walk was built for, and in
            // 20261009-204518-destructorgate it never fired once on the thread
            // that matters (pc=0x4 pc_img=sentinel pc_stable=0
            // lr_elfva=0xb3bc44, stable for 3783 samples).
            //
            // The lr admission is SAFE BY CONSTRUCTION and needs no second
            // hypothesis to be safe: censusIsGuestImage(lrRc) re-checks, from
            // the same context, that the lr really lands in a loaded guest
            // image. If the context read were returning the READER's registers
            // -- which is what guest_view_probe.cpp:716-720 measured for the pc
            // -- then the lr would be a host-nro address, the classification
            // would fail, and this gate would stay shut exactly as before. The
            // change can only open for a thread whose lr is genuinely guest,
            // which is the condition the walk needs and cannot fake.
            const bool pcStalled = ctxOk && !sentinel
                && importsDelta == 0
                && pcStable >= kStallWalkStablePasses
                && idleMs >= kStallWalkIdleMs;
            const bool lrStalled = ctxOk && sentinel
                && censusIsGuestImage(lrRc)
                && importsDelta == 0
                && lrStable >= kStallWalkStablePasses
                && idleMs >= kStallWalkIdleMs;
            const bool stalled = pcStalled || lrStalled;
            if (stalled && !wantFpWalk
                && entry.pcStallWalks < kStallWalkBudget
                && gCensusStallWalks < kStallWalkGlobal) {
                ++entry.pcStallWalks;
                ++gCensusStallWalks;
                wantStallWalk = true;
            }
            break;
        }
        mutexUnlock(&gCensusLock);

        if (exited || samples == 0) {
            continue;
        }
        // After the resume above, deliberately: the walk is pure memory
        // reading and must not extend the pause on the main guest thread.
        if (wantFpWalk && !R_FAILED(got)) {
            censusFpWalk(targets[i].ordinal, context, startRoutine, "root");
            // Same gate, same thread, same instant: the burst is the
            // wedge-time view of the pc that census-pc can only average over a
            // whole thread lifetime. Root only -- other threads' pcs are already
            // readable and are reported by the sweep above.
            censusPcBurst(targets[i].ordinal, targets[i].handle);
        }
        if (wantSpinWalk && !R_FAILED(got)) {
            // Reused verbatim, not reimplemented: this is the walk that already
            // emits fpwalk-raw (sp/fp/lr plus [sp]/[sp+8]/[fp]...), the full
            // x19..x28 dump, and the img=/off= classified frame chain.
            censusFpWalk(targets[i].ordinal, context, startRoutine, "spin");
        }
        if (wantStallWalk && !R_FAILED(got)) {
            // The same walk under a third role label, so a log reader can tell
            // a pc-free stall sample from a spin-flagged one without having to
            // reconstruct which gate fired. role= is a label only: it changes
            // no argument and no read.
            censusFpWalk(targets[i].ordinal, context, startRoutine, "stall");
        }
        if (!publish) {
            continue;   // steady state: the verdict has not changed
        }
        const char* verdict = sentinelCount == samples ? "sentinel"
            : (sentinelCount == 0 ? "running" : "mixed");
        char hot[kCensusPcSlots * 26]{};
        size_t hotLength = 0;
        for (size_t s = 0; s < kCensusPcSlots && top[s] != 0; ++s) {
            // Tag the address rather than labelling everything "elfva": on Eden
            // a foreign thread's reported pc lands in the NRO's own text, well
            // below the guest load bias, so subtracting the bias would invent a
            // nonsense offset. The tag says which it actually is.
            const bool guest = bias != 0 && top[s] >= bias;
            const int written = __builtin_snprintf(
                hot + hotLength, sizeof(hot) - hotLength, "%s%s0x%llx:%u",
                s == 0 ? "" : " ",
                guest ? "elfva:" : "host:",
                (unsigned long long)(guest ? top[s] - bias : top[s]),
                topHits[s]);
            if (written <= 0) {
                break;
            }
            hotLength += static_cast<size_t>(written);
        }
        // The CURRENT pc and lr of THIS thread, classified, next to the
        // lifetime histogram above. The two answer different questions and the
        // histogram could not answer this one: top= is a deduped lifetime
        // summary, while pc=/lr= is what the thread is sitting on right now.
        //
        // pc_elfva is the guest ELF virtual address -- pc minus the base of the
        // image pc actually landed in -- and is the whole point, because it is
        // what a disassembler takes. It reads n/a whenever pc is not in a guest
        // image: printing a number there would be a plausible lie, which is how
        // the old bias-only test survived so long.
        // Rendered as strings so "not in a guest image" is n/a rather than a
        // zero that reads like a real offset.
        char pcElfva[24];
        char lrElfva[24];
        __builtin_snprintf(pcElfva, sizeof(pcElfva), "0x%llx",
                           (unsigned long long)pcRc.off);
        __builtin_snprintf(lrElfva, sizeof(lrElfva), "0x%llx",
                           (unsigned long long)lrRc.off);
        // 512, sized against censusLog's own 640-byte line buffer: the absolute
        // worst case (every counter at UINT32_MAX, both images named, all four
        // top= slots full) is 498 bytes of details, and the phase/ordinal/target
        // prefix adds 46. Anything smaller truncates the top= tail off the end,
        // which is the field the previous format had room to spare.
        // Host-NRO hits get their offset too. This is the highest-value field
        // in the line and it was being withheld: `pc_elfva=n/a` for a host pc
        // is a decision, not a missing measurement -- censusClassifyRegister is
        // handed `nroBase` right above and carries `off` for exactly this case.
        //
        // The cost of withholding it: every host pc in every run had to be
        // guessed at, and the run's only two distinct GUEST addresses
        // (0xcd2abc and 0xcd2ad0, sampled 5x and 3x, and they are the head and
        // body of the loop the guest never leaves) stayed unidentified for a
        // whole session. Prefixed `host+` so it can never be mistaken for a
        // guest elfva.
        char pcElfvaBuf[40];
        if (pcGuest) {
            __builtin_snprintf(pcElfvaBuf, sizeof(pcElfvaBuf), "%s", pcElfva);
        } else if (pcRc.off != 0) {
            __builtin_snprintf(pcElfvaBuf, sizeof(pcElfvaBuf), "host+%#llx",
                               (unsigned long long)pcRc.off);
        } else {
            __builtin_snprintf(pcElfvaBuf, sizeof(pcElfvaBuf), "n/a");
        }
        char lrElfvaBuf[40];
        if (censusIsGuestImage(lrRc)) {
            __builtin_snprintf(lrElfvaBuf, sizeof(lrElfvaBuf), "%s", lrElfva);
        } else if (lrRc.off != 0) {
            __builtin_snprintf(lrElfvaBuf, sizeof(lrElfvaBuf), "host+%#llx",
                               (unsigned long long)lrRc.off);
        } else {
            __builtin_snprintf(lrElfvaBuf, sizeof(lrElfvaBuf), "n/a");
        }
        char details[512];
        __builtin_snprintf(details, sizeof(details),
                           "start=0x%llx verdict=%s samples=%u sentinel=%u "
                           "pause_fail=%u ctx_fail=%u pc=0x%llx pc_ctx=%s "
                           "pc_img=%s pc_elfva=%s lr=0x%llx lr_ctx=%s "
                           "lr_img=%s lr_elfva=%s pc_stable=%u "
                           "imports_delta=%u idle_ms=%llu spinning=%s "
                           "rule=stable%u top=%s",
                           (unsigned long long)startRoutine, verdict, samples,
                           sentinelCount, pauseFail, ctxFail,
                           (unsigned long long)pc, ctxOk ? "ok" : "failed",
                           pcRc.kind != nullptr ? pcRc.kind : "unknown",
                           pcElfvaBuf,
                           (unsigned long long)lr, ctxOk ? "ok" : "failed",
                           lrRc.kind != nullptr ? lrRc.kind : "unknown",
                           lrElfvaBuf,                           pcStable, importsDelta,
                           (unsigned long long)idleMs,
                           pcGuest ? (spin ? "yes" : "no") : "unset",
                           kSpinStablePasses, hot);
        censusLog("census-pc", targets[i].ordinal, samples, details);
    }
}

void* censusReporterWorker(void*);

void censusStartReporter() {
    if (gCensusReporterStarted) {
        return;
    }
    gCensusReporterStarted = true;
    pthread_t reporter{};
    // Joinable on purpose and never joined: it is a diagnostic backstop for
    // the all-threads-frozen case, and handoff must never block on it.
    if (::pthread_create(&reporter, nullptr, censusReporterWorker, nullptr) != 0) {
        gCensusReporterStarted = false;
    }
}

// The reporter's 500 ms tick waited on svcSleepThread, which an Eden-created
// thread does not return from: in 20261004-030501-pump-before this reporter
// produced exactly three census-pc lines, all samples=1 (its first and only
// pass) and zero census-sweep lines, in a run where the sweep is attempted
// thousands of times a second. Swapped to the pair this file already uses in
// tree -- condvarWaitTimeout, as reapDetachedThreadWorker does at line 3989 --
// so the mechanism is the reaper's, not a new one.
//
// libnx Mutex + CondVar, NOT std::mutex. This thread is host-only: it is a
// diagnostic reporter, not a guest start routine, and the wait it adds is not
// on any guest call path. Nothing on the guest path changes -- in particular
// censusNoteImportFrom, the ~265k calls/second poll path, is untouched by this
// and must stay byte-for-byte identical.
Mutex gCensusReporterWaitLock{};
CondVar gCensusReporterWait{};

// One-shot, second iteration only: the same bounded reporting the reaper does
// for its own wait. It reports the measured gap between the first two ticks --
// at or near 500 ms the wait is working, and anything far from it (0, or many
// seconds) says the wait returned early or the thread stopped being resumed.
// This thread is created by ::pthread_create rather than threadCreate, so its
// priority and entry path differ from the two raw reporters; this line is what
// settles whether that difference matters.
// A plain bool, not std::atomic, and deliberately: only this thread ever reads
// or writes it, and a plain bool is what every other one-shot flag in this file
// is (gCensusReporterStarted, gReaperStarted, gQuiesceRequested).
bool gCensusReporterWakeReported = false;

void* censusReporterWorker(void*) {
    const uint64_t firstTick = censusTick();
    unsigned long long ticks = 0;
    for (;;) {
        ++ticks;
        mutexLock(&gCensusReporterWaitLock);
        // 0xEA01 on timeout is expected and ignored: this is a pure tick, not a
        // condition wait, so there is no wakeup to lose.
        (void)condvarWaitTimeout(&gCensusReporterWait,
                                 &gCensusReporterWaitLock, 500000000LL);
        mutexUnlock(&gCensusReporterWaitLock);
        if (ticks == 2 && !gCensusReporterWakeReported) {
            gCensusReporterWakeReported = true;
            char details[128];
            __builtin_snprintf(details, sizeof(details),
                               "iters=%llu interval_ms=%llu target_ms=500"
                               " prim=condvarWaitTimeout",
                               ticks,
                               (unsigned long long)censusMillis(censusTick() -
                                                               firstTick));
            guestViewWideLog("census-reporter-wake", ticks, 500000000ULL,
                             details);
        }
        mutexLock(&gRegistryLock);
        const bool stop = gQuiesceRequested;
        mutexUnlock(&gRegistryLock);
        if (stop) {
            return nullptr;
        }
        if (gCensusArmed) {
            // The PC pass runs FIRST, before the sweep, and not because of
            // taste: the sweep is the part that can still be slow, and the
            // pass is the only thing that feeds pcStable, which the frame-walk
            // trigger needs at 3. A pass that runs once can never satisfy
            // pcStable >= 3, so a sweep that starves the pass does not just cost
            // log volume -- it makes the walk structurally unreachable. The
            // sweep is on a 4 s wall-clock gate of its own (censusMaybeSweep)
            // and is not skipped by it: that gate returns out of
            // censusMaybeSweep only, and the pass was always called
            // unconditionally on the next line. Ordering the cheap-to-lose
            // output first is the rest of the insurance.
            censusSampleThreadPcs();
            // Respect the sweep interval: the reporter is a backstop for the
            // all-threads-frozen case, not a second sweep source. Cannot block.
            censusMaybeSweep();
        }
    }
}

// ===========================================================================
// ROOT-thread PC sampler. Supplementary to censusSampleThreadPcs, not part of
// it: the pass above is left exactly as it was.
//
// WHY IT EXISTS. The one number still missing is expect_0x5e0 -- is x29 this
// function's frame pointer -- and the pass cannot get it at its current
// cadence. The cost is one pause/context/resume round trip PER THREAD per
// pass, and that pass takes ~12.7 s of wall clock: 20261006-023240-fpsp-long
// carries 33 root-thread samples (the per-thread `samples=` counter, printed as
// target=) across its 420 s, with 4 census sweeps, i.e. ~0.079 Hz for the one
// thread this question is about. Raising the rate is the fix, and the only way
// to raise the rate for ONE thread without paying per-thread for all of them
// is a second loop that samples only the root.
//
// WHAT THE ARCHIVES SAY IS ACTUALLY BLOCKING. Not the publish gate, and not
// the function being a small slice of execution. Two corrections, both
// measured, because the obvious reading of the log is wrong on both counts:
//
//   1. kCensusPcPublishMs does NOT bound the B/E line. The census-deinfo-bounds
//      block sits BEFORE the publish gate (line ~2687 vs ~2843), so it is
//      emitted on every read whose pc is a guest image, published or not. In
//      20261006-022123-devinfo-install 39 such lines came from 10 published
//      ordinal-0 lines -- ~4 per published line. The 5-vs-5 published count
//      across a 200 s and a 420 s run is a property of the publish gate, not
//      of the B/E sampling, and it is not what starves expect_0x5e0.
//
//   2. The real gate is whether the platform reports a GUEST pc for this
//      thread at all, and on Eden it alternates. Ordinal 0 in
//      20261006-022123-devinfo-install is host-nro for targets 1..59 (~40 s)
//      and libRealRacing3.so from target 60 on. The census's B/E block is
//      behind `if (pcGuest)`, so during those 40 s -- the period in which the
//      root thread is PROVABLY outside [0xcd1014, 0xcd3648) because its pc is
//      a host NRO address -- the census emitted nothing at all. The next
//      newest run, 20261006-023240-fpsp-long, is the same story end to end:
//      all 33 samples were host-nro, so it has zero census-deinfo-bounds lines.
//
//      That is why this sampler adds FormatNotGuest. A sampler that reuses the
//      `if (pcGuest)` gate would be silent through exactly the window a
//      negative control needs, and "silent because out of range" would be
//      indistinguishable from "silent because the gate never opened".
//
// SAFETY. No std::mutex anywhere near this path and no lock held across a
// pause: the port is built -fno-exceptions and a std::mutex on a guest-visible
// path once cost 187 s of CPU. The only locks here are the libnx Mutex this
// file already uses -- gCensusLock, taken to COPY the root handle and released
// before any pause, and gRegistryLock for the stop flag. The wait is libnx
// Mutex+CondVar and condvarWaitTimeout, the same mechanism as
// censusReporterWorker, because svcSleepThread does not return on an
// Eden-created thread.
// ===========================================================================

// The sampler's own wait. Separate from the reporter's CondVar so this thread
// can never be woken by -- or delay -- a census pass.
Mutex gRootPcSamplerWaitLock{};
CondVar gRootPcSamplerWait{};

// Plain bool, and only ever touched by rr3RootPcSamplerStart (before the thread
// exists) and by the sampler thread. Same convention as gCensusReporterStarted
// and gCensusReporterWakeReported.
bool gRootPcSamplerStarted{};

// Bounds the wait for a root census entry. censusArm() binds the caller as root
// and then starts the reporter, so a sampler started after the census is armed
// normally finds the entry on its first look; the loop only covers being
// started a beat earlier. 200 x 100 ms = 20 s, then it gives up loudly.
constexpr unsigned kRootPcHandleTries = 200;
constexpr uint64_t kRootPcHandleRetryMs = 100;

// One rate/heartbeat line per this many ticks, unconditionally, so the achieved
// rate is on the record even while every sample is a miss and the miss budget
// is being spent. This line is the evidence and is NOT budgeted.
constexpr uint64_t kRootPcRateEveryTicks = 100;

// ---------------------------------------------------------------------------
// The resume watchdog.
//
// THE DEFECT IT EXISTS FOR. The sampler pauses the guest root thread, calls
// svcGetThreadContext3, and resumes it. The comment above the resume has always
// said "always resume before anything else: leaving the guest thread paused
// turns frozen into wedged, which is the very failure this investigation is
// about". The intent is right and the enforcement is not: if
// svcGetThreadContext3 does not return, the resume on the following line is
// never REACHED. The safety property that comment asserts is enforced solely
// by the sampler thread continuing to execute, and 20261006-030541-sentinel-back
// is a run whose evidence is that it did not.
//
// So the guest main thread's frozen time is currently bounded by whatever a
// possibly-stuck thread happens to do next, which is to say it is not bounded
// at all. The watchdog moves the resume off the stuck thread and onto one that
// is not stuck. After this the bound is structural.
//
// It also settles the OTHER half of the same question, which is cheaper and
// more important: because the watchdog keeps beating whether or not the sampler
// does, "the sampler's last tick stopped advancing" becomes a number in the log
// rather than an inference from an absence. See the heartbeat below.
//
// SAFETY. No std::mutex anywhere on this path -- the port is -fno-exceptions and
// a std::mutex on a guest-visible path once cost 187 s of CPU here. The only
// lock is the libnx Mutex this file already uses, gRootPcResumeLock, and it is
// never held across svcSetThreadActivity: a syscall under a lock on a
// guest-facing path is how this file keeps producing new ways to wedge.
// ---------------------------------------------------------------------------

// The sampler's own wait, reused so the watchdog wakes on the same mechanism and
// never depends on svcSleepThread, which does not return on an Eden-created
// thread (same reason the sampler below uses a CondVar).
constexpr uint64_t kRootPcWatchdogPeriodMs = 50;
// The ceiling on how long the guest root thread may stay paused by the sampler.
// The sampler's own resume normally runs in microseconds; this is the ceiling
// for the case where it does not run at all. 150 ms is 3x the 100 ms sample
// period, so a healthy tick can never trip it -- it is a bound on a pathology,
// not a tuning knob on the sample rate.
constexpr uint64_t kRootPcResumeMaxPausedMs = 150;
// The watchdog's heartbeat, in watchdog checks: 100 x 50 ms = 5 s. This is the
// ONLY thing the watchdog logs routinely (~37 lines over a 187 s run) and it is
// here deliberately: without an independent liveness witness, "the sampler
// stopped" and "the log stopped" are the same observation. The watchdog is the
// one thread whose beating does not depend on the thing being measured.
constexpr uint64_t kRootPcWatchdogHeartbeatChecks = 100;

// Published BEFORE the pause, so a pause that does not return is still bounded.
// Cleared only after a resume returns success. The generation makes a watchdog
// that read a stale record identifiable: it resumes nothing rather than
// cancelling the pause of a LATER tick it knows nothing about.
Mutex gRootPcResumeLock{};
uint32_t gRootPcResumePending{};
uint32_t gRootPcResumeGeneration{};
uint64_t gRootPcResumeTick{};
uint64_t gRootPcResumeRecoveries{};

// The sampler's progress, published by the sampler and read by the watchdog.
// Plain aligned 64-bit words, the same convention as gRootPcSamplerStarted and
// every other one-shot flag in this file. A torn read is harmless: these are a
// coverage hint, and the worst case is one nonsense age on one heartbeat. No
// atomic and no lock, because the watchdog must never be the reason it cannot
// read them.
uint64_t gRootPcSamplerLastTick{};
uint64_t gRootPcSamplerLastWallTick{};

// Step markers. One line before each of the four calls on this path that can
// each fail to return -- the pause, the getctx, the resume, and the log -- so
// the LAST marker in the log names the call that did not come back.
//
// Why that matters, and it is not cosmetic: wedged in getctx means the guest
// root thread is PAUSED right now and we caused it; wedged in the log means the
// thread is RUNNING and only the probe was lost. All four produce the same log
// otherwise -- no next line -- which is precisely why 20261006-030541 could not
// be adjudicated.
//
// Bounded two ways: at most one line per (step, tick) via the mask the loop
// passes in, and a run-wide budget whose usage is printed on EVERY rate
// heartbeat as step_lines=N/BUDGET, so the limit can never be a silent one. At
// the measured ~9.6 Hz the budget covers ~2000 ticks (~208 s), past the length
// of the 187 s run that produced the ambiguity.
enum : uint8_t {
    kSamplerStepPause  = 1u << 0,
    kSamplerStepGetctx = 1u << 1,
    kSamplerStepResume = 1u << 2,
    kSamplerStepLog    = 1u << 3,
};
constexpr uint32_t kRootPcStepLineBudget = 8000;

const char* censusSamplerStepName(uint8_t step) {
    switch (step) {
    case kSamplerStepPause:  return "pause";
    case kSamplerStepGetctx: return "getctx";
    case kSamplerStepResume: return "resume";
    default:                 return "log";
    }
}

// No lock is held here and none is taken: censusLog takes gCensusLogLock itself,
// and the point of a step marker is to be cheap enough to place immediately
// before the call it is describing.
void censusSamplerStep(unsigned long long ordinal, uint64_t tick, uint8_t step,
                       uint8_t& mask, uint32_t& emitted) {
    if ((mask & step) != 0) {
        return;   // already reported for this tick
    }
    mask = static_cast<uint8_t>(mask | step);
    if (emitted >= kRootPcStepLineBudget) {
        return;
    }
    ++emitted;
    char details[192];
    __builtin_snprintf(details, sizeof(details),
                       "step=%s tick=%llu step_lines=%u budget=%u",
                       censusSamplerStepName(step),
                       (unsigned long long)tick, emitted, kRootPcStepLineBudget);
    censusLog("root-pc-sampler-step", ordinal, 0, details);
}

// Runs for the life of the process, not for the life of the sampler. That is the
// entire point: a watchdog that stops when the thing it watches stops is not a
// watchdog.
void* rootPcResumeWatchdogWorker(void*) {
    uint64_t checks = 0;
    uint32_t recoveriesReported = 0;
    for (;;) {
        ++checks;
        uint32_t handle = 0;
        uint32_t generation = 0;
        uint64_t pendingSince = 0;
        mutexLock(&gRootPcResumeLock);
        if (gRootPcResumePending != 0) {
            handle = gRootPcResumePending;
            generation = gRootPcResumeGeneration;
            pendingSince = gRootPcResumeTick;
        }
        mutexUnlock(&gRootPcResumeLock);

        if (handle != 0
            && censusMillis(censusTick() - pendingSince) >= kRootPcResumeMaxPausedMs) {
            // Re-read immediately before the syscall. The window between this
            // unlock and the call is the residual race named above and is not
            // removable without holding a lock across a syscall on a guest-facing
            // path; shrinking it is what is available.
            mutexLock(&gRootPcResumeLock);
            const bool stillPending = gRootPcResumePending == handle
                && gRootPcResumeGeneration == generation;
            mutexUnlock(&gRootPcResumeLock);
            if (stillPending) {
                const uint64_t heldMs = censusMillis(censusTick() - pendingSince);
                const Result resumed =
                    svcSetThreadActivity(static_cast<Handle>(handle),
                                         ThreadActivity_Runnable);
                ++gRootPcResumeRecoveries;
                mutexLock(&gRootPcResumeLock);
                if (gRootPcResumePending == handle
                    && gRootPcResumeGeneration == generation) {
                    gRootPcResumePending = 0;
                }
                mutexUnlock(&gRootPcResumeLock);
                // Reported, but bounded to two lines for the whole run: this is
                // an abnormal event, and a 50 ms cadence on it would be 3740
                // lines over the run that needs it.
                if (recoveriesReported < 2) {
                    ++recoveriesReported;
                    char details[320];
                    __builtin_snprintf(details, sizeof(details),
                        "RECOVERED handle=0x%x generation=%u held_ms=%llu "
                        "resume_rc=%d recoveries=%llu sampler_last_tick=%llu "
                        "note=watchdog_resumed_the_root_thread_the_sampler_"
                        "never_released",
                        handle, generation, (unsigned long long)heldMs,
                        static_cast<int>(resumed),
                        (unsigned long long)gRootPcResumeRecoveries,
                        (unsigned long long)gRootPcSamplerLastTick);
                    censusLog("root-pc-resume-watchdog", 0, handle, details);
                }
            }
        }

        if (checks % kRootPcWatchdogHeartbeatChecks == 0) {
            // The coverage witness. `sampler_last_tick` and
            // `sampler_heartbeat_age_ms` are read here and nowhere else, and
            // they are what turn "no sampler lines after tick 425" from a guess
            // into a measurement: if this keeps beating while the sampler's own
            // heartbeat stops, the sampler thread stopped and the log did not.
            char details[320];
            __builtin_snprintf(details, sizeof(details),
                "checks=%llu checks_ms=%llu period_ms=%llu max_paused_ms=%llu "
                "recoveries=%llu sampler_last_tick=%llu "
                "sampler_heartbeat_age_ms=%llu "
                "note=this_line_beating_while_sampler_lines_stop_"
                "means_the_sampler_thread_stopped_not_the_log",
                (unsigned long long)checks,
                (unsigned long long)(checks * kRootPcWatchdogPeriodMs),
                (unsigned long long)kRootPcWatchdogPeriodMs,
                (unsigned long long)kRootPcResumeMaxPausedMs,
                (unsigned long long)gRootPcResumeRecoveries,
                (unsigned long long)gRootPcSamplerLastTick,
                (unsigned long long)censusMillis(
                    gRootPcSamplerLastWallTick != 0
                        ? censusTick() - gRootPcSamplerLastWallTick : 0));
            censusLog("root-pc-resume-watchdog", 0, 0, details);
        }

        // Exits only with nothing outstanding. If the sampler is wedged with
        // the guest still paused, exiting here would undo the entire reason this
        // thread exists, so quiesce is honoured only once the record is clear.
        mutexLock(&gRegistryLock);
        const bool stop = gQuiesceRequested;
        mutexUnlock(&gRegistryLock);
        mutexLock(&gRootPcResumeLock);
        const bool outstanding = gRootPcResumePending != 0;
        mutexUnlock(&gRootPcResumeLock);
        if (stop && !outstanding) {
            char details[256];
            __builtin_snprintf(details, sizeof(details),
                               "STOPPED checks=%llu recoveries=%llu "
                               "sampler_last_tick=%llu",
                               (unsigned long long)checks,
                               (unsigned long long)gRootPcResumeRecoveries,
                               (unsigned long long)gRootPcSamplerLastTick);
            censusLog("root-pc-resume-watchdog", 0, 0, details);
            return nullptr;
        }

        mutexLock(&gRootPcSamplerWaitLock);
        // 0xEA01 on timeout is expected and ignored: this is a pure tick.
        (void)condvarWaitTimeout(&gRootPcSamplerWait,
                                 &gRootPcSamplerWaitLock,
                                 kRootPcWatchdogPeriodMs * 1000000ULL);
        mutexUnlock(&gRootPcSamplerWaitLock);
    }
}

// The wait is bracketed by a batch scope so a tick's lines become one SD write
// instead of a write each, and so census lines from the reporter -- which
// censusLog appends whenever the batch depth is non-zero -- share the same
// write. Held across the wait only, never across a pause, and
// kCensusBatchMaxOpenMs still bounds how long the batch may stay open.

void* rootPcSamplerWorker(void*) {
    const uint64_t tickFreq = censusTickFreq();
    const uint64_t periodTicks = tickFreq / rr3::rootpc::kRequestedHz;
    if (periodTicks == 0) {
        censusLog("root-pc-sampler-start", 0, 0,
                  "FAILED reason=tick-freq-unavailable");
        return nullptr;
    }

    // The root thread's handle is the census's. There is deliberately no second
    // way to find the root thread: censusBindCaller already marks entry->root
    // and already stored threadGetCurHandle() for it, and a second discovery
    // path is a second thing that can be wrong about which thread is the guest
    // main thread.
    uint32_t ordinal = 0;
    uint32_t rootHandle = 0;
    for (unsigned tries = 0; tries < kRootPcHandleTries && rootHandle == 0;
         ++tries) {
        mutexLock(&gCensusLock);
        for (const CensusEntry& entry : gCensus) {
            if (entry.used && entry.root && !entry.exited
                && entry.kernelHandle != 0) {
                ordinal = entry.ordinal;
                rootHandle = entry.kernelHandle;
                break;
            }
        }
        mutexUnlock(&gCensusLock);
        if (rootHandle == 0) {
            mutexLock(&gRootPcSamplerWaitLock);
            // 0xEA01 on timeout is expected and ignored: this is a pure tick.
            (void)condvarWaitTimeout(&gRootPcSamplerWait,
                                     &gRootPcSamplerWaitLock,
                                     kRootPcHandleRetryMs * 1000000ULL);
            mutexUnlock(&gRootPcSamplerWaitLock);
        }
    }
    if (rootHandle == 0) {
        // Almost always RR3_CENSUS=0, in which case censusArm() returns before
        // binding anyone and there is no root entry to find. Say that, rather
        // than starting a loop that can never sample anything.
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "FAILED reason=no-root-census-entry tries=%u "
                           "census_disabled=%d census_armed=%d",
                           kRootPcHandleTries, censusDisabled() ? 1 : 0,
                           gCensusArmed ? 1 : 0);
        censusLog("root-pc-sampler-start", 0, 0, details);
        return nullptr;
    }

    // ONE capture, NOT censusCachedPlan(). That cache is documented as touched
    // only by the reporter thread and carries no lock, so calling it from a
    // second thread would be an unsynchronised race on a struct it assigns in
    // place; it also re-captures, with an fsync, every kPlanRefreshPasses
    // passes. The image set does not change under a wedge, and if it ever did
    // the symptom is visible rather than silent: a stale plan degrades a guest
    // pc to host-nro and the sample is reported as NOT-GUEST-IMAGE.
    // Liveness FIRST, capture second.
    //
    // captureGuestStartupPlan() appends one guest-view log line and fsyncs it
    // (see the note at kPlanRefreshPasses), so it can block. The original order
    // logged STARTED *after* it, which means "the thread never started" and "the
    // capture never returned" produced the identical log: nothing at all. That
    // ambiguity cost a whole run on 2026-10-06 -- the search had in fact already
    // succeeded (no FAILED line appeared) and the reading of the empty log was
    // wrong twice before this was found.
    //
    // A "I started" proof must not depend on a call that may not return.
    {
        char details[256];
        __builtin_snprintf(details, sizeof(details),
                           "STARTED ordinal=%u handle=0x%x tick_freq=%llu "
                           "requested_hz=%llu period_ms=%llu (plan NOT yet "
                           "captured; the next line is the capture returning)",
                           ordinal, rootHandle, (unsigned long long)tickFreq,
                           (unsigned long long)rr3::rootpc::kRequestedHz,
                           (unsigned long long)censusMillis(periodTicks));
        censusLog("root-pc-sampler-start", ordinal, 0, details);
    }

    // ONE capture, for the whole run. The loop below uses this copy and never
    // re-captures; `censusCachedPlan`'s kPlanRefreshPasses re-capture is a
    // different code path and does not apply here.
    const GuestStartupPlan plan = captureGuestStartupPlan();
    {
        // `imageCount`, NOT `images`. `images` is the ARRAY; passing it to a
        // %u decays it to a pointer and prints the low 32 bits of this local's
        // stack address -- which is what produced the nonsense
        // "PLAN-CAPTURED images=2969242736" and, from it, a whole theory that
        // the plan was garbage and therefore every NOT-GUEST-IMAGE was a false
        // negative. There is no `images` count. The plan was fine; the field name
        // was mine and it was wrong. Fourth time this session that a conclusion
        // was built on a log field whose meaning had not been checked.
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "PLAN-CAPTURED available=%d imageCount=%llu "
                           "hash=0x%llx",
                           plan.available ? 1 : 0,
                           (unsigned long long)plan.imageCount,
                           (unsigned long long)plan.resolutionPlanHash);
        censusLog("root-pc-sampler-plan", ordinal, 0, details);
    }
    const uintptr_t nroBase = censusNroBase();
    const Handle handle = static_cast<Handle>(rootHandle);

    const uint64_t startTick = censusTick();
    uint64_t nextDue = startTick;
    uint64_t tick = 0;
    uint32_t missLines = 0;
    uint32_t hitLines = 0;
    uint32_t pauseFails = 0;
    uint32_t stepLines = 0;
    bool missCapped = false;
    bool hitCapped = false;

    // The STARTED line is emitted ABOVE, before captureGuestStartupPlan(), so
    // that a capture that does not return cannot also erase the proof that the
    // thread exists. The duplicate that used to sit here is gone on purpose:
    // two lines with the same phase name, indistinguishable in a log, is the
    // same class of defect as the `n/a` fields this session spent hours on.

    for (;;) {
        {
            mutexLock(&gRegistryLock);
            const bool stop = gQuiesceRequested;
            mutexUnlock(&gRegistryLock);
            if (stop) {
                char details[256];
                __builtin_snprintf(details, sizeof(details),
                                   "STOPPED ticks=%llu hits=%u misses=%u "
                                   "pause_fails=%u step_lines=%u/%u",
                                   (unsigned long long)tick, hitLines, missLines,
                                   pauseFails, stepLines, kRootPcStepLineBudget);
                censusLog("root-pc-sampler-start", ordinal, 0, details);
                return nullptr;
            }
        }

        ++tick;
        const uint64_t nowTick = censusTick();
        const uint64_t elapsedMs = censusMillis(nowTick - startTick);
        const uint64_t hzX100 = rr3::rootpc::RateX100(tick, elapsedMs);

        // ---- the only guest-visible part: one round trip, no lock held ----
        uint64_t pc = 0;
        uint64_t sp = 0;
        uint64_t fp = 0;
        bool ctxOk = false;
        // Published BEFORE the pause. If it were published after, the one pause
        // that matters most -- the one whose own svc does not return -- would be
        // the one the watchdog knows nothing about. The cost of publishing
        // early is that a pause which FAILS leaves a pending record the watchdog
        // will clear by resuming a thread that was never paused; that is a
        // no-op on a running thread and it costs at most two RECOVERED lines,
        // which is the right trade against a main thread frozen forever.
        uint32_t myGeneration = 0;
        mutexLock(&gRootPcResumeLock);
        myGeneration = ++gRootPcResumeGeneration;
        gRootPcResumePending = rootHandle;
        gRootPcResumeTick = nowTick;
        mutexUnlock(&gRootPcResumeLock);
        // Liveness for the watchdog, likewise before the pause rather than
        // after: a tick published after its own pause would be a tick that had
        // already completed.
        gRootPcSamplerLastTick = tick;
        gRootPcSamplerLastWallTick = nowTick;
        uint8_t stepMask = 0;
        censusSamplerStep(ordinal, tick, kSamplerStepPause, stepMask, stepLines);

        const Result paused =
            svcSetThreadActivity(handle, ThreadActivity_Paused);
        if (R_FAILED(paused)) {
            ++pauseFails;
            mutexLock(&gRootPcResumeLock);
            if (gRootPcResumeGeneration == myGeneration) {
                gRootPcResumePending = 0;
            }
            mutexUnlock(&gRootPcResumeLock);
        } else {
            ThreadContext context;
            std::memset(&context, 0, sizeof(context));
            censusSamplerStep(ordinal, tick, kSamplerStepGetctx, stepMask,
                              stepLines);
            const Result got = svcGetThreadContext3(&context, handle);
            censusSamplerStep(ordinal, tick, kSamplerStepResume, stepMask,
                              stepLines);
            // Always resume before anything else: leaving the guest thread
            // paused turns "frozen" into "wedged", which is the very failure
            // this investigation is about.
            const Result resumed =
                svcSetThreadActivity(handle, ThreadActivity_Runnable);
            if (R_FAILED(resumed)) {
                // The pending record is deliberately NOT cleared. This thread
                // is giving up on the root thread, but "giving up" is not "the
                // root thread is running": if the resume failed it may still be
                // paused, and clearing the record would hand a possibly frozen
                // guest main thread to nobody. The watchdog keeps retrying on
                // its own clock and logs RECOVERED if it ever succeeds -- which
                // is strictly better than this thread spinning on pauses it
                // cannot undo, the one thing the comment below forbids.
                char details[192];
                __builtin_snprintf(details, sizeof(details),
                                   "FAILED reason=cannot-resume-root ticks=%llu "
                                   "handoff=watchdog pending=1",
                                   (unsigned long long)tick);
                censusLog("root-pc-sampler-start", ordinal, 0, details);
                return nullptr;
            }
            mutexLock(&gRootPcResumeLock);
            if (gRootPcResumeGeneration == myGeneration) {
                gRootPcResumePending = 0;
            }
            mutexUnlock(&gRootPcResumeLock);
            ctxOk = !R_FAILED(got);
            pc = static_cast<uint64_t>(context.pc.x);
            sp = static_cast<uint64_t>(context.sp);
            fp = static_cast<uint64_t>(context.fp);
        }

        // ---- everything below runs with the root thread RUNNING. ---------
        char body[288];
        bool isHit = false;
        if (!ctxOk) {
            __builtin_snprintf(body, sizeof(body),
                               "NO-CONTEXT (B/E not read) pause_fails=%u",
                               pauseFails);
        } else {
            // The SAME attribution the census uses, and the same range test
            // (rr3::rootpc::kFdeLo/kFdeHi are copies of the census's
            // kDevInfoFdeLo/kDevInfoFdeHi), so the two lines mean one thing.
            const RegisterClass pcRc = censusClassifyRegister(
                plan, nroBase, static_cast<uintptr_t>(sp),
                static_cast<uintptr_t>(pc), true);
            const bool pcGuest = pc >= kCensusPcSentinel
                && censusIsGuestImage(pcRc);
            if (!pcGuest) {
                rr3::rootpc::FormatNotGuest(body, sizeof(body), pcRc.kind, pc,
                                            pcRc.off);
            } else if (!rr3::rootpc::InFdeRange(pcRc.off)) {
                rr3::rootpc::FormatOutOfRange(body, sizeof(body), pcRc.off);
            } else {
                // In range counts as a hit even when the frame guard then trips
                // the B/E read off: the pc WAS inside the function, which is the
                // rare condition, and it must not be charged to the miss budget
                // that exists to prove the sampler was running.
                isHit = true;
                if (!rr3::rootpc::FramePlausible(fp)) {
                    rr3::rootpc::FormatGuardTripped(body, sizeof(body), fp,
                                                    pcRc.off);
                } else {
                    // Read AFTER the resume, from the same offsets the census
                    // uses. The frame is stable for the duration of the call,
                    // which is the premise the whole report rests on -- and
                    // expect_0x5e0 is what tests that premise.
                    const uint64_t bWord =
                        *reinterpret_cast<const uint64_t*>(fp - 0x48);
                    const uint64_t eWord =
                        *reinterpret_cast<const uint64_t*>(fp - 0x40);
                    rr3::rootpc::FormatInRange(body, sizeof(body), fp, sp,
                                               pcRc.off, bWord, eWord);
                }
            }
        }

        // Two budgets, not one, so the misses can never spend the allowance the
        // hits need. The cap is announced, never silent.
        uint32_t& counter = isHit ? hitLines : missLines;
        bool& capped = isHit ? hitCapped : missCapped;
        const uint32_t budget =
            isHit ? rr3::rootpc::kHitLineBudget : rr3::rootpc::kMissLineBudget;
        // The fourth and last blocking point on this path: censusLog itself,
        // which takes gCensusLogLock and can fsync an emulated SD card. Marked
        // HERE rather than inside the budget branch so it also covers the ticks
        // where both budgets are already spent, which is exactly the case where
        // "nothing was logged" would otherwise be unanswerable.
        censusSamplerStep(ordinal, tick, kSamplerStepLog, stepMask, stepLines);
        if (counter < budget) {
            ++counter;
            char line[384];
            int written =
                rr3::rootpc::FormatPrefix(line, sizeof(line), tick, hzX100);
            if (written < 0) {
                written = 0;
            }
            (void)__builtin_snprintf(line + written, sizeof(line) - written,
                                     " %s", body);
            censusLog("root-pc-sampler", ordinal, 0, line);
        } else if (!capped) {
            capped = true;
            char line[256];
            rr3::rootpc::FormatCapped(line, sizeof(line),
                                      isHit ? "hit" : "miss", budget, tick);
            censusLog("root-pc-sampler", ordinal, 0, line);
        }

        // Unbudgeted heartbeat: the achieved rate has to be on the record even
        // while every sample is a miss. This is the line that settles whether
        // the pause round trip or the log is the limit.
        //
        // It is NOT inside either budget branch and never was, so an all-miss
        // run cannot starve it -- the 20261006-030541 reading that it was
        // starved is wrong about the mechanism: tick 500 is absent because the
        // sampler thread stopped, not because the budget gated this line. The
        // watchdog heartbeat is what proves that. step_lines and
        // sample_lines_capped are here so that COVERAGE is arithmetic in the log
        // rather than an inference from a bounded tail: "how many ticks did this
        // run cover, and how many lines was it allowed" has to be answerable
        // from the last line of the run.
        if (tick == 1 || tick % kRootPcRateEveryTicks == 0) {
            char details[320];
            __builtin_snprintf(details, sizeof(details),
                               "ticks=%llu elapsed_ms=%llu hz_x100=%llu "
                               "requested_hz=%llu hits=%u/%u misses=%u/%u "
                               "pause_fails=%u step_lines=%u/%u "
                               "miss_capped=%d hit_capped=%d",
                               (unsigned long long)tick,
                               (unsigned long long)elapsedMs,
                               (unsigned long long)hzX100,
                               (unsigned long long)rr3::rootpc::kRequestedHz,
                               hitLines, rr3::rootpc::kHitLineBudget,
                               missLines, rr3::rootpc::kMissLineBudget,
                               pauseFails, stepLines, kRootPcStepLineBudget,
                               missCapped ? 1 : 0, hitCapped ? 1 : 0);
            censusLog("root-pc-sampler-rate", ordinal, 0, details);
        }

        // Deadline wait, not "sleep 100 ms then sample". A round trip slower
        // than the period is the expected case here (see kRequestedHz), and a
        // fixed sleep compounds into 1/(roundTrip+sleep) while a deadline
        // resyncs to the fastest rate the loop can actually sustain. Debt is
        // dropped rather than accumulated, so one very slow tick does not leave
        // the loop permanently behind.
        const uint64_t afterTick = censusTick();
        if (nextDue < afterTick) {
            nextDue = afterTick;
        }
        nextDue += periodTicks;
        const uint64_t remainNanos =
            ((nextDue - afterTick) * 1000000000ULL) / tickFreq;
        {
            CensusBatchScope batch;
            mutexLock(&gRootPcSamplerWaitLock);
            (void)condvarWaitTimeout(&gRootPcSamplerWait,
                                     &gRootPcSamplerWaitLock, remainNanos);
            mutexUnlock(&gRootPcSamplerWaitLock);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// The one thing the guest-frame probe needs from this file.
//
// The probe has to compare the guest's x30 against a range expressed in the
// guest ELF's OWN address space ([0xcd1014, 0xcd3648)), and x30 is a RUNTIME
// address. The version that compared them directly could never match, because
// a runtime address is always above 0xcd3648 -- so `inrange` was structurally
// 0 and the probe's whole observation route was dead while looking alive. See
// the header for the full account and the archived pairing that proves the
// arithmetic.
//
// This accessor exists so the fold is not a second, independently-invented
// opinion about where the guest images are loaded. It reuses the census's own
// startup plan -- the same GuestStartupPlan instance censusImageOffsetOf reads,
// maintained by the same reporter pass -- and the fold itself is
// rr3::guestFrameElfOffsetInPlan, inline in the header so its control can run
// on the host.
//
// WHY THE CACHE AND NOT captureGuestStartupPlan
// captureGuestStartupPlan appends an fsync'd guest-view log line on EVERY
// call. The probe runs inside the guest's own memcpy; giving it a per-call
// capture would put an SD-card fsync on the guest's hot path, which is the one
// thing this probe is not allowed to do (see the design constraints at the top
// of guest_frame_probe.cpp, and the note on kPlanRefreshPasses above: census's
// own walk path reads the cache for the same reason).
//
// The plan is captured at frames==0, so a boot-time caller sees an unavailable
// plan. That is fail-open by construction, not an error: the probe's
// translation self-test reports "plan unavailable" instead of claiming a pass.
// ---------------------------------------------------------------------------
const GuestStartupPlan& guestFrameCensusPlan() {
    return censusCachedPlan();
}

// The one entry point a caller needs. extern "C" so the call site does not have
// to know it lives in namespace rr3 -- the census's own entry points
// (censusArm, censusNoteImport) are C++-mangled, which is fine when a header
// declares them and not fine when a one-line diagnostic hook wants to call one.
//
// INERT UNTIL CALLED. Nothing above starts this thread: it is not a static
// initialiser, censusArm does not call it, and no constructor does. A run that
// does not call this is a normal run.
//
// void rr3RootPcSamplerStart(void);   // declare this at the call site.
extern "C" void rr3RootPcSamplerStart(void) {
    if (gRootPcSamplerStarted) {
        return;
    }
    gRootPcSamplerStarted = true;
    pthread_t sampler{};
    // Joinable and never joined, exactly like censusStartReporter: this is a
    // diagnostic sampler and handoff must never block on it.
    if (::pthread_create(&sampler, nullptr, rootPcSamplerWorker, nullptr) != 0) {
        gRootPcSamplerStarted = false;
        censusLog("root-pc-sampler-start", 0, 0, "FAILED reason=pthread_create");
        return;
    }
    // The watchdog is started SECOND and independently. Its failure must not
    // stop the sampler -- the sampler is still useful without a watchdog -- but
    // it is reported, because "no watchdog line in the log" then has an
    // explanation instead of being its own mystery.
    pthread_t watchdog{};
    if (::pthread_create(&watchdog, nullptr, rootPcResumeWatchdogWorker,
                         nullptr) != 0) {
        censusLog("root-pc-resume-watchdog", 0, 0,
                  "FAILED reason=pthread_create "
                  "note=sampler_running_without_a_resume_bound");
    }
}

void censusArm() {
    if (censusDisabled()) {
        return;
    }
    bool start = false;
    mutexLock(&gCensusLock);
    if (!gCensusArmed) {
        gCensusArmed = true;
        gCensusSweepTick = censusTick();
        gCensusFirstTick = gCensusSweepTick;
        start = true;
    }
    mutexUnlock(&gCensusLock);
    if (start) {
        censusLog( "census-arm", 0,
                            gCensusTickFreq, "sweep=4s stall=8s");
        // BEFORE censusStartReporter, and therefore before any frame can be
        // read: the frame dump's negative readings mean nothing at all unless
        // the dumper has been shown to print what it read, and the only moment
        // that is guaranteed to precede every dump is before the reporter
        // exists. Runs exactly once, because `start` is set once under the lock.
        censusFrameDumpSelftest();
        // The thread that arms the census is the one driving guest image
        // relocation, i.e. the main thread. Bind it now so a main thread that
        // never imports again after boot is still tracked.
        censusBindCaller("arm-thread");
        censusStartReporter();
    }
}

void censusNoteImportFrom(const void* returnAddress, const char* symbol);

void censusNoteImport(const char* symbol) {
    censusNoteImportFrom(nullptr, symbol);
}

// Records that this guest thread reached an intercepted host import. The
// return address is the guest instruction that resumes once the shim returns,
// so the ring of the last few is the call chain that leads into a freeze. It
// must be captured by the shim itself: one level deeper, the link register
// would only name this function.
void censusNoteImportFrom(const void* returnAddress, const char* symbol) {
    if (!gCensusArmed) {
        return;
    }
    CensusEntry* entry = censusLookup(censusKey());
    if (entry == nullptr) {
        entry = censusBindCaller("first-import");
        if (entry == nullptr) {
            return;
        }
    }
    ++entry->imports;
    entry->lastSymbol = symbol != nullptr ? symbol : "(null)";
    entry->lastTick = censusTick();
    if (returnAddress != nullptr) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(returnAddress);
        entry->lastReturn = returnAddress;
        // The ring keeps the symbol name next to the return address: the bare
        // addresses resolved into libc++ guard sites, which is how the previous
        // 8-slot dump hid the actual host-call sequence.
        if (entry->recentCount < kCensusReturnRing) {
            entry->recentReturns[entry->recentCount] = address;
            entry->recentSymbols[entry->recentCount] = entry->lastSymbol;
            ++entry->recentCount;
        }
        else {
            for (size_t i = 1; i < kCensusReturnRing; ++i) {
                entry->recentReturns[i - 1] = entry->recentReturns[i];
                entry->recentSymbols[i - 1] = entry->recentSymbols[i];
            }
            entry->recentReturns[kCensusReturnRing - 1] = address;
            entry->recentSymbols[kCensusReturnRing - 1] = entry->lastSymbol;
        }
    }
    if (entry->watchNextImport) {
        entry->watchNextImport = false;
        char details[160];
        __builtin_snprintf(details, sizeof(details), "after=%s lr=%llx", symbol,
                           (unsigned long long)reinterpret_cast<uintptr_t>(returnAddress));
        censusLog("hbn-next-import", entry->ordinal, entry->imports, details);
    }
    if ((entry->imports & 0x3f) == 0) {
        censusMaybeSweep();
        censusNoteStall(entry);
    }
}

// Every intercepted guest import records the address it will return to. That
// has to be read in the shim itself: one level deeper the link register would
// only name this function.
// In-flight breadcrumb. Entering any bridge import bumps the caller's depth;
// leaving drops it. A thread observed with depth > 0 is parked inside this
// bridge; a thread with depth == 0 and a non-zero import count completed
// every bridge call it ever made and is therefore executing guest code.
// censusKey() is TPIDRRO_EL0, which survives GuestTlsScope, so entry and exit
// resolve to the same census entry.
void censusEnterBridge(const char* symbol) {
    if (!gCensusArmed) {
        return;
    }
    CensusEntry* entry = censusLookup(censusKey());
    if (entry == nullptr) {
        return;
    }
    if (entry->bridgeDepth == 0) {
        entry->bridgeSymbol = symbol;
    }
    ++entry->bridgeDepth;
    ++entry->bridgeEnters;
}

void censusLeaveBridge() {
    if (!gCensusArmed) {
        return;
    }
    CensusEntry* entry = censusLookup(censusKey());
    if (entry == nullptr) {
        return;
    }
    if (entry->bridgeDepth > 0) {
        --entry->bridgeDepth;
    }
    if (entry->bridgeDepth == 0) {
        entry->bridgeSymbol = nullptr;
    }
}

// RAII so the early returns all over this file cannot leak a depth.
struct BridgeFrame {
    BridgeFrame(const char* symbol) { censusEnterBridge(symbol); }
    ~BridgeFrame() { censusLeaveBridge(); }
    BridgeFrame(const BridgeFrame&) = delete;
    BridgeFrame& operator=(const BridgeFrame&) = delete;
};

#define CENSUS_IMPORT(symbol) \
    ::rr3::censusNoteImportFrom(__builtin_return_address(0), symbol); \
    ::rr3::BridgeFrame rr3BridgeFrame(symbol)

void censusWatchNextImport(const char* tag) {
    if (!gCensusArmed) {
        return;
    }    CensusEntry* entry = censusLookup(censusKey());
    if (entry != nullptr) {
        entry->watchNextImport = true;
        censusLog( "hbn-watch", entry->ordinal,
                            entry->imports, tag);
    }
}

namespace {

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

// Ledger taps. They only move counters; no locking beyond the registry lock.
void noteCondWait(void* condition, const void* caller) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_cond_t>* slot = findSlot(gConditions, condition);
    if (slot != nullptr) {
        ++slot->waits;
        slot->lastWaitReturn = caller;
        if (slot->waitersPeak == 0) {
            slot->waitersPeak = 1;
        }
    }
    mutexUnlock(&gRegistryLock);
}

void noteCondSignal(void* condition) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_cond_t>* slot = findSlot(gConditions, condition);
    if (slot != nullptr) {
        ++slot->signals;
    }
    mutexUnlock(&gRegistryLock);
}

void noteMutexWait(void* mutex, const void* caller) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_mutex_t>* slot = findSlot(gMutexes, mutex);
    if (slot != nullptr) {
        ++slot->waits;
        slot->lastWaitReturn = caller;
        if (slot->waitersPeak == 0) {
            slot->waitersPeak = 1;
        }
    }
    mutexUnlock(&gRegistryLock);
}

// ---------------------------------------------------------------------------
// Mutex ownership ledger.
//
// Every successful guest lock stamps the host-side slot with the owning guest
// thread and the guest return address that took it; every successful unlock
// clears it. A guest thread that exits still holding entries is the one state
// no POSIX call can clean up: the host pthread_mutex_t stays locked forever,
// and every guest thread that blocks on it never wakes. There is no way to see
// that from the host mutex alone, because a locked mutex does not remember who
// locked it.
//
// Ownership is only stamped for threads running inside a guest TLS scope, so
// the bridge's own host workers (reaper, TLS self-test) never claim a lock.
// ---------------------------------------------------------------------------

struct MutexOwnerTag {
    uintptr_t key{};
    uint32_t ordinal{};
    bool valid{};
};

// Hot path: runs on every successful lock, so it stays cheap. censusLookup's
// fast slot is a lock-free read of a write-once key, and CensusEntry::ordinal
// is assigned once at reserve and never rewritten, so reading it without
// gCensusLock is safe by construction rather than merely tolerated.
MutexOwnerTag currentMutexOwner() {
    MutexOwnerTag tag;
    if (!censusInsideGuestScope()) {
        return tag;
    }
    const CensusEntry* entry = censusLookup(censusKey());
    if (entry == nullptr) {
        return tag;
    }
    // Host threads are NOT excluded here. The main guest thread runs on the
    // host's root thread (census `host=1`), so skipping `entry->host` meant
    // that every lock the main thread took was never ownership-stamped: the
    // ledger reported `held_now=0` and `acquires=0` for objects the main
    // thread was demonstrably contending on (one object showed
    // `waits=55 acquires=0 owner_ord=0`). "Not held" could not rule out a
    // main-thread-held orphan, which is the exact case we needed to see.
    // censusInsideGuestScope() above already excludes the census reporter,
    // the reaper and the self-tests.
    tag.key = entry->key;
    tag.ordinal = entry->ordinal;
    tag.valid = true;
    return tag;
}

void mutexNoteAcquire(void* guest, const void* site) {
    const MutexOwnerTag tag = currentMutexOwner();
    if (!tag.valid) {
        return;
    }
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_mutex_t>* slot = findSlot(gMutexes, guest);
    if (slot != nullptr) {
        slot->ownerKey = tag.key;
        slot->ownerOrdinal = tag.ordinal;
        slot->acquireSite = site;
        slot->acquireTick = censusTick();
        slot->held = true;
        ++slot->acquires;
    }
    mutexUnlock(&gRegistryLock);
}

// Real contention, counted in the EBUSY loop of guestPthreadMutexLock. Separate
// from `waits`, which counts requests; see the note at the call site.
void mutexNoteContended(void* guest, uint64_t spins) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_mutex_t>* slot = findSlot(gMutexes, guest);
    if (slot != nullptr) {
        slot->blockedSpins += spins;
        if (spins > slot->worstSpins) {
            slot->worstSpins = spins;
        }
    }
    mutexUnlock(&gRegistryLock);
}

void mutexNoteRelease(void* guest) {
    mutexLock(&gRegistryLock);
    ObjectSlot<pthread_mutex_t>* slot = findSlot(gMutexes, guest);
    if (slot != nullptr) {
        slot->held = false;
        slot->ownerKey = 0;
        slot->acquireSite = nullptr;
    }
    mutexUnlock(&gRegistryLock);
}

// Who holds guest mutexes right now, and -- the number this whole ledger
// exists for -- how many are held by a thread that has already exited.
// Sampled every sweep, so a spin that starts mid-run is caught against the
// owner it is actually spinning on.
void censusMutexLedger(unsigned int sweep) {
    constexpr size_t kMaxRows = 8;
    struct Row {
        uint64_t ageMs{};
        uint64_t acquires{};
        const void* guest{};
        const void* site{};
        uintptr_t ownerKey{};
        uint32_t ordinal{};
        bool orphan{};
    };
    Row rows[kMaxRows];
    size_t count = 0;
    uint64_t used = 0;
    uint64_t held = 0;
    uint64_t orphans = 0;
    uint64_t totalAcquires = 0;
    uint64_t totalWaits = 0;
    uint64_t totalBlocked = 0;

    // Snapshot the mutex table first, then resolve owner liveness separately:
    // gRegistryLock and gCensusLock are never nested here or anywhere else.
    struct Held {
        uint64_t tick{};
        uint64_t acquires{};
        const void* guest{};
        const void* site{};
        uintptr_t ownerKey{};
        uint32_t ordinal{};
    };
    Held heldList[kMaxRows * 4];
    size_t heldCount = 0;
    // A lock nobody holds, but that somebody has waited on. Three fields, no
    // owner, because that absence is the finding.
    // `acquireSite` is the WRONG field for this report and reading it is how the
    // first pass of these rows came back all-zero. It is written only by
    // mutexNoteAcquire, i.e. only for a lock that was successfully taken and
    // ownership-stamped; these objects are waited on far more often than they are
    // acquired, so for most of them it is legitimately 0. The guest address that
    // actually waited is `lastWaitReturn`, written by noteMutexWait. Both are
    // printed: `acq_site` says who last took it, `wait_site` says who is stuck.
    struct Waited { uint64_t waits; const void* guest; uint64_t acquires;
                    const void* site; const void* waitSite;
                    uint64_t blockedSpins; };
    // The second list, and the reason this file could not see the wall it was
    // built to see. `heldList` is filled ONLY from slots with `held` set, so a
    // lock nobody holds produces no row at all -- by construction. That is
    // exactly backwards for the case that matters: a thread blocked acquiring a
    // mutex shows up as `held_now=0` and an empty detail list, and the report
    // then says "nothing is held" while the main thread sits in
    // `svcArbitrateLock`. Measured 2026-10-05, archive
    // 20261005-081112-hostelfva: `mutexes_known=48 held_now=0
    // total_acquires=1221` and **zero** census-mutex-* rows, with the main
    // thread's PC at the arbitration stub in 2 of its 4 samples.
    //
    // `waits` is the discriminator this report never had: `waits > 0 && !held`
    // is "someone is queued on a lock that nobody owns", and it is invisible to
    // a held-only list no matter how many acquires went through it.
    Waited waitedList[kMaxRows];
    size_t waitedCount = 0;
    const uint64_t now = censusTick();
    mutexLock(&gRegistryLock);
    for (size_t i = 0; i < kMutexCapacity; ++i) {
        const ObjectSlot<pthread_mutex_t>& slot = gMutexes[i];
        if (!slot.used) {
            continue;
        }
        totalBlocked += slot.blockedSpins;
        ++used;
        totalAcquires += slot.acquires;
        totalWaits += slot.waits;
        if (!slot.held) {
            // Ranked by REAL contention (blockedSpins), not by `waits`. `waits`
            // is a request count -- noteMutexWait fires on every entry to
            // guestPthreadMutexLock, contended or not -- so ranking by it lists
            // the most-used locks, which is how `waits == acquires` came to read
            // as "510 threads queueing on a lock nobody holds". It is a
            // tautology: one call, two counters, in sequence.
            if (slot.blockedSpins > 0
                && waitedCount < sizeof(waitedList) / sizeof(waitedList[0])) {
                const Waited w{slot.blockedSpins, slot.guest, slot.acquires,
                               slot.acquireSite, slot.lastWaitReturn,
                               slot.blockedSpins};
                size_t j = waitedCount++;
                while (j > 0 && waitedList[j - 1].blockedSpins < w.blockedSpins) {
                    waitedList[j] = waitedList[j - 1];
                    --j;
                }
                waitedList[j] = w;
            }
            continue;
        }
        ++held;
        if (heldCount < sizeof(heldList) / sizeof(heldList[0])) {
            heldList[heldCount++] = Held{slot.acquireTick, slot.acquires,
                                         slot.guest, slot.acquireSite,
                                         slot.ownerKey, slot.ownerOrdinal};
        }
    }
    mutexUnlock(&gRegistryLock);

    for (size_t i = 0; i < heldCount; ++i) {
        const Held& entry = heldList[i];
        const CensusEntry* owner = censusLookup(entry.ownerKey);
        const bool orphan = owner == nullptr || owner->exited || owner->host;
        if (orphan) {
            ++orphans;
        }
        if (count < kMaxRows) {
            rows[count++] = Row{
                censusMillis(now - entry.tick), entry.acquires, entry.guest,
                entry.site, entry.ownerKey, entry.ordinal, orphan};
        }
    }
    // Orphans first, then the longest-held, which is the most likely to be the
    // one everybody is stuck behind.
    for (size_t i = 0; i < count; ++i) {
        const Row key = rows[i];
        size_t j = i;
        while (j > 0 && (rows[j - 1].orphan < key.orphan
                         || (rows[j - 1].orphan == key.orphan
                             && rows[j - 1].ageMs < key.ageMs))) {
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = key;
    }
    char summary[224];
    __builtin_snprintf(summary, sizeof(summary),
                       "mutexes_known=%llu held_now=%llu held_by_exited=%llu "
                       "total_acquires=%llu requests=%llu blocked_spins=%llu contended=%zu",
                       (unsigned long long)used, (unsigned long long)held,
                       (unsigned long long)orphans,
                       (unsigned long long)totalAcquires,
                       (unsigned long long)totalWaits,
                       (unsigned long long)totalBlocked, waitedCount);
    censusLog("census-mutex-sum", sweep, held, summary);
    for (size_t i = 0; i < waitedCount; ++i) {
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "guest=%llx waits=%llu acquires=%llu held=0 "
                           "acq_site=%llx wait_site=%llx "
                           "(waits>0 with nobody holding)",
                           (unsigned long long)waitedList[i].guest,
                           (unsigned long long)waitedList[i].waits,
                           (unsigned long long)waitedList[i].acquires,
                           (unsigned long long)waitedList[i].site,
                           (unsigned long long)waitedList[i].waitSite);
        censusLog("census-mutex-waited", sweep,
                  reinterpret_cast<uintptr_t>(waitedList[i].guest), details);
    }
    for (size_t i = 0; i < count; ++i) {
        char details[224];
        __builtin_snprintf(details, sizeof(details),
                           "orphan=%d guest=%llx owner_ord=%u owner_key=%llx "
                           "acquire_site=%llx held_ms=%llu acquires=%llu",
                           rows[i].orphan ? 1 : 0,
                           (unsigned long long)rows[i].guest,
                           rows[i].ordinal,
                           (unsigned long long)rows[i].ownerKey,
                           (unsigned long long)rows[i].site,
                           (unsigned long long)rows[i].ageMs,
                           (unsigned long long)rows[i].acquires);
        censusLog(rows[i].orphan ? "census-mutex-orphan" : "census-mutex-held",
                  rows[i].ordinal, reinterpret_cast<uintptr_t>(rows[i].guest),
                  details);
    }
    // libc++'s internal guard mutex/condvar live in libc++'s own data segment,
    // so the per-mutex rows above never mention them by name. Read them here,
    // where census-mutex-sum is emitted: the sweep runs every 4 s off the back
    // of whatever threads are still importing, so this point is reached during
    // the wedge even though the wedged thread itself stopped importing (and
    // therefore can no longer reach censusNoteStall). The probe is once-only.
    censusLibcxxSyncProbe(sweep);
}

// ---------------------------------------------------------------------------
// libc++ internal sync-word probe (bounded: once per run).
//
// libc++'s __cxa_guard_acquire/release serialise on ONE process-wide mutex
// that lives in libc++'s own data segment (ELF +0x100048) and wait on a
// condvar at +0x100070. From the disassembly these are reached through the
// ordinary pthread_mutex_lock/unlock and pthread_cond_wait/broadcast imports
// (libc++ __libcpp_* wrappers at 0xec880/0xec890/0xecbc0/0xecbb0), so the
// mutex is a plain pthread_mutex_t the bridge already tracks as a HOST shadow
// keyed by that guest address -- it is NOT invisible to the ledger. What the
// ledger cannot show is the guest's own bytes, because the bridge never
// interprets them. So this reports both: the raw guest word for the record,
// and the shadow's held/owner state, which is the only thing that actually
// answers "is it locked, and by whom".
//
// The shadow is the load-bearing part: on this host a locked pthread_mutex_t
// is a Horizon kernel handle whose __lock word is only meaningful to the
// kernel, so the guest bytes at +0x100048 are expected to read as a constant
// whether the mutex is held or free. Reporting them raw keeps that honest.
// ---------------------------------------------------------------------------
void censusLibcxxSyncProbe(uint32_t ordinal) {
    mutexLock(&gRegistryLock);
    static bool reported = false;
    if (reported) {
        mutexUnlock(&gRegistryLock);
        return;
    }
    reported = true;
    mutexUnlock(&gRegistryLock);

    const GuestStartupPlan plan = captureGuestStartupPlan();
    uintptr_t libcxxBase = 0;
    for (uint64_t i = 0; i < plan.imageCount && i < kGuestStartupMaximumImages; ++i) {
        if (std::strcmp(plan.images[i].soname, "libc++_shared.so") == 0) {
            libcxxBase = static_cast<uintptr_t>(plan.images[i].base);
            break;
        }
    }
    char line[320];
    if (libcxxBase == 0) {
        censusLog("libcxx-sync", ordinal, 0, "img=libc++_shared.so NOT-LOADED");
        return;
    }

    // Raw guest words at the two internal sync objects, mapping-guarded.
    static const uint32_t kObjects[] = {0x00100048u, 0x00100070u};
    static const char* const kNames[] = {"guard-mutex", "guard-condvar"};
    for (size_t o = 0; o < 2; ++o) {
        uint64_t word = 0;
        const uintptr_t addr = libcxxBase + kObjects[o];
        const bool ok = censusReadGuardWord(addr, &word);
        __builtin_snprintf(line, sizeof(line),
                           "obj=%s off=0x%08x addr=0x%llx raw=0x%016llx readable=%d",
                           kNames[o], kObjects[o], (unsigned long long)addr,
                           (unsigned long long)word, ok ? 1 : 0);
        censusLog("libcxx-obj", ordinal, static_cast<uint32_t>(o), line);
    }

    // The locked set, from the shadow table, selected with the single
    // attribution rule (censusImageOffsetOf) so nothing is re-derived. Every
    // guest mutex that lands in libc++'s image is one libc++ owns; each row
    // carries the ledger state that answers "locked, and by whom".
    uint32_t inLibcxx = 0;
    uint32_t heldCount = 0;
    uint32_t emitted = 0;
    mutexLock(&gRegistryLock);
    for (size_t i = 0; i < kMutexCapacity; ++i) {
        const ObjectSlot<pthread_mutex_t>& slot = gMutexes[i];
        if (!slot.used) {
            continue;
        }
        const uintptr_t guest = reinterpret_cast<uintptr_t>(slot.guest);
        const char* soname = "host";
        const uintptr_t off = censusImageOffsetOf(plan, guest, &soname);
        if (soname == nullptr || std::strcmp(soname, "libc++_shared.so") != 0) {
            continue;
        }
        ++inLibcxx;
        if (slot.held) {
            ++heldCount;
        }
        if (emitted >= 16) {
            continue;
        }
        ++emitted;
        const char* siteSoname = "host";
        const uintptr_t siteOff = censusImageOffsetOf(
            plan, reinterpret_cast<uintptr_t>(slot.acquireSite), &siteSoname);
        __builtin_snprintf(line, sizeof(line),
            "guest=0x%llx off=0x%llx held=%d owner_ord=%u owner_key=0x%llx "
            "acquires=%llu waits=%llu waiters_peak=%u lastwait_site=%s+0x%llx",
            (unsigned long long)guest, (unsigned long long)off,
            slot.held ? 1 : 0, slot.ownerOrdinal,
            (unsigned long long)slot.ownerKey, (unsigned long long)slot.acquires,
            (unsigned long long)slot.waits, slot.waitersPeak,
            siteSoname, (unsigned long long)siteOff);
        censusLog("libcxx-mutex", ordinal, emitted, line);
    }
    mutexUnlock(&gRegistryLock);
    __builtin_snprintf(line, sizeof(line),
                       "libcxx_base=0x%llx tracked_in_libcxx=%u held=%u "
                       "emitted=%u cap=16",
                       (unsigned long long)libcxxBase, inLibcxx, heldCount,
                       emitted);
    censusLog("libcxx-sync-end", ordinal, 0, line);
}

// The first-class diagnostic: for every condvar the guest ever waited on, the
// number of waits it took minus the number of notifications it received. A
// positive deficit means waiters parked on it and nobody ever signalled, which
// is exactly the failure this whole investigation is about. Objects are
// identified by guest address plus the guest return address that waited on
// them, so the report names the guest code doing the waiting.
//
// Mutexes are deliberately not scored here: a lock pairs with an unlock, so a
// raw deficit would be noise. Whether a thread is stuck inside a mutex lock is
// answered by the inbridge breadcrumb instead.
void censusWaitLedger(unsigned int sweep) {
    constexpr size_t kMaxRows = 32;
    struct Row {
        uint64_t deficit{};
        uint64_t waits{};
        uint64_t signals{};
        const void* object{};
        const void* caller{};
    };
    Row rows[kMaxRows];
    size_t count = 0;
    uint64_t totalWaits = 0;
    uint64_t totalSignals = 0;
    uint64_t objectsWaited = 0;
    uint64_t unsignalledObjects = 0;
    uint64_t worstDeficit = 0;

    mutexLock(&gRegistryLock);
    for (size_t i = 0; i < kCondCapacity; ++i) {
        const ObjectSlot<pthread_cond_t>& slot = gConditions[i];
        if (!slot.used || slot.waits == 0) {
            continue;
        }
        ++objectsWaited;
        totalWaits += slot.waits;
        totalSignals += slot.signals;
        const uint64_t deficit = slot.waits - slot.signals;
        if (deficit > 0) {
            ++unsignalledObjects;
            if (deficit > worstDeficit) {
                worstDeficit = deficit;
            }
        }
        if (count < kMaxRows) {
            rows[count++] = Row{deficit, slot.waits, slot.signals,
                                slot.guest, slot.lastWaitReturn};
        }
    }
    mutexUnlock(&gRegistryLock);

    for (size_t i = 1; i < count; ++i) {
        const Row key = rows[i];
        size_t j = i;
        while (j > 0 && rows[j - 1].deficit < key.deficit) {
            rows[j] = rows[j - 1];
            --j;
        }
        rows[j] = key;
    }
    if (count > 6) {
        count = 6;
    }
    for (size_t i = 0; i < count; ++i) {
        char details[192];
        __builtin_snprintf(details, sizeof(details),
                           "kind=cond obj=%llx waited=%llu signalled=%llu "
                           "deficit=%llu wait_caller=%llx",
                           (unsigned long long)rows[i].object,
                           (unsigned long long)rows[i].waits,
                           (unsigned long long)rows[i].signals,
                           (unsigned long long)rows[i].deficit,
                           (unsigned long long)rows[i].caller);
        censusLog("census-wait", sweep, rows[i].deficit, details);
    }
    char summary[192];
    __builtin_snprintf(summary, sizeof(summary),
                       "condvars_waited_on=%llu total_waits=%llu "
                       "total_signals=%llu unsignalled_objects=%llu "
                       "worst_deficit=%llu",
                       (unsigned long long)objectsWaited,
                       (unsigned long long)totalWaits,
                       (unsigned long long)totalSignals,
                       (unsigned long long)unsignalledObjects,
                       (unsigned long long)worstDeficit);
    censusLog("census-wait-sum", sweep, objectsWaited, summary);
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
    CensusEntry* census;
};

void* guestThreadStart(void* opaque) {
    const GuestThreadStartContext context = *static_cast<GuestThreadStartContext*>(opaque);
    std::free(opaque);
    censusAttach(context.census);
    GuestTlsScope tlsScope;
    if (!tlsScope.active()) {
        std::abort();
    }
    void* result = context.start(context.argument);
    censusDetach(context.census);
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
    CENSUS_IMPORT("pthread_create");
    mutexLock(&gRegistryLock);
    if (gQuiesceRequested) {
        mutexUnlock(&gRegistryLock);
        return EBUSY;
    }
    const size_t guestStackSize = stackSizeForAttributeLocked(attribute);
    mutexUnlock(&gRegistryLock);

    // The bridge's own reaper and TLS self-test worker are host threads, not
    // guest threads: keep them out of the census table.
    const bool hostOwned = start == threadSelfTestStart ||
        start == reapDetachedThreadWorker;
    // Captured here, in the frame the guest actually called into, so the value
    // is the guest instruction that resumes once the shim returns. Taken one
    // level down it would name this function instead, and reading a foreign
    // thread's frame chain does not work on this platform, so the immediate
    // caller is the only frame that can be reported honestly.
    const uintptr_t creatorReturn =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    CensusEntry* census = censusReserveChild(
        reinterpret_cast<uintptr_t>(start),
        reinterpret_cast<uintptr_t>(argument), 0, hostOwned, creatorReturn);
    if (census != nullptr) {
        CensusEntry* creator = censusLookup(censusKey());
        census->creatorOrdinal = creator != nullptr ? creator->ordinal : 0;
    }

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
            *context = {start, argument, census};
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

// ---------------------------------------------------------------------------
// Reaper join bracketing -- INSTRUMENTATION ONLY. Nothing here changes the
// join, the reaper loop, the detach path, or any timeout.
//
// Why: ::pthread_join is the reaper's only untimed blocking call, and it is
// entered with gRegistryLock already dropped (:3942), so a guest thread that
// never exits parks the reaper here with no bound and no breadcrumb. Its wait
// is bounded at 1 s (kReaperWaitSliceNanos), so the absence of a third
// reaper-wait-expired is itself the signature of being stuck somewhere else --
// and until these two lines exist, "somewhere else" cannot be narrowed.
//
// Bound, in the kCensusSpinWalks shape: at most kReapJoinTraceLines
// before-lines are EVER emitted, and each one is always followed by its own
// after-line (the flag is a local, not a second budget), so the total is at
// most 2 * kReapJoinTraceLines lines for the life of the process no matter how
// many joins happen. A join that blocks forever shows up as a before-line with
// no after-line, which is the whole point; a stream of successful joins costs
// 4 lines and then goes silent. The counter is touched only while
// gRegistryLock is held, which is why it is a plain unsigned and not an
// atomic, and why no <atomic> and no new lock is needed.
//
// The lines go through guestViewWideLog -> appendGuestBootLine, the same
// fsync-free cached-handle append the reaper lines already use, and are
// emitted with gRegistryLock DROPPED so the file write never extends the hold
// time of the registry lock (the existing reaper-wait-expired line, by
// contrast, is emitted while holding it).
//
// boot_ms is censusMillis(censusTick()): censusTick() is armGetSystemTick(),
// the cntpct_el0 register read this file already uses, scaled by
// armGetSystemTickFreq() -- the existing convention (see the census-reporter-
// wake line). cntpct_el0 is zeroed at boot, so that is a host wall-clock stamp
// in ms since boot, which is what the guest-view log otherwise lacks entirely.
// ---------------------------------------------------------------------------
constexpr unsigned kReapJoinTraceLines = 4;
// gRegistryLock only; see the note above.
unsigned gReapJoinTraceBefores = 0;

const char* reapLifecycleName(int lifecycle) {
    switch (static_cast<ThreadLifecycle>(lifecycle)) {
    case ThreadLifecycle::Running:
        return "Running";
    case ThreadLifecycle::Detached:
        return "Detached";
    case ThreadLifecycle::Joined:
        return "Joined";
    }
    return "unknown";
}

void reapJoinTrace(const char* phase, size_t ordinal, int lifecycle,
                   bool joinInProgress, uintptr_t host, int joinResult,
                   uint64_t bootMillis, uint64_t elapsedMillis) {
    char details[192];
    __builtin_snprintf(
        details, sizeof(details),
        "slot=%zu lifecycle=%s joinInProgress=%d host=0x%llx cur=%u "
        "boot_ms=%llu joinResult=%d elapsed_ms=%llu",
        ordinal, reapLifecycleName(lifecycle), joinInProgress ? 1 : 0,
        (unsigned long long)host, (unsigned int)threadGetCurHandle(),
        (unsigned long long)bootMillis, joinResult,
        (unsigned long long)elapsedMillis);
    guestViewWideLog(phase, (unsigned long long)ordinal,
                     (unsigned long long)host, details);
}

void reapDetachedThreadSlotLocked(ThreadSlot& slot) {
    // Instrumentation only: the join, the lock dance and the recycle decision
    // below are byte-for-byte the originals. What is new is that the slot's
    // identity is captured while the registry lock is still held, because
    // reading slot.lifecycle / slot.joinInProgress after mutexUnlock would
    // race recycleDetachedThreadSlotLocked and the next detach.
    //
    // lifecycle and joinInProgress are the values observed ON ENTRY, i.e.
    // before the assignment below, so joinInProgress=0 here means this is the
    // first entry into this slot and not a re-entrant join on one already
    // being joined.
    const size_t ordinal = static_cast<size_t>(&slot - gThreads);
    const int lifecycleAtEntry = static_cast<int>(slot.lifecycle);
    const bool joinInProgressAtEntry = slot.joinInProgress;
    const uintptr_t hostAtEntry = reinterpret_cast<uintptr_t>(slot.host);
    const bool traceJoin = gReapJoinTraceBefores < kReapJoinTraceLines;
    if (traceJoin) {
        ++gReapJoinTraceBefores;
    }
    slot.joinInProgress = true;
    mutexUnlock(&gRegistryLock);
    const uint64_t joinTick = censusTick();
    if (traceJoin) {
        reapJoinTrace("reap-join-before", ordinal, lifecycleAtEntry,
                      joinInProgressAtEntry, hostAtEntry, -1,
                      censusMillis(joinTick), 0);
    }
    void* ignored = nullptr;
    const int joinResult = ::pthread_join(slot.host, &ignored);
    if (traceJoin) {
        const uint64_t doneTick = censusTick();
        reapJoinTrace("reap-join-after", ordinal, lifecycleAtEntry,
                      joinInProgressAtEntry, hostAtEntry, joinResult,
                      censusMillis(doneTick), censusMillis(doneTick - joinTick));
    }
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
    CENSUS_IMPORT("pthread_join");
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

// The reaper's only wait, in bounded rounds.
//
// libnx's condvarWait is condvarWaitTimeout(c, m, UINT64_MAX): no bound and no
// report, so a lost wake-up here is completely silent. That is the one genuine
// lost-wake-up hole in the tree, and its failure mode is far away from its
// cause: the detached ThreadSlots are never recycled, the kThreadCapacity (96)
// table fills, and guestPthreadCreate starts returning ENOMEM from
// createThreadSlotLocked with nothing in the log pointing at the reaper.
// The join side of the same hazard is already worked around, with a comment, in
// guestPthreadQuiesceAll below.
//
// One line per expired round, and only while there is nothing to reap, so a
// reaper that reaps on every wake-up never reaches the report: the emission is
// bounded by the slice, not by hope.
constexpr uint64_t kReaperWaitSliceNanos = 1000000000ULL;
// libnx's timeout Result (condvar.h:29: "@return Result code (0xEA01 on
// timeout)"). This is NOT errno ETIMEDOUT, which is why it is named here
// rather than borrowed from the guestPthreadCondWait pattern below.
constexpr Result kReaperWaitTimedOut = 0xEA01;

void* reapDetachedThreadWorker(void*) {
    // Total time this reaper has been blocked, accumulated across every round
    // of the inner loop, so a report says how long it has been waiting overall
    // and not merely that it waited once. Reset only when a round is woken by
    // condvarWakeAll, i.e. when a detach or a quiesce actually happened.
    unsigned long long waitedNanos = 0;
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
            const Result waitResult = condvarWaitTimeout(
                &gThreadsChanged, &gRegistryLock, kReaperWaitSliceNanos);
            if (R_FAILED(waitResult) && waitResult != kReaperWaitTimedOut) {
                // A real error, not an expiry: keep the original exit
                // condition. (guestViewTraceSyncBlocked has no channel for a
                // payload, so the running total is reported via
                // guestViewWideLog, which is the same appendGuestBootLine the
                // other three blocked-wait breadcrumbs end up writing.)
                mutexUnlock(&gRegistryLock);
                return nullptr;
            }
            if (waitResult == 0) {
                waitedNanos = 0;
                continue;
            }
            // Timed out: re-check quiesce and pending above before re-arming.
            waitedNanos += kReaperWaitSliceNanos;
            // boot_ms is the same host wall-clock stamp reapJoinTrace reports
            // (censusMillis(censusTick())), added here because waited_ns alone
            // cannot answer "does the cliff move between runs": the guest-view
            // log has no timestamps, so without this the expiry times are only
            // ever known relative to each other. Not a new line, only a new
            // field on the existing bounded one, so the emission bound below
            // (one per expired 1 s slice) is unchanged.
            char details[512]{};
            __builtin_snprintf(details, sizeof(details),
                               "waited_ns=%llu slice_ns=%llu boot_ms=%llu",
                               waitedNanos,
                               (unsigned long long)kReaperWaitSliceNanos,
                               (unsigned long long)censusMillis(censusTick()));

            // ---- the asset singleton, sampled on this 1 Hz tick ---------------
            //
            // Why HERE and not in the frame loop: the fatal dialog is raised
            // INSIDE the second onViewRenderJNI (faults from 5.827 s, the box
            // at 6.049 s) and that call never returns, so no frames >= 2
            // iteration exists. A probe in the loop can only ever read the state
            // BEFORE the transition. This thread is confirmed to tick for the
            // whole run, which is what makes it the only place a read can
            // bracket the transition in time.
            //
            // Why this address: tools/guest-analysis/xref-str.py located the
            // raise of GAMETEXT_GAME_DATA_FAILED_TO_LOAD_ERROR_MESSAGE at
            // 0xcd35c0 in FDE [0xcd1014, 0xcd3648), and the only non-cleanup
            // branch into it is
            //
            //   0xcd1478  ldr  x28, [x28, #0x5d0]   ; x28 = *(0x1f6d5d0)
            //   0xcd16fc  ldr  x0, [x28]
            //   0xcd1700  cbz  x0, #0xcd35c0        ; NULL -> raise
            //
            // "Singleton null" and "singleton present, first word null" are
            // different bugs with opposite fixes, and a single pre-wedge read
            // cannot tell them apart, so both are reported on every tick. The
            // field is appended to the EXISTING bounded line rather than being
            // a new one, so the emission budget above is unchanged.
            {
                static constexpr uint64_t kAssetSingletonElfVa = 0x1F6D5D0ULL;
                static constexpr uint64_t kEngineSingletonElfVa = 0x1F6D5D8ULL;
                // The .rela.dyn addend for the 0x1F6D5D0 .got slot. Stated
                // here as a literal because the point of this check is to make
                // the bias self-verifying: if the slot really is a
                // R_AARCH64_RELATIVE whose only value is addend+bias, then
                // reading the slot must satisfy that identity or the bias we
                // used to find the slot is wrong and every other value read
                // through it is unrelated memory.
                //
                // 2026-10-04: this assert was added after a whole night of
                // conclusions drawn from values this probe read with a bias
                // 4 MB off. Four different biases had been reported across the
                // project and none had been reconciled; the probe printed a
                // confident NULL-FIRST-WORD from an address that was not the
                // singleton. Self-consistency is not correctness.
                static constexpr uint64_t kAssetSlotAddend = 0x1FC6E98ULL;
                const uintptr_t biasNow = resolveMainImageLoadBias();
                const uintptr_t slot =
                    static_cast<uintptr_t>(biasNow + kAssetSingletonElfVa);
                const uintptr_t obj =
                    *reinterpret_cast<const volatile uintptr_t*>(slot);
                const bool biasAgrees = (biasNow + kAssetSlotAddend) == obj;
                if (!biasAgrees) {
                    // Report the disagreement and nothing else. Printing the
                    // chain anyway is what produced the retracted readings.
                    char bdiag[224];
                    __builtin_snprintf(
                        bdiag, sizeof(bdiag),
                        "asset-probe: REFUSING bias=0x%llx slot=0x%llx read=0x%llx"
                        " expected=0x%llx delta=0x%llx -- coordinate system"
                        " unverified, no other value reported",
                        (unsigned long long)biasNow,
                        (unsigned long long)slot,
                        (unsigned long long)obj,
                        (unsigned long long)(biasNow + kAssetSlotAddend),
                        (unsigned long long)(obj - (biasNow + kAssetSlotAddend)));
                    guestViewWideLog("reaper-asset-singleton", waitedNanos,
                                     kReaperWaitSliceNanos, bdiag);
                    mutexUnlock(&gRegistryLock);
                    continue;
                }
                if (biasNow != 0) {
                const uintptr_t first = (obj != 0)
                        ? *reinterpret_cast<const volatile uintptr_t*>(obj) : 0;
                    // Walk the ENGINE chain that 0x13bca64 and 0x12bcea8 use,
                    // one rung at a time, because "the table lookup missed" and
                    // "the table was never populated" are different bugs and
                    // only this can tell them apart:
                    //   engine = *(0x1f6d5d8); f8 = engine[1];
                    //   vtable = *f8;  table = vtable[0x38]
                    // engine->f8, vtable[0x38] and the first entries are all
                    // reported, so a zero is localised to a rung instead of
                    // being inferred.
                    const uintptr_t engine =
                        *reinterpret_cast<const volatile uintptr_t*>(
                            static_cast<uintptr_t>(biasNow + kEngineSingletonElfVa));
                    const uintptr_t ef8 = (engine != 0)
                        ? *reinterpret_cast<const volatile uintptr_t*>(engine + 8) : 0;
                    const uintptr_t vt = (ef8 != 0)
                        ? *reinterpret_cast<const volatile uintptr_t*>(ef8) : 0;
                    const uintptr_t table = (vt != 0)
                        ? *reinterpret_cast<const volatile uintptr_t*>(vt + 0x38) : 0;
                    uint32_t e0 = 0, e1 = 0, e2 = 0, e3 = 0;
                    if (table != 0) {
                        const volatile uint32_t* t =
                            reinterpret_cast<const volatile uint32_t*>(table);
                        e0 = t[0]; e1 = t[1]; e2 = t[2]; e3 = t[3];
                    }
                    __builtin_snprintf(details + __builtin_strlen(details),
                                       sizeof(details) - __builtin_strlen(details),
                                       " asset=%llx/%llx/%s"
                                       " eng=%llx f8=%llx vt=%llx table=%llx"
                                       " t0=%08x t1=%08x t2=%08x t3=%08x",
                                       static_cast<unsigned long long>(obj),
                                       static_cast<unsigned long long>(first),
                                       obj == 0 ? "NULL-SINGLETON"
                                                : (first == 0 ? "NULL-FIRST-WORD"
                                                              : "PRESENT"),
                                       static_cast<unsigned long long>(engine),
                                       static_cast<unsigned long long>(ef8),
                                       static_cast<unsigned long long>(vt),
                                       static_cast<unsigned long long>(table),
                                       e0, e1, e2, e3);
                }
            }
            guestViewWideLog("reaper-asset-singleton", waitedNanos,
                             kReaperWaitSliceNanos, details);
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
    CENSUS_IMPORT("pthread_detach");
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
    // condvarWaitTimeout on gThreadsChanged; joining it while it is still
    // waiting deadlocks the process at handoff-quiesce-enter (device rounds
    // 7-8). The wait is now sliced, so the reaper wakes on its own every
    // kReaperWaitSliceNanos and re-checks this flag -- but the ordering below
    // is still what guarantees the join, and it is unchanged.
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
    CENSUS_IMPORT("pthread_self");
    return reinterpret_cast<uintptr_t>(::pthread_self());
}

[[noreturn]] void guestPthreadExit(void* value) {
    CENSUS_IMPORT("pthread_exit");
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
    CENSUS_IMPORT("pthread_mutex_lock");
    noteMutexWait(mutex, __builtin_return_address(0));
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    if (result != 0) return result;
    int lockResult = ::pthread_mutex_trylock(host);
    uint64_t spins = 0;
    while (lockResult == EBUSY) {
        ++spins;
        guestViewTraceSyncBlocked("mutex", 52);
        svcSleepThread(kSyncBlockedReportNanos);
        lockResult = ::pthread_mutex_trylock(host);
    }
    // REAL contention, counted where it actually happens.
    //
    // `noteMutexWait` is called unconditionally on entry to this function, once
    // per REQUEST, whether or not the lock is contended -- so `waits` in the
    // ledger means "asked for", not "waited for". A first pass of the
    // census-mutex-waited rows read `waits == acquires` as "510 threads queueing
    // on a lock nobody holds" (20261005-084007-waitsite). It is a tautology: both
    // counters are incremented by the same call, in sequence.
    //
    // The number that means contention is how many times this loop went round,
    // and the 2026-10-05 run measured it as ZERO everywhere:
    // `census-wait-sum condvars_waited_on=0 total_waits=0`, and no sync-blocked
    // record of any kind. The main thread's PC inside `mutexLock+80` is a thread
    // that happened to be executing a lock when the census paused it, not a
    // thread waiting for one.
    if (spins != 0) {
        mutexNoteContended(mutex, spins);
    }
    if (lockResult == 0) {
        mutexNoteAcquire(mutex, __builtin_return_address(0));
    }
    return lockResult;
}

int guestPthreadMutexTryLock(void* mutex) {
    CENSUS_IMPORT("pthread_mutex_trylock");
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    if (result != 0) return result;
    const int lockResult = ::pthread_mutex_trylock(host);
    if (lockResult == 0) {
        mutexNoteAcquire(mutex, __builtin_return_address(0));
    }
    return lockResult;
}

int guestPthreadMutexUnlock(void* mutex) {
    CENSUS_IMPORT("pthread_mutex_unlock");
    pthread_mutex_t* host = nullptr;
    const int result = getMutex(mutex, &host);
    if (result != 0) return result;
    const int unlockResult = ::pthread_mutex_unlock(host);
    if (unlockResult == 0) {
        mutexNoteRelease(mutex);
    }
    return unlockResult;
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
    CENSUS_IMPORT("pthread_cond_wait");
    noteCondWait(condition, __builtin_return_address(0));
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
    CENSUS_IMPORT("pthread_cond_signal");
    noteCondSignal(condition);
    pthread_cond_t* host = nullptr;
    const int result = getCondition(condition, &host);
    return result == 0 ? ::pthread_cond_signal(host) : result;
}

int guestPthreadCondBroadcast(void* condition) {
    CENSUS_IMPORT("pthread_cond_broadcast");
    noteCondSignal(condition);
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
    CENSUS_IMPORT("pthread_rwlock_rdlock");
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_rdlock(host) : result;
}

int guestPthreadRwlockWriteLock(void* lock) {
    CENSUS_IMPORT("pthread_rwlock_wrlock");
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_wrlock(host) : result;
}

int guestPthreadRwlockUnlock(void* lock) {
    CENSUS_IMPORT("pthread_rwlock_unlock");
    pthread_rwlock_t* host = nullptr;
    const int result = getRwlock(lock, &host);
    return result == 0 ? ::pthread_rwlock_unlock(host) : result;
}

int guestPthreadOnce(void* onceControl, void (*initializer)()) {
    CENSUS_IMPORT("pthread_once");
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
    CENSUS_IMPORT("pthread_getspecific");
    return ::pthread_getspecific(static_cast<pthread_key_t>(key));
}

int guestPthreadSetSpecific(GuestKey key, const void* value) {
    CENSUS_IMPORT("pthread_setspecific");
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
