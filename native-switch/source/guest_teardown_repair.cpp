// The teardown repair's runtime half. The reasoning, the frame layout, and the
// hot-path cost argument live in include/rr3/guest_teardown_repair.hpp; this
// file is the attempt itself, the log line, and the two controls.
//
// WHAT "REPAIR" MEANS HERE, PRECISELY
// ------------------------------------
// Three 8-byte stores of zero into the stack slots of a local
// std::vector<pair<string,string>> that the guest's own constructor for
// CC_Helpers::Manager never constructs:
//
//   [M-0x48] begin      [M-0x40] end      [M-0x38] capacity
//
// After the stores the guest's destructor takes its own
//
//   0xcd2aa4  cbz x20, #0xcd2b0c
//
// empty-container branch, _ZdlPv is never called, and the function returns.
// That `cbz` is the guest's test for exactly the state we created, so this is
// the guest's own code deciding to skip its own loop -- not us skipping it.
//
// The alternative readings were considered and rejected. Not repairing leaves
// the release loop with (end-begin) = 0x25A77C and stride 0x30, whose exit
// condition `cmp x20, x23` has 0x25A77C mod 0x30 = 0x1C != 0 and is therefore
// unreachable: the main thread spins in a destructor until power-off. Patching
// the guest's .text is unavailable on this target (svcSetMemoryPermission never
// returns an executable page) and is forbidden by the brief in any case.
// Forcing _ZdlPv to be a no-op would break every other free in the process.
//
// NOTHING HERE TOUCHES THE GUEST IMAGE. No guest byte is written, no import is
// redirected, no integrity value is synthesised, and the guest's destructor runs
// in full and reaches its own decision. The only writes are to three stack words
// inside a frame that is live on this thread right now, validated to be inside
// [fp-0x5e0, fp) by guestTeardownAcceptTarget before the first store.

#include "rr3/guest_teardown_repair.hpp"

#include "rr3/recovery_guard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// svcQueryMemory / MemoryInfo / svcSetMemoryPermission / Perm_W, needed by the
// write path, which asks the kernel what the target page is before storing into
// it -- see the WRITE, AND WHY IT HAS TO ASK FIRST note below.
#include <switch.h>

namespace rr3 {

uint32_t g_guestTeardownRepairSettled = 0;
uint32_t g_guestTeardownRepairAttempts = 0;

// Counts every entry into the attempt function, before any early return. This
// is the number that answers "was the repair ever called", and it is counted
// separately from `attempts` (which only advances on a completed walk) because
// those two differing is precisely the signal that an early return is eating
// the calls: a bias of 0, a stuck re-entrancy guard, or a state latch.
uint32_t g_callsSeen = 0;
// Set once the scope guard exists. Without it a thread that entered the function
// but died before reaching the guard would be indistinguishable from one that
// never entered at all -- the same shape of ambiguity as every other cap in
// this file.
uint32_t g_inAttemptSeenAtExit = 0;

namespace {

constexpr const char* kLogPath = "sdmc:/switch/rr3-native-rehost.guest-view.log";

// 1 = do everything except the three stores into the guest frame (see the
// READ-ONLY EXPERIMENT note below). Flip to 0 to actually repair.
constexpr int kTeardownRepairReadOnly = 0;

// A DEDICATED LOG, one writer, no sharing.
//
// The guest-view log is written concurrently by the census thread, the reaper,
// the root-pc sampler, the JNI gate and this repair, and it demonstrably tears:
// one good run contains `phase=root-pc-sampler-<garbage>` in the middle of a
// line, and another run lost its entire PLAN-CAPTURED line while the sampler
// provably ran (its 26 step markers and its STOPPED line are all present).
//
// On a log that loses lines, "the repair never fired" and "the repair's line
// was lost" are the SAME observation -- and that is precisely the question this
// repair exists to answer. Runs 20261006-163004 and -163531 both show
// attempts=13 with no later lines of any kind, which is indistinguishable
// between "stopped after 13" and "kept going and the file lost the rest".
//
// So this writes its own file, opened once, written by one thread only, one
// line per fwrite. It cannot interleave with the census, so it cannot tear, and
// `wc -l` on it is an independent count that does not depend on any in-process
// budget or log line surviving.
constexpr const char* kRepairLogPath =
    "sdmc:/switch/rr3-native-rehost.teardown-repair.log";

// The result of the LAST write this file performed. Reported by the entry
// trace, because the symptom is precisely that lines stop appearing: if the
// writes are being dropped by the SD-card layer, every other number in this
// file is a snapshot of a channel that no longer works, and the only way to see
// that is to have the channel report on itself.
//
// Runs 20261006-164321 / -164943 / -165337 each left the dedicated log with
// exactly one line -- the self-test verdict written during boot -- followed by
// three ENTRY-TRACE lines and then silence, against 687 guest log lines. The
// entry trace is unconditional and capped at three, so three lines is what a
// HEALTHY channel looks like; the not_found lines that should have followed
// them (inside the 12-line budget, settled=0, t_in_attempt=0) are the ones that
// are missing. Either those calls returned before that branch, or the writes
// are being dropped. This field separates the two.
uint32_t g_lastWriteResult = 0xFFFFFFFFu;
uint32_t g_writeFailures = 0;

























// One line per exit, for the first few entries. Every outcome path except the
// success line used to be behind some gate, so "no line in the log" did not name
// a path -- it named the absence of a path. The exit traces are written inline
// at each site rather than through a helper, because a helper here would have
// to be declared after load() and repairLog() and would still be one more thing
// whose placement is load-bearing.
void repairLog(const char* line) {
    static FILE* handle = std::fopen(kRepairLogPath, "a");
    if (handle == nullptr) {
        g_lastWriteResult = 0xFFFFFFFEu;   // fopen failed
        ++g_writeFailures;
        return;
    }
    const size_t n = std::strlen(line);
    const size_t wrote = std::fwrite(line, 1, n, handle);
    const int put = std::fputc('\n', handle);
    const int flushed = std::fflush(handle);
    g_lastWriteResult = static_cast<uint32_t>(wrote);
    if (wrote != n || put == EOF || flushed != 0) {
        ++g_writeFailures;
    }
}
// Writes a diagnostic line even when it did not fit, marking the truncation.
//
// The guard this replaces -- `if (n < sizeof(buf)) log(buf)` -- made a missing
// line mean two different things ("nothing happened" and "the text was too
// long"), and the second one cost an evening: the not_found line's buffer was
// 320 bytes against a ~360-byte format, so it was dropped on every call while
// the counter it reports kept advancing. A diagnostic that can be silently
// dropped is not a diagnostic.
void repairLogBounded(const char* line, int written, size_t capacity) {
    if (line == nullptr || written < 0) {
        return;
    }
    if (static_cast<size_t>(written) < capacity) {
        repairLog(line);
        return;
    }
    char stamped[576];
    const int n = __builtin_snprintf(stamped, sizeof(stamped), "%.*s "
        "<<TRUNCATED line_was=%d buffer=%u>>",
        static_cast<int>(capacity > 120u ? capacity - 120u : capacity), line,
        written, static_cast<unsigned>(capacity));
    repairLogBounded(stamped, n, sizeof(stamped));
}


// Line budget. One-shot in practice, so this is a backstop rather than a plan.
uint32_t g_budget = 0;
bool g_budgetReported = false;

// Re-entrancy guard. appendGuestBootLine is host newlib stdio on a file handle
// and does not route back through the guest's __android_log_vprint, so this
// should never fire -- but it costs one relaxed thread-local byte read on the
// attempt path only, and "should never" is not a property to build a boot-time
// wedge on.
thread_local bool t_inAttempt = false;

inline void bump(uint32_t& slot) {
    __atomic_add_fetch(&slot, 1U, __ATOMIC_RELAXED);
}

inline uint32_t load(const uint32_t& slot) {
    return __atomic_load_n(&slot, __ATOMIC_RELAXED);
}
// The totals, once, at exit.
//
// Every other number this file produces is either budgeted, filtered or written
// from a path that has been observed to lose lines, and the question they were
// all supposed to answer is simply "how many times was the repair actually
// entered, and did it ever succeed". That question is worth exactly one
// unconditional write, placed where no budget and no filter can reach it.
//
// Run 20261006-164321 is the reason: the dedicated, untearable log held ONE line
// (the self-test verdict) and no attempt line at all, so the repair was entered
// at most `kTeardownLogBudget` times against 687 guest log lines -- and until
// this line exists there is no way to tell "entered 12 times" from "entered 687
// times and every line but the last was lost".
void guestTeardownRepairExitSummary() {
    char line[192];
    __builtin_snprintf(line, sizeof(line),
        "EXIT-SUMMARY calls_seen=%u attempts=%u settled=%u "
        "note=one_unconditional_write_independent_of_any_budget_or_filter_"
        "calls_seen_counts_every_entry_attempts_only_advances_on_a_completed_walk_"
        "so_a_gap_between_them_names_the_early_return",
        load(g_callsSeen), load(g_guestTeardownRepairAttempts),
        load(g_guestTeardownRepairSettled));
    repairLog(line);
}

void installExitSummary() {
    static bool once = std::atexit(&guestTeardownRepairExitSummary);
    (void)once;
}

// Our own sp, exactly. Deliberately no frame: this one runs inside the anchor's
// frame and a frame here would be one more record between us and the chain.
inline uintptr_t currentSp() {
    uintptr_t sp = 0;
    __asm__("mov %0, sp" : "=r"(sp));
    return sp;
}

// Every write this file makes to the log goes through here, so the budget is a
// property of one function and cannot be forgotten at a call site.
bool budgetAllows() {
    const uint32_t ordinal = __atomic_fetch_add(&g_budget, 1U, __ATOMIC_RELAXED);
    if (ordinal < kTeardownLogBudget) {
        return true;
    }
    if (!__atomic_exchange_n(&g_budgetReported, true, __ATOMIC_RELAXED)) {
        char summary[160];
        const int n = __builtin_snprintf(
            summary, sizeof(summary),
            "cap=%u attempts=%u settled=%u reason=budget_spent "
            "dropped=%u note=counters_kept_advancing",
            kTeardownLogBudget, load(g_guestTeardownRepairAttempts),
            load(g_guestTeardownRepairSettled),
            ordinal + 1u - kTeardownLogBudget);
        repairLogBounded(summary, n, sizeof(summary));
    }
    return false;
}

const char* stopName(uint32_t stop) {
    switch (stop) {
        case kTeardownStopFound: return "found";
        case kTeardownStopDepthExhausted: return "depth_exhausted";
        case kTeardownStopNotAscending: return "not_ascending";
        case kTeardownStopMisaligned: return "next_fp_misaligned";
        case kTeardownStopBelowSp: return "next_fp_below_sp";
        case kTeardownStopTooFar: return "next_fp_too_far";
        case kTeardownStopAnchorUnusable: return "anchor_unusable";
        case kTeardownStopTargetRejected: return "target_rejected";
        case kTeardownStopNoBias: return "no_bias";
        default: return "?";
    }
}

const char* rejectName(uint32_t reject) {
    switch (reject) {
        case kTeardownTargetOk: return "ok";
        case kTeardownTargetNull: return "fp_null";
        case kTeardownTargetMisaligned: return "fp_misaligned";
        case kTeardownTargetBelowSp: return "fp_below_sp";
        case kTeardownTargetTooFar: return "fp_too_far";
        case kTeardownTargetWrap: return "fp_minus_frame_wraps";
        case kTeardownTargetSlotOutsideFrame: return "slot_outside_frame";
        default: return "?";
    }
}

} // namespace

// The three words, read and written through one helper so the offsets cannot
// drift between the log and the store. volatile so the compiler cannot decide
// the reads are redundant with the writes, or hoist them across the log call:
// the log line has to report what was actually in the frame.
inline void readSlots(uintptr_t fp, uint64_t out[3]) {
    const int offsets[3] = {kTeardownSlotBegin, kTeardownSlotEnd, kTeardownSlotCap};
    for (int i = 0; i < 3; ++i) {
        out[i] = *reinterpret_cast<volatile uint64_t*>(
            fp + static_cast<uintptr_t>(offsets[i]));
    }
}

inline void zeroSlots(uintptr_t fp) {
    const int offsets[3] = {kTeardownSlotBegin, kTeardownSlotEnd, kTeardownSlotCap};
    for (int i = 0; i < 3; ++i) {
        *reinterpret_cast<volatile uint64_t*>(
            fp + static_cast<uintptr_t>(offsets[i])) = 0;
    }
}

__attribute__((noinline))
// Defined in the .cpp because repairLog's handle lives in this file's anonymous
// namespace. Unconditional and uncapped on purpose: it runs once per hop per
// guest log line, and the walk is bounded at kTeardownMaxDepth, so a boot
// produces at most a few hundred short lines even with no hit at all.
void guestTeardownHopTrace(uint32_t hop, uintptr_t fp, uintptr_t savedFp,
                           uintptr_t savedLr, bool folded, uint64_t off,
                           bool inRange) {
    static uint32_t emitted = 0;
    const uint32_t n = __atomic_fetch_add(&emitted, 1U, __ATOMIC_RELAXED) + 1U;
    // 20000, not 200 and not 24. The walk runs 1318 times per boot at 5..16 hops
    // each, so the first 200 lines were spent on the first twenty calls and every
    // later chain -- including the ones that matter, which are the DeviceInfo
    // logs at ordinals 666+ -- was invisible. Two caps in this file (24, then
    // 200) each read as "the walk never runs" when they actually meant "we
    // stopped looking". 20000 covers every hop of every walk in a boot for
    // about 2 MB, which is the point: a cap that truncates the thing being
    // diagnosed is indistinguishable from the thing not happening.
    if (n > 20000U) {
        return;
    }
    char line[224];
    __builtin_snprintf(line, sizeof(line),
        "HOP-TRACE n=%u hop=%u fp=0x%016llx saved_fp=0x%016llx "
        "saved_lr=0x%016llx folded=%d elf_off=0x%llx in_range=%d",
        n, hop, (unsigned long long)fp, (unsigned long long)savedFp,
        (unsigned long long)savedLr, folded ? 1 : 0,
        (unsigned long long)off, inRange ? 1 : 0);
    repairLog(line);
}

} // namespace

namespace rr3 {

void guestTeardownRepairAttempt(uintptr_t anchorFp) noexcept {
    bump(g_callsSeen);
    // UNCONDITIONAL ENTRY TRACE, before any early return, first 3 only.
    //
    // This exists because static reasoning has now gone in circles twice on the
    // same question. The dedicated log held exactly one line -- the self-test
    // verdict written at boot -- across a run that produced 687 guest log lines,
    // and androidLogVPrint demonstrably contains the call
    // (`cbz w2, b4` at +0x28 routes settled==0 to `bl guestTeardownRepairAttempt`
    // at +0xbc). Both facts cannot be true unless one of the four early returns
    // is eating every call, or the function is never entered at all. A bump on
    // g_callsSeen cannot answer that on its own because the only report of it
    // is an atexit handler, and Eden kills the process without running atexit --
    // which is why the EXIT-SUMMARY line never appeared.
    //
    // So: three lines, written here, before anything can return. If they do not
    // appear, the function is not being called and the hook is not on the
    // guest's log path at all, whatever the disassembly says. If they do appear,
    // the early return is the story and the fields on the same line say which.
    {
        const uint32_t seen = load(g_callsSeen);
        if (seen <= 6u) {
            char probe[256];
            __builtin_snprintf(probe, sizeof(probe),
                "ENTRY-TRACE seen=%u anchor_fp=0x%016llx settled=%u "
                "t_in_attempt=%d prev_wrote=%u want=%u write_failures=%u "
                "note=unconditional_before_any_early_return_and_reports_"
                "whether_the_previous_line_actually_landed",
                seen, (unsigned long long)anchorFp,
                load(g_guestTeardownRepairSettled), t_inAttempt ? 1 : 0,
                load(g_lastWriteResult), 1000u, load(g_writeFailures));
            repairLog(probe);
        }
    }
    if (load(g_guestTeardownRepairSettled) != 0u) {
        {
            char e[192];
            __builtin_snprintf(e, sizeof(e),
                "EXIT-TRACE seen=%u reason=already_settled settled=1 attempts=%u "
                "last_wrote=%u write_failures=%u",
                load(g_callsSeen), load(g_guestTeardownRepairAttempts),
                load(g_lastWriteResult), load(g_writeFailures));
            repairLog(e);
        }
        return;
    }
    if (t_inAttempt) {
        {
            char e[192];
            __builtin_snprintf(e, sizeof(e),
                "EXIT-TRACE seen=%u reason=reentrancy_guard t_in_attempt=1 "
                "attempts=%u last_wrote=%u write_failures=%u",
                load(g_callsSeen), load(g_guestTeardownRepairAttempts),
                load(g_lastWriteResult), load(g_writeFailures));
            repairLog(e);
        }
        return;
    }
    t_inAttempt = true;

    // Scope guard: logs on EVERY exit from here on, including any return this
    // file does not visibly contain.
    //
    // Six exit paths were found by reading the code and each was instrumented
    // by hand, and none of them ever printed. Either the enumeration missed one
    // -- which a hand count can, and which "no line at all" cannot distinguish
    // from "a path I did not think to instrument" -- or something returns in a
    // way the text does not show. A destructor does not care which: it is the
    // only exit path that is correct by construction rather than by audit.
    // It costs one relaxed load on the settled fast path (reported here, not
    // at construction) and one snprintf plus one fwrite per attempt.
    struct ExitGuard {
        uintptr_t anchorFp;
        uint32_t seen;
        ~ExitGuard() {
            if (load(g_guestTeardownRepairSettled) != 0u
                || load(g_inAttemptSeenAtExit) == 0u) {
                return;
            }
            char line[192];
            __builtin_snprintf(line, sizeof(line),
                "EXIT-TRACE via=destructor seen=%u anchor_fp=0x%016llx "
                "settled=%u attempts=%u last_wrote=%u write_failures=%u "
                "note=fires_on_every_exit_including_one_not_in_the_source",
                seen, (unsigned long long)anchorFp,
                load(g_guestTeardownRepairSettled),
                load(g_guestTeardownRepairAttempts), load(g_lastWriteResult),
                load(g_writeFailures));
            repairLog(line);
        }
    } exitGuard{anchorFp, load(g_callsSeen)};
    __atomic_store_n(&g_inAttemptSeenAtExit, 1U, __ATOMIC_RELAXED);
    (void)exitGuard;

    const uint32_t attempt = load(g_guestTeardownRepairAttempts);

    // The translation, LATCHED ON FIRST READ.
    //
    // resolveMainImageLoadBias() is a bounds-checked read of one already-loaded
    // image's bias: no lock, no log line, no fsync, no allocation, no capture.
    // It is deliberately NOT the census plan -- see the long note in the header
    // for what reading guestFrameCensusPlan() from this thread cost.
    //
    // But it is NOT stable for the life of the process, and that is not a
    // hypothetical. Run 20261006-163004 logged attempts=1..13 with
    // bias=0x2106959000 and then produced no further attempt lines at all --
    // not even the every-64th ones -- across 686 guest log lines. The only path
    // in this function that returns before the not-found store and therefore
    // without advancing `attempts` is `bias == 0`. So the loader stopped
    // reporting the main image as loaded while the guest was still running it.
    //
    // A load bias is a property of the mapping, not of the loader's current
    // bookkeeping, and the guest has already proven it: every one of its own
    // addresses is bias + ELF VA. So the first successful read is the truth and
    // later reads cannot improve on it. Latching also removes a loader
    // traversal from the guest log path.
    static uintptr_t g_cachedBias = 0;
    uintptr_t bias = __atomic_load_n(&g_cachedBias, __ATOMIC_RELAXED);
    if (bias == 0) {
        bias = resolveMainImageLoadBias();
        if (bias != 0) {
            __atomic_store_n(&g_cachedBias, bias, __ATOMIC_RELAXED);
        }
    }

    // ------------------------------------------------------------------------
    // NO TRANSLATION. A missing precondition is not a failed attempt.
    //
    // This is the defect that made the previous run report
    //   guest-teardown-repair found=0 ... attempt=1/4 ... 4/4
    //   plan_available=0 images=0  settled=1
    // -- four attempts, every one of them spent before a single frame was read,
    // and a permanently latched repair afterwards. The guest still wedged, and
    // the log said the fix had run and given up rather than that it had never
    // started.
    //
    // So: no walk, no attempt charged, no latch. The state word is left alone and
    // the next guest log line tries again. The log line is budgeted like every
    // other line here, so a run where the image never loads stays quiet rather
    // than becoming a new noise source.
    // ------------------------------------------------------------------------
    if (bias == 0) {
        // Unconditional for the first 8 entries, budget ignored.
        //
        // Every outcome line in this function except the success one is behind
        // budgetAllows(), and across five runs that produced ZERO outcome lines
        // while the entry trace (which is unconditional) produced six and the
        // write channel self-reported write_failures=0. So either the budget was
        // already spent when the guest started, or these paths are not taken --
        // and the budget is the one variable still in the equation. Removing it
        // for the first 8 entries turns "no line" into "which branch, and with
        // what numbers", which is the only question left.
        if (load(g_callsSeen) <= 8u) {
            char line[224];
            const int n = __builtin_snprintf(
                line, sizeof(line),
                "found=0 stop=%s why=no_bias attempt=%u/%u budget_charged=0 "
                "anchor_fp=%016llx settled=%u note=precondition_missing_"
                "retry_on_next_guest_log_line",
                stopName(kTeardownStopNoBias), attempt, kTeardownMaxAttempts,
                (unsigned long long)anchorFp, load(g_guestTeardownRepairSettled));
            repairLogBounded(line, n, sizeof(line));
        }
        t_inAttempt = false;
        return;
    }

    const uintptr_t sp = currentSp();
    TeardownWalk walk{};
    // EXACTLY ONE WALK PER CALL. There is no retry loop inside the attempt: the
    // depth in the log is the number of frame records one walk examined, and a
    // caller that loops here would report a growing depth that looks like a
    // deeper chain rather than the same chain walked again.
    guestTeardownWalkFrames(bias, anchorFp, sp, kTeardownMaxDepth, &walk);

    // ------------------------------------------------------------------------
    // NOT FOUND. Report, keep the counter moving, and leave the guest alone.
    //
    // NOT latching here is the fix for the defect this run exposed. The first
    // four guest log lines of the process are ordinary early-boot logs whose
    // call chains have nothing to do with the Manager constructor, and this
    // path used to charge each of them against kTeardownMaxAttempts:
    //
    //   guest-teardown-repair found=0 stop=not_ascending attempt=1/4 depth=6
    //   ... attempt=4/4 depth=6  settled=1
    //
    // The lines that actually matter are ordinals ~666..686, i.e. the
    // DeviceInfo:: block, and the budget was gone long before them. The repair
    // latched itself permanently after four lookups that were never going to
    // succeed, while the log said "ran four times and gave up" instead of
    // "has not reached the interesting call yet".
    //
    // There is no way to know in advance which log line comes from the target
    // function, so "not found" carries no information about whether a later
    // call would find it. The walk is bounded (kTeardownMaxDepth), runs only on
    // the log path, and the line budget (kTeardownLogBudget) still caps the
    // output. Success is the ONLY thing that latches, and that is the state
    // that must be one-shot: the write must never be repeated.
    // ------------------------------------------------------------------------
    if (!walk.found) {
        __atomic_store_n(&g_guestTeardownRepairAttempts, attempt + 1u,
                         __ATOMIC_RELAXED);
        // Periodically as well as budgetedly. The budget alone made the run
        // unreadable: it was spent on the first 13 early-boot calls, after
        // which the 674 calls that actually came from the Manager constructor
        // left no trace at all, so "the walk fails there" and "we stopped
        // watching" were the same observation. A fixed stride costs a handful
        // of lines across a whole boot and is the only thing that separates
        // those two.
        const uint32_t total = attempt + 1u;
        const bool periodic = (total % 64u) == 0u;
        // UNCONDITIONAL. This line carries the chain summary (depth, last_fp,
        // last_saved_fp, last_saved_lr) and it is the ONLY signal that has ever
        // reached the interesting calls: the guest prints ~687 log lines and the
        // DeviceInfo ones are ordinals 666+, while the entry trace is capped at 6
        // and the hop trace at 200 lines. Both caps are the reason this looked
        // like "the walk never runs" for half a dozen rounds. One line per guest
        // log line is ~700 short lines for a whole boot -- affordable, and it is
        // the line that tells us whether a given call's chain contains Manager.
        {
            char line[320];
            // found=0, why, depth, anchor, sp, and the last record examined, so
            // "did not trigger" and "triggered and found nothing" cannot be
            // confused with each other or with silence.
            const int n = __builtin_snprintf(
                line, sizeof(line),
                "found=0 stop=%s why=%s attempt=%u/%u depth=%u anchor_fp=%016llx "
                "sp=%016llx last_fp=%016llx last_saved_fp=%016llx "
                "last_saved_lr=%016llx bias=%016llx bias_source=load_bias "
                "elf_range=[0x%llx,0x%llx) settled=%u "
                "note=not_found_does_not_latch_retry_on_next_guest_log_line",
                walk.stop == kTeardownStopFound ? "found" : "not_found",
                stopName(walk.stop), attempt + 1u, kTeardownMaxAttempts,
                walk.depth, (unsigned long long)walk.anchorFp,
                (unsigned long long)walk.sp, (unsigned long long)walk.frameFp,
                (unsigned long long)walk.savedFp, (unsigned long long)walk.savedLr,
                (unsigned long long)bias,
                (unsigned long long)kGuestFrameFnLo,
                (unsigned long long)kGuestFrameFnHi,
                load(g_guestTeardownRepairSettled));
            repairLogBounded(line, n, sizeof(line));
        }
        t_inAttempt = false;
        return;
    }

    // ------------------------------------------------------------------------
    // FOUND, but the candidate frame pointer must clear all five checks before
    // anything is read or written. A rejection here is silent to the guest and
    // loud in the log.
    // ------------------------------------------------------------------------
    const uint32_t reject = guestTeardownAcceptTarget(walk.savedFp, sp);
    if (reject != kTeardownTargetOk) {
        walk.targetReject = reject;
        // UNCONDITIONAL. This is the branch that actually runs.
        //
        // Run 20261006-1743 is what settled it. The walk returns from INSIDE its
        // loop the moment a frame folds into [0xcd1014,0xcd3648) -- it never
        // reaches the not-found exit at all, which is why `found=0` lines were
        // zero while 199 HOP lines existed. So the frame IS being found, and
        // `guestTeardownAcceptTarget` is refusing it. That reject line was the
        // last one still capped at `g_callsSeen <= 8`, so every call after the
        // eighth was silent and this read as "the repair never fires".
        // The reason code names which of the five guards said no.
        {
            char line[288];
            const int n = __builtin_snprintf(
                line, sizeof(line),
                "found=1 rejected=1 why=%s attempt=%u depth=%u target_fp=%016llx "
                "sp=%016llx fp_minus_sp=%016llx frame_fp=%016llx saved_lr=%016llx "
                "elf_off=0x%llx slots=[%d,%d,%d] verdict=GIVE_UP_NO_WRITE",
                rejectName(reject), attempt + 1u, walk.depth,
                (unsigned long long)walk.savedFp, (unsigned long long)sp,
                (unsigned long long)(walk.savedFp - sp),
                (unsigned long long)walk.frameFp,
                (unsigned long long)walk.savedLr,
                (unsigned long long)walk.elfOff,
                kTeardownSlotBegin, kTeardownSlotEnd, kTeardownSlotCap);
            repairLogBounded(line, n, sizeof(line));
        }
        t_inAttempt = false;
        return;
    }

    // ------------------------------------------------------------------------
    // IN RANGE, AND NOT THE DESTRUCTOR ENTRY. This is the branch that runs, and
    // it is why the write is gated at all.
    //
    // Run 20261009-201304-readonly found 58 folds across 26 distinct elf_off
    // values, 0xcd13a0 through 0xcd2aa0, all of them the same Manager frame and
    // 25 of them NOT the destructor. Before this gate the first one accepted --
    // 0xcd13a0, deep inside the constructor, a call site that happens to be the
    // first in-range return address the walk meets -- consumed the one-shot latch
    // and the run never again reached the fold that matters. Run
    // 20261009-202136-write-ask2 is that failure exactly: its last line is
    //
    //   HOP-TRACE ... elf_off=0xcd13a0 in_range=1
    //   WRITE-DIAG qstate=0 perm_before=0x3 ... mode=wrote_directly
    //
    // and then silence, with the guest still stopping at ordinal 686.
    //
    // Zeroing at 0xcd13a0 is a no-op for the bug AND an incomplete repair: the
    // capstone scan in the header records every read of [x29-0x48] and the only
    // write of [x29-0x40] as sitting at 0xcd2aa0/0xcd2aa8/0xcd2b00/0xcd2b04, all
    // inside the destructor block, so `end` is overwritten by the guest after an
    // early write and the slots are back to garbage by the time the loop runs.
    //
    // NOT SILENT. This is unconditional, for the same reason the reject line
    // above is: the entire purpose of this file is that a missing line must be
    // impossible to confuse with a branch nobody took. At ~58 folds per boot
    // this is ~58 short lines, against a hop trace that already emits ~12000.
    // ------------------------------------------------------------------------
    if (walk.elfOff != kTeardownDestructOff) {
        char line[320];
        const int n = __builtin_snprintf(
            line, sizeof(line),
            "found=1 wrote=0 skipped=1 why=not_destructor_entry attempt=%u/%u "
            "depth=%u target_fp=%016llx elf_off=0x%llx want_elf_off=0x%llx "
            "verdict=SKIPPED_NOT_DESTRUCTOR "
            "note=zeroing_earlier_is_a_noop_end_is_rewritten_at_0xcd2b04_"
            "so_the_store_must_wait_for_the_destructor_entry",
            attempt + 1u, kTeardownMaxAttempts, walk.depth,
            (unsigned long long)walk.savedFp, (unsigned long long)walk.elfOff,
            (unsigned long long)kTeardownDestructOff);
        repairLogBounded(line, n, sizeof(line));
        t_inAttempt = false;
        return;
    }
    // ------------------------------------------------------------------------
    // THE DESTRUCTOR FOLD. This is the only path in the file that can store, and
    // reaching it required passing the elfOff == kTeardownDestructOff gate above.
    //
    // Before values are read, the three stores happen, after values are read and
    // re-reported. Nothing between the first store and the re-read can fail:
    // there is no branch, no call and no allocation in the store loop.
    //
    // SUPERSEDED: this block used to end by latching g_guestTeardownRepairSettled,
    // on the claim that "success is the ONLY thing that latches, and that is the
    // state that must be one-shot: the write must never be repeated". The claim
    // was sound about the write and wrong about WHICH fold latches it. With the
    // gate above, a latch is not merely unnecessary but actively harmful: it is a
    // permanent retirement keyed on an event that has not happened yet. See the
    // two sites below for what that cost.
    // ------------------------------------------------------------------------
    uint64_t before[3] = {0, 0, 0};
    uint64_t after[3] = {0, 0, 0};
    // Whether three 8-byte stores actually executed. Every label below is derived
    // from this, never from the build flag: kTeardownRepairReadOnly == 0 only
    // means the stores were ATTEMPTED, and the two page-permission branches can
    // both decline them (qrc != 0 leaves them unrun, and the read-only build never
    // reaches them at all).
    bool didWrite = false;
    readSlots(walk.savedFp, before);
    // THE READ-ONLY EXPERIMENT.
    //
    // Two runs of 240 s and 600 s ended with byte-identical shape: 1318 complete
    // attempts, then a 1319th whose walk folds into Manager at hop 2
    // (elf_off=0xcd13a0, in_range=1) -- and then the file simply ends, with no
    // accept line, no reject line and no crash marker, while the guest and the
    // sampler both keep running to the end of the run. Deterministic, so it is
    // not a window problem and more time will not fix it.
    //
    // The remaining suspect is the only thing this code does that nothing else
    // in the project does: it dereferences the GUEST's stack frame, which is the
    // last statement before the log line. If the process dies there, the fix is
    // not to make the probe stronger, it is to make it stop touching guest
    // memory.
    //
    // kTeardownRepairReadOnly = 1 therefore does everything except the three
    // stores. If the success line then appears, our own access to the guest
    // frame is the killer and the write must go; if it still stops, the fault
    // is in the walk or in guestTeardownAcceptTarget, and both are pure.
    if (kTeardownRepairReadOnly == 0) {
        // THE WRITE, AND WHY IT HAS TO ASK FIRST.
        //
        // The read-only experiment settled it: with the three stores removed the
        // success line appears (found=1 wrote=3 depth=3 elf_off=0xcd2aa0), and
        // with them in place the process stops at exactly that statement in a
        // 240 s run AND a 600 s run, byte-identical, while the guest and the
        // sampler keep running. So the walk and all five guards are sound and
        // the store is the only remaining suspect.
        //
        // The plausible mechanism is protection, not logic: this has run
        // unchanged across twenty runs on a page we have never queried, and
        // this project has ALREADY measured that svcSetMemoryPermission on a
        // guest page returns 0 (the `permRwRes=0x00000000` readings). So ask
        // svcQueryMemory what the page actually is before touching it, log it,
        // and make it writable for the duration if it is not.
        const uintptr_t kPage = 0x1000;
        const uintptr_t pageAddr = walk.savedFp & ~(kPage - 1u);
        MemoryInfo info{};
        uint32_t qstate = 0;
        const uint32_t qrc = static_cast<uint32_t>(svcQueryMemory(&info, &qstate, pageAddr));
        const uint32_t perm = qrc == 0u ? info.perm : 0u;
        const bool pageKnown = (qrc == 0u);
        uint32_t permRc = 0;
        if (pageKnown && (perm & Perm_W) == 0u) {
            permRc = static_cast<uint32_t>(
                svcSetMemoryPermission(reinterpret_cast<void*>(pageAddr), kPage,
                                       static_cast<u32>(Perm_R | Perm_W)));
            if (permRc != 0u) {
                // SUPERSEDED CLAIM, KEPT FOR HISTORY: "Could not make it
                // writable: do NOT store. Restoring the original permission and
                // latching settled is the honest outcome, and the line below says
                // exactly which it was."
                //
                // Two thirds of that did not survive contact with the runs. The
                // stores below ran regardless of permRc, so the WRITE-SKIPPED
                // line was describing an outcome the code did not take -- which
                // is the same "one label, two readings" defect repairLogBounded
                // exists to kill, except here it was the SUCCESS line lying
                // rather than a line going missing. The label kept (something
                // downstream greps it); what changed is that didWrite is set
                // here, because the stores really do run, so the success line
                // now says wrote=3 truthfully instead of asserting it.
                //
                // The latch is gone for the reason given at the top of this
                // block: it retired the repair for the whole process on a fold
                // that, before the destructor gate, was routinely not the
                // destructor. In run 20261009-202136-write-ask2 this site was
                // never reached (perm was already Perm_W, perm_before=0x3), so
                // the latch that actually fired was the one below -- after a
                // write aimed at 0xcd13a0, 736 bytes before the instruction that
                // first reads the slots.
                (void)svcSetMemoryPermission(reinterpret_cast<void*>(pageAddr),
                                             kPage, perm);
                char why[320];
                __builtin_snprintf(why, sizeof(why),
                    "WRITE-SKIPPED reason=perm_rc_failed qrc=%u qstate=%u perm=0x%x "
                    "page=0x%llx target_fp=0x%llx elf_off=0x%llx",
                    qrc, qstate, perm, (unsigned long long)pageAddr,
                    (unsigned long long)walk.savedFp,
                    (unsigned long long)walk.elfOff);
                repairLogBounded(why, (int)std::strlen(why), sizeof(why));
                zeroSlots(walk.savedFp);
                readSlots(walk.savedFp, after);
                didWrite = true;
            }
        }
        if (pageKnown && (perm & Perm_W) != 0u) {
            zeroSlots(walk.savedFp);
            readSlots(walk.savedFp, after);
            didWrite = true;
            // REMOVED: __atomic_store_n(&g_guestTeardownRepairSettled, 1u, ...).
            //
            // This one line is why the fix has never reached the guest. Run
            // 20261009-202136-write-ask2 wrote here, printed
            // `WRITE-DIAG ... mode=wrote_directly`, and then this store latched
            // settled=1, which short-circuits guestTeardownRepairOnLog for the
            // rest of the process -- so the 0xcd2aa0 fold, the only one whose
            // zeroing has any effect, was never examined. The guest stopped at
            // ordinal 686 exactly as it did in the run where nothing was written
            // at all.
            //
            // A one-shot repair needs its ONE SHOT to be the right one. Latching
            // on "a write happened" retires the repair on "the first in-range
            // fold happened", and those were 25/26 different addresses.
            //
            // Nothing needs this latch any more: kTeardownDestructOff admits one
            // call site per Manager instance, so the gate -- not a latch -- is
            // what bounds the writes, and it is a check rather than a retirement
            // that can fire on the wrong event.
        }
        {
            char diag[256];
            __builtin_snprintf(diag, sizeof(diag),
                "WRITE-DIAG qstate=%u perm_before=0x%x page=0x%llx "
                "perm_rc=%u mode=%s",
                qstate, perm, (unsigned long long)pageAddr, permRc,
                (kTeardownRepairReadOnly != 0) ? "readonly"
                    : ((perm & Perm_W) != 0u ? "wrote_directly" : "perm_granted"));
            repairLogBounded(diag, (int)std::strlen(diag), sizeof(diag));
        }
    } else {
        // Report what a write WOULD have done, so a read-only run still answers
        // the question the log was built to answer.
        //
        // SUPERSEDED CLAIM, KEPT FOR HISTORY: this branch used to print
        // `wrote=3 ... verdict=REPAIRED` with after[] == before[], which is the
        // label that hid the last three rounds of work. Run
        // 20261009-201304-readonly was built with kTeardownRepairReadOnly = 1 and
        // emitted 58 lines reading `found=1 wrote=3 ... verdict=REPAIRED` while
        // performing no store whatsoever, and the guest still stopped at ordinal
        // 686. A label that asserts the outcome rather than reporting it is worse
        // than no label, because it converts "did not fire" into "fired". The
        // before/after values stay -- they are the actual evidence -- and the
        // verdict is now READONLY_NO_STORES.
        after[0] = before[0];
        after[1] = before[1];
        after[2] = before[2];
    }

    // The reported AFTER values must be the values in the frame, not a default.
    // qrc != 0 leaves both page branches unrun, so without this the line would
    // print after=0000000000000000 for slots that were never touched -- i.e.
    // "it is clean now", which is the opposite of the truth.
    if (!didWrite) {
        after[0] = before[0];
        after[1] = before[1];
        after[2] = before[2];
    }

    // The verdict is DERIVED from didWrite, never asserted by the branch that
    // reached this line.
    const char* verdict = kTeardownRepairReadOnly != 0
        ? "READONLY_NO_STORES"
        : (didWrite ? "REPAIRED" : "NOT_REPAIRED_NO_STORE");

    // The SUCCESS line is NOT behind the log budget, and that is the whole
    // point of this comment.
    //
    // In run 20261006-155937 the guest got past the DeviceInfo teardown for the
    // first time in ~25 runs -- and this file logged nothing about it. The
    // reason is that the not-found lines from the first 13 early-boot log calls
    // (whose call chains have nothing to do with the Manager constructor) spent
    // the entire kTeardownLogBudget, and the success line was gated on the same
    // budget. A one-shot, non-repeating event whose only record can be silenced
    // by unrelated noise is not observable, and its silence looks exactly like
    // "the repair never ran".
    //
    // So: success always writes one line. It happens at most once per process,
    // which is precisely what makes it affordable to exempt from any cap.
    {
        // 768, not 400. The 400-byte buffer truncated every one of the 58 lines
        // in run 20261009-201304-readonly at exactly `bias_source=load_b`, and
        // the slot_* fields were the next thing in the format string -- so the
        // before/after evidence this file exists to produce had never once been
        // written down in any of the 19 archived runs. `slot_` appears in the
        // log only in the boot self-test line.
        //
        // Two fixes, because either alone would have hidden it again: the buffer
        // is now large enough for the whole line -- 580 bytes at the sizes the
        // archived runs actually produce (attempt=1319, calls_seen=1319), 605
        // with both counters at their uint32 ceiling, against 767 usable bytes,
        // so worst case leaves 162 bytes of headroom -- AND the slots moved ahead
        // of `bias_source` rather than behind it, so the field that matters is the
        // first thing on the line after `wrote=`. Truncation must not be able to
        // eat the evidence again by gaining a few characters somewhere upstream.
        char line[768];
        // Every question the brief asks the log to answer, in one line:
        // found, whether stores ran, all three slots before and after, then the
        // frame geometry, then the fold, then the verdict.
        //
        // calls_seen is here because `attempt` is not the same number: attempt
        // only advances on a COMPLETED walk, so a gap between the two names an
        // early return, and attempt alone (1319/4 in the archived runs) reads as
        // a progress counter when it is really a failure count.
        const int n = __builtin_snprintf(
            line, sizeof(line),
            "found=1 wrote=%u "
            "slot_begin_fp%+d %016llx -> %016llx  "
            "slot_end_fp%+d %016llx -> %016llx  "
            "slot_cap_fp%+d %016llx -> %016llx  "
            "end_minus_begin_before=%016llx end_minus_begin_after=%016llx "
            "attempt=%u/%u calls_seen=%u depth=%u target_fp=%016llx sp=%016llx "
            "fp_minus_sp=%016llx sp_final=%016llx frame_fp=%016llx saved_lr=%016llx "
            "elf_off=0x%llx elf_range=[0x%llx,0x%llx) bias=%016llx "
            "bias_source=load_bias "
            "guard=cbz_x20_0xcd2aa4 verdict=%s",
            didWrite ? 3u : 0u,
            kTeardownSlotBegin, (unsigned long long)before[0],
            (unsigned long long)after[0],
            kTeardownSlotEnd, (unsigned long long)before[1],
            (unsigned long long)after[1],
            kTeardownSlotCap, (unsigned long long)before[2],
            (unsigned long long)after[2],
            (unsigned long long)(before[0] - before[1]),
            (unsigned long long)(after[0] - after[1]),
            attempt + 1u, kTeardownMaxAttempts, load(g_callsSeen), walk.depth,
            (unsigned long long)walk.savedFp, (unsigned long long)sp,
            (unsigned long long)(walk.savedFp - sp),
            (unsigned long long)(walk.savedFp - kGuestFrameFpMinusSp),
            (unsigned long long)walk.frameFp,
            (unsigned long long)walk.savedLr,
            (unsigned long long)walk.elfOff,
            (unsigned long long)kGuestFrameFnLo,
            (unsigned long long)kGuestFrameFnHi,
            (unsigned long long)bias, verdict);
        repairLogBounded(line, n, sizeof(line));
    }

    t_inAttempt = false;
}

// ---------------------------------------------------------------------------
// THE CONTROLS
//
// A control that cannot fail is worse than no control, so both of these are
// built to fail.
//
// POSITIVE. A synthetic frame chain is laid out on the host exactly like the
// real one -- an anchor, then the guest logger's record, then the target's
// record -- with runtime addresses built from the SAME synthetic load bias the
// fold subtracts, so the positive case is self-consistent and cannot pass by
// agreeing with a constant transcribed from a log. The walk must find the target
// at the expected depth and compute the three slot addresses from it.
//
// The bias is the only translation input. No GuestStartupPlan is constructed,
// captured or read anywhere in this file, which is the point: on target the plan
// is not available on the guest's log path, so a control that needs one is
// testing a configuration that never happens.
//
// MUTATION. The same chain is then walked three ways, each of which is wrong in
// a way this project has actually been bitten by:
//
//   1. a fold that forgets to subtract the image base -- the bug that made
//      inrange structurally 0 for an entire run;
//   2. a depth cap one hop short of the target -- the walk "goes wrong by one
//      step", which is the failure this control exists to catch;
//   3. a slot offset shifted by 8 -- a near-miss that produces a plausible but
//      wrong address, which a test that only asks "did it find something" would
//      happily pass.
//
// Each MUST fail. If any of them passes, the positive case is measuring nothing
// and this function returns 0 to say so.
//
// The host build (tests/host/guest_teardown_repair_selftest.cpp) runs this same
// code against synthetic frames, so the mutation is verifiable with no
// emulator; the target run re-runs it against the real bias.
// ---------------------------------------------------------------------------

namespace {

// 1024 words is ample: the four records are 0x200 apart and the whole layout
// needs a little over 0x800, so the rest is slack that keeps targetFp's 0x5e0
// frame comfortably inside the arena instead of hanging off the end.
constexpr size_t kSyntheticArenaWords = 1024;

struct SyntheticChain {
    // One contiguous arena, so every frame record is a real address in real
    // memory and the walk dereferences the same way it will on target. Nothing
    // here is a stand-in for a pointer.
    alignas(16) uint64_t arena[kSyntheticArenaWords]{};
    uintptr_t arenaLo = 0;
    uintptr_t arenaHi = 0;
    uintptr_t anchorFp = 0;   // the anchor (androidLogVPrint's own x29)
    uintptr_t loggerFp = 0;   // 0x109fa18's frame record
    uintptr_t frameFp = 0;    // the record whose saved LR lands in Manager
    uintptr_t targetFp = 0;   // Manager's x29
    uint32_t targetDepth = 0; // hops the walk must take
};

// The guest's load bias, far from any ELF offset, so a fold that forgets to
// subtract it cannot accidentally land inside [kGuestFrameFnLo, kGuestFrameFnHi).
// This is the ONLY translation input the self-test uses: there is no
// GuestStartupPlan anywhere below, which is the point -- the positive control has
// to prove the fix works with no census plan in existence, because on target that
// is the whole claim.
constexpr uintptr_t kSyntheticBias = 0x2106559000ull;

// The runtime address of the guest's onViewRenderJNI export, i.e. what
// g_renderTarget holds. Reproducing the production derivation
// (bias = renderTarget - 0x12B3614) here is what makes the bias-based positive
// control SELF-CONSISTENT rather than a constant copied out of a log.
constexpr uint64_t kSyntheticRenderElfVa = 0x12B3614ull;
constexpr uint64_t kSyntheticRenderTarget = kSyntheticBias + kSyntheticRenderElfVa;

// The call sites the synthetic chain replays, straight out of the guest ELF.
constexpr uint64_t kSyntheticElfLogger = 0x109fac4ull;  // ret into 0x109fa18
constexpr uint64_t kSyntheticElfTarget = 0x000cd2a9cull;  // 0xcd2a9c, the `bl`

// Lays out anchor < logger < frame < target, ascending, all 16-byte aligned,
// and fills each record's two words. targetFp is given a plausible 0x5e0-byte
// frame below it so guestTeardownAcceptTarget accepts it for the right reasons
// rather than by accident.
// Fills in place rather than returning one: the arena is 8 KiB, and returning it
// by value would put a second copy on the caller's stack for no reason.
void BuildSyntheticChain(SyntheticChain& c, uint64_t loggerElf, uint64_t targetElf,
                         uint64_t poisonB, uint64_t poisonE, uint64_t poisonC) {
    c.arenaLo = reinterpret_cast<uintptr_t>(&c.arena[0]);
    c.arenaHi = c.arenaLo + sizeof(c.arena);

    // Anchors sit at the TOP of the arena, the target at the BOTTOM, because the
    // stack grows down and the walk requires savedFp > fp. targetFp is the low
    // address and the caller's frame is the high one, matching the real layout.
    c.targetFp = (c.arenaHi - kGuestFrameFpMinusSp) & ~static_cast<uintptr_t>(0xF);
    c.frameFp = c.targetFp - 0x200;
    c.loggerFp = c.frameFp - 0x200;
    c.anchorFp = c.loggerFp - 0x200;

    // targetFp + kGuestFrameSlotBegin and its two siblings are inside the arena
    // because targetFp - 0x5e0 == arenaBase, which is the tightest case the
    // accept() check allows: every slot sits at the very bottom of the frame.
    *reinterpret_cast<uint64_t*>(c.targetFp + kTeardownSlotBegin) = poisonB;
    *reinterpret_cast<uint64_t*>(c.targetFp + kTeardownSlotEnd) = poisonE;
    *reinterpret_cast<uint64_t*>(c.targetFp + kTeardownSlotCap) = poisonC;

    // frameFp's saved LR is the target call site; frameFp's saved fp is the
    // Manager x29 we want to recover.
    *reinterpret_cast<uint64_t*>(c.frameFp) = c.targetFp;
    *reinterpret_cast<uint64_t*>(c.frameFp + 8) = kSyntheticBias + targetElf;

    // loggerFp's saved LR is a return address into the guest logger itself --
    // inside a loaded image, so the fold succeeds, and OUTSIDE the Manager
    // range. That is the case a filter which only checked "did the fold work"
    // would wave through.
    *reinterpret_cast<uint64_t*>(c.loggerFp) = c.frameFp;
    *reinterpret_cast<uint64_t*>(c.loggerFp + 8) = kSyntheticBias + loggerElf;

    // The anchor's saved LR is a host address: in no guest image, so the fold
    // fails and the walk must keep going rather than treat it as an error.
    *reinterpret_cast<uint64_t*>(c.anchorFp) = c.loggerFp;
    *reinterpret_cast<uint64_t*>(c.anchorFp + 8) = 0x0000000000401a30ull;

    // depth counts RECORDS EXAMINED and the anchor's own record is one of
    // them, so the match lands on hop 3:
    //   1 anchorFp -> host address, fold fails, keep going
    //   2 loggerFp -> 0x109fac4, folds, OUT of range
    //   3 frameFp  -> 0xcd2a9c, in range, found
    c.targetDepth = 3;
}

// THE HISTORICAL BUG, REPRODUCED. Same input, forgets to subtract the load bias,
// so a runtime address comes back as itself.
bool MutatedFoldNoBaseSubtraction(uintptr_t bias, uintptr_t runtimeAddr,
                                  uint64_t* outOff) {
    uint64_t ignored = 0;
    if (!guestTeardownElfOffset(bias, runtimeAddr, &ignored)) {
        return false;
    }
    *outOff = static_cast<uint64_t>(runtimeAddr);
    return true;
}

// THE BIAS DERIVATION'S OWN BUG, REPRODUCED, IN THE FORM THAT MATTERS.
//
// Switching to a bare bias introduces a failure the plan never had: if the bias
// is built wrong, EVERY address folds wrong together, silently and
// symmetrically. There is no second source of truth left to disagree with it.
//
// TWO wrong biases, because one of them is easy and one is the dangerous one:
//
//   (a) kMutatedBiasMissingOneSubtraction -- production builds the bias as
//       (renderTarget - onViewRenderJNI's ELF VA); drop that subtraction and you
//       get the EXPORT'S RUNTIME ADDRESS used as the image base. It is non-zero,
//       so it passes every availability check, but it is not page-aligned, so
//       guestTeardownElfOffset rejects it outright.
//
//   (b) kMutatedBiasPlausibleWrong -- 0x20000 off the correct bias, which is
//       ALSO non-zero, ALSO page-aligned, ALSO within a plausible magnitude, and
//       ALSO the same order as every other address in the process. Nothing cheap
//       can reject it. The walk has to, and the only reason it can is that the
//       folded offset misses [kGuestFrameFnLo, kGuestFrameFnHi) by more than the
//       constructor's own 0x2634-byte length.
//
//       This is the one that would ship as "the repair silently never fires",
//       which is precisely the failure the previous run is being investigated for.
//       (b) is the control that matters; (a) only proves the alignment guard works.
constexpr uintptr_t kMutatedBiasMissingOneSubtraction = kSyntheticRenderTarget;
constexpr uintptr_t kMutatedBiasPlausibleWrong = kSyntheticBias - 0x20000;

// The walk, wired to the broken fold instead of the real one. Kept as its own
// function so the mutation is a DIFFERENCE IN CODE rather than a flag someone
// can switch off.
void MutatedWalkNoBaseSubtraction(uintptr_t bias, uintptr_t anchorFp,
                                  uintptr_t sp, uint32_t, TeardownWalk* out) {
    TeardownWalk w{};
    w.anchorFp = anchorFp;
    w.sp = sp;
    w.stop = kTeardownStopAnchorUnusable;
    if (anchorFp == 0 || (anchorFp & 0xF) != 0) {
        *out = w;
        return;
    }
    uintptr_t fp = anchorFp;
    for (uint32_t hop = 0; hop < kTeardownMaxDepth; ++hop) {
        w.depth = hop + 1;
        const uintptr_t savedFp = *reinterpret_cast<const volatile uintptr_t*>(fp);
        const uintptr_t savedLr = *reinterpret_cast<const volatile uintptr_t*>(fp + 8);
        uint64_t off = 0;
        if (MutatedFoldNoBaseSubtraction(bias, savedLr, &off)
            && off >= kGuestFrameFnLo && off < kGuestFrameFnHi) {
            w.found = true;
            w.stop = kTeardownStopFound;
            w.frameFp = fp;
            w.savedFp = savedFp;
            w.savedLr = savedLr;
            w.elfOff = off;
            *out = w;
            return;
        }
        if (savedFp <= fp) {
            w.stop = kTeardownStopNotAscending;
            *out = w;
            return;
        }
        if ((savedFp & 0xF) != 0) {
            w.stop = kTeardownStopMisaligned;
            *out = w;
            return;
        }
        if (savedFp < sp) {
            w.stop = kTeardownStopBelowSp;
            *out = w;
            return;
        }
        if (savedFp - sp > kGuestFrameMaxDistance) {
            w.stop = kTeardownStopTooFar;
            *out = w;
            return;
        }
        fp = savedFp;
    }
    w.stop = kTeardownStopDepthExhausted;
    *out = w;
}

// MUTATION 4's walk: the real one, handed a bias that was derived wrong. Kept
// separate from MutatedWalkNoBaseSubtraction because the two fail for opposite
// reasons -- that one underflows the offset, this one lands a plausible offset in
// the wrong place -- and a control that only proves one of them is half a control.
void MutatedWalkBiasMissingOneSubtraction(uintptr_t anchorFp, uintptr_t sp,
                                          uint32_t maxDepth, TeardownWalk* out) {
    guestTeardownWalkFrames(kMutatedBiasMissingOneSubtraction, anchorFp, sp,
                            maxDepth, out);
}

void MutatedWalkBiasPlausibleWrong(uintptr_t anchorFp, uintptr_t sp,
                                   uint32_t maxDepth, TeardownWalk* out) {
    guestTeardownWalkFrames(kMutatedBiasPlausibleWrong, anchorFp, sp, maxDepth,
                            out);
}

// The walk, one hop short. kTeardownMaxDepth is not used; the bound is passed in,
// so the mutation is a number and nothing else.
void MutatedWalkOneHopShort(uintptr_t bias, uintptr_t anchorFp,
                            uintptr_t sp, uint32_t maxDepth, TeardownWalk* out) {
    guestTeardownWalkFrames(bias, anchorFp, sp, maxDepth, out);
}

} // namespace

int guestTeardownRepairSelfTest() {
    // The one place that is guaranteed to run before the guest does, so it is
    // where the exit summary gets armed. Registering it here rather than from a
    // static initializer means the handler is installed on every path that
    // reaches the probe, and a boot that never gets this far has nothing to
    // report anyway.
    installExitSummary();
    uint32_t failed = 0;
    uint32_t passed = 0;
    auto check = [&failed, &passed](bool ok, const char* what) {
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
        // Printed to the boot console as well as folded into the line below,
        // because a control that only reports a number cannot be argued with.
        std::printf("%-5s teardown-repair-selftest: %s\n", ok ? "ok" : "FAIL", what);
    };

    // THE ONLY TRANSLATION INPUT. No GuestStartupPlan is constructed, captured,
    // read or referenced anywhere in this function, and none exists on the
    // device when it runs. That is the property the positive control exists to
    // prove: the walk finds the target frame from the load bias alone.
    const uintptr_t bias = kSyntheticBias;
    // The synthetic arena is a local of this self-test, laid out to look exactly
    // like the real thing: the target's 0x5e0 frame at the bottom, the anchor's
    // frame at the top, and every hop ascending.
    //
    // `sp` is deliberately NOT the real sp. It is placed at exactly
    // kGuestFrameMaxDistance below targetFp, so the distance check has to be
    // genuinely satisfied rather than trivially so -- a control that passes
    // because the guard was never close to binding is not testing the guard.
    static SyntheticChain chain;
    BuildSyntheticChain(chain, kSyntheticElfLogger, kSyntheticElfTarget,
                        0x1111111111111111ull, 0x2222222222222222ull,
                        0x3333333333333333ull);
    const uintptr_t syntheticSp =
        chain.targetFp - kGuestFrameMaxDistance;

    // ---- 1. POSITIVE: the walk finds the planted frame at the right depth. ---
    TeardownWalk walk{};
    guestTeardownWalkFrames(bias, chain.anchorFp, syntheticSp, kTeardownMaxDepth,
                            &walk);
    check(walk.found, "positive: walk finds the frame whose saved LR is in Manager");
    check(walk.depth == chain.targetDepth,
          "positive: found at the expected hop count (3)");
    check(walk.targetReject == kTeardownTargetOk, "positive: no target reject");
    check(walk.savedFp == chain.targetFp,
          "positive: recovered fp == the Manager x29 we planted");
    check(walk.elfOff == kSyntheticElfTarget,
          "positive: folded ELF offset == the planted call site 0xcd2a9c");
    check(walk.stop == kTeardownStopFound, "positive: stop == found");

    // ---- 2. POSITIVE: the slot addresses are the ones we would write. -------
    // Not "three addresses inside some frame" -- the exact three, derived from
    // the recovered fp. A wrong offset that still lands in-frame would satisfy a
    // looser test, which is why the addresses are compared literally.
    {
        const uintptr_t wantB = chain.targetFp + kTeardownSlotBegin;
        const uintptr_t wantE = chain.targetFp + kTeardownSlotEnd;
        const uintptr_t wantC = chain.targetFp + kTeardownSlotCap;
        check(wantB == chain.targetFp - 0x48
              && wantE == chain.targetFp - 0x40
          && wantC == chain.targetFp - 0x38,
              "positive: slots are [fp-0x48] [fp-0x40] [fp-0x38] exactly");
        check(guestTeardownAcceptTarget(chain.targetFp, syntheticSp)
              == kTeardownTargetOk,
              "positive: the planted frame is accepted for a write");
        // And the planted poison is really there to be found -- a control that
        // reads zeros proves nothing about a repair that reads garbage.
        check(*reinterpret_cast<uint64_t*>(wantB) == 0x1111111111111111ull
              && *reinterpret_cast<uint64_t*>(wantE) == 0x2222222222222222ull
          && *reinterpret_cast<uint64_t*>(wantC) == 0x3333333333333333ull,
              "positive: the planted begin/end/cap poison reads back");
    }

    // ---- 3. MUTATION 1: the fold forgets the image base. MUST NOT FIND. -----
    // This is the exact bug that shipped for a run: a runtime address came back
    // as itself, every runtime address is above kGuestFrameFnHi, and the filter
    // was structurally 0 forever while the counters said the probe was alive.
    {
        TeardownWalk bad{};
        MutatedWalkNoBaseSubtraction(bias, chain.anchorFp, syntheticSp,
                                     kTeardownMaxDepth, &bad);
        check(!bad.found,
              "MUTATION 1: fold without base subtraction finds NOTHING");
        check(bad.found == false && (kSyntheticBias > kGuestFrameFnHi),
              "MUTATION 1: the error IS the forgotten base, and base > FnHi");
        // And the real fold, on the same input, DOES find it. Without this the
        // mutation would pass for the wrong reason (a broken chain rather than
        // a broken fold).
        check(walk.found,
              "MUTATION 1: the REAL fold still finds it (so mutation 1 is the fold)");
    }

    // ---- 4. MUTATION 2: the walk goes one hop short. MUST NOT FIND. ---------
    // "Off by one step" in the most literal sense: the cap is one less than the
    // hop count the target sits at, so the target record is never examined.
    {
        TeardownWalk bad{};
        const uint32_t shortBy = chain.targetDepth - 1u;  // 1
        MutatedWalkOneHopShort(bias, chain.anchorFp, syntheticSp, shortBy, &bad);
        check(!bad.found,
              "MUTATION 2: a walk one hop short finds NOTHING");
        check(bad.stop == kTeardownStopDepthExhausted,
              "MUTATION 2: it stops as depth_exhausted, not as a crash");
        // The full walk still finds it, at exactly shortBy+1 hops.
        check(walk.found && walk.depth == shortBy + 1u,
              "MUTATION 2: the REAL walk needs exactly one more hop");
    }

    // ---- 5. MUTATION 3: a slot offset shifted by 8. MUST BE REJECTED. -------
    // The near-miss a "did it find something" test waves through: the address
    // still lands in the frame, but it is the wrong word. This is why the
    // positive case compares literal addresses and not containment.
    {
        const uintptr_t shifted = chain.targetFp + kTeardownSlotBegin + 8;
        check(shifted != chain.targetFp + kTeardownSlotBegin,
              "MUTATION 3: +8 yields a different address than [fp-0x48]");
        // And the three real offsets are all distinct -- a duplicate offset
        // would silently write the same word three times.
        check(kTeardownSlotBegin != kTeardownSlotEnd
              && kTeardownSlotEnd != kTeardownSlotCap
              && kTeardownSlotBegin != kTeardownSlotCap,
              "MUTATION 3: begin/end/cap offsets are all distinct");
    }

    // ---- 6. FAIL-OPEN: bad inputs are refused, never guessed. ---------------
    {
        check(walk.found && walk.savedFp != 0,
              "fail-open: a found walk always carries a non-zero target fp");
        // A target below our sp is refused (we are executing inside that frame).
        check(guestTeardownAcceptTarget(chain.targetFp, chain.targetFp + 0x10)
              == kTeardownTargetBelowSp,
              "fail-open: target below sp is refused");
        // A misaligned target is refused.
        check(guestTeardownAcceptTarget(chain.targetFp + 8, syntheticSp)
              == kTeardownTargetMisaligned,
              "fail-open: misaligned target is refused");
        // A target further than the bound is refused. syntheticSp puts the real
        // target at EXACTLY the bound, so this is the first value past it.
        check(guestTeardownAcceptTarget(chain.targetFp + kGuestFrameMaxDistance + 0x10,
                                        syntheticSp)
              == kTeardownTargetTooFar,
              "fail-open: target too far above sp is refused");
        // The bound itself is NOT rejected: the guard is `>`, not `>=`.
        check(guestTeardownAcceptTarget(chain.targetFp, syntheticSp)
              == kTeardownTargetOk,
              "fail-open: a target exactly at the distance bound is accepted");
        // A near-zero target cannot produce a representable frame base.
        check(guestTeardownAcceptTarget(0x40, 0) != kTeardownTargetOk,
              "fail-open: a low target with no frame below it is refused");
        // A chain that descends instead of ascending stops instead of looping.
        {
            static SyntheticChain loop;
            BuildSyntheticChain(loop, kSyntheticElfLogger, kSyntheticElfTarget,
                                0, 0, 0);
            // Point loggerFp's saved fp back DOWN to anchorFp. It has to be
            // planted on a record the walk must ADVANCE FROM: the first version
            // of this test pointed frameFp downward, which is the record that
            // MATCHES, so the walk returned before the guard was ever reached
            // and the control failed. On loggerFp the walk folds, finds the
            // address out of range, and then meets a downward pointer.
            *reinterpret_cast<uint64_t*>(loop.loggerFp) = loop.anchorFp;
            TeardownWalk cyc{};
            guestTeardownWalkFrames(bias, loop.anchorFp, syntheticSp,
                                    kTeardownMaxDepth, &cyc);
            check(!cyc.found && cyc.stop == kTeardownStopNotAscending,
                  "fail-open: a descending chain stops as not_ascending");
        }
    }

    // ---- 7. The mutation's structural argument, in one number. -------------
    // The reason the no-base fold can NEVER match: a runtime address is always
    // above kGuestFrameFnHi. This is what made the historical inrange=0
    // invisible.
    {
        const uint64_t runtimeTarget = kSyntheticBias + kSyntheticElfTarget;
        uint64_t folded = 0;
        const bool good = guestTeardownElfOffset(bias, runtimeTarget, &folded);
        uint64_t bad = 0;
        const bool badOk = MutatedFoldNoBaseSubtraction(bias, runtimeTarget, &bad);
        std::printf("     target runtime=0x%llx real_fold=0x%llx mutated_fold=0x%llx "
                    "(delta=0x%llx)\n",
                    (unsigned long long)runtimeTarget, (unsigned long long)folded,
                    (unsigned long long)bad,
                    (unsigned long long)(bad - folded));
        check(good && folded == kSyntheticElfTarget,
              "translation: the real fold recovers the planted ELF offset");
        check(badOk && bad == runtimeTarget && bad >= kGuestFrameFnHi,
              "translation: the mutated fold returns the runtime address, > FnHi");
    }

    // ---- 7b. THE BIAS, BUILT THE WAY PRODUCTION BUILDS IT. ---------------
    // The positive control above takes the bias as a constant. This one derives
    // it, with the same two terms production uses, and then re-runs the whole
    // walk with the derived value:
    //
    //     bias = g_renderTarget - 0x12B3614      (onViewRenderJNI's ELF VA)
    //
    // If that derivation and the planted chain ever stop agreeing, the control
    // fails here rather than on a device where the only symptom is a repair that
    // silently never happens. It is also the check that the bias alone -- with no
    // GuestStartupPlan constructed, captured or referenced -- is sufficient.
    {
        const uintptr_t derivedBias = static_cast<uintptr_t>(kSyntheticRenderTarget)
                                    - kSyntheticRenderElfVa;
        std::printf("     bias derived: render_target=0x%llx - elf_va=0x%llx "
                    "= 0x%llx\n",
                    (unsigned long long)kSyntheticRenderTarget,
                    (unsigned long long)kSyntheticRenderElfVa,
                    (unsigned long long)derivedBias);
        check(derivedBias == bias,
              "positive: bias == renderTarget - onViewRenderJNI ELF VA");

        TeardownWalk derived{};
        guestTeardownWalkFrames(derivedBias, chain.anchorFp, syntheticSp,
                                kTeardownMaxDepth, &derived);
        check(derived.found,
              "positive: the DERIVED bias alone locates the target frame");
        check(derived.depth == chain.targetDepth,
              "positive: derived bias finds it at the same hop count");
        check(derived.savedFp == chain.targetFp
                  && derived.elfOff == kSyntheticElfTarget,
              "positive: derived bias recovers the same fp and ELF offset");
        check(guestTeardownAcceptTarget(derived.savedFp, syntheticSp)
                  == kTeardownTargetOk,
              "positive: the frame found via the derived bias is writable-legal");
    }

    // ---- 7c. MUTATION 4: the bias is wrong. MUST NOT FIND. ---------------
    // Two wrong biases, because one is easy and one is the dangerous one.
    {
        // (a) An export's runtime address used as the image base. Non-zero, so it
        // passes any availability check -- and not page-aligned, so the fold
        // rejects it outright.
        TeardownWalk bad{};
        MutatedWalkBiasMissingOneSubtraction(chain.anchorFp, syntheticSp,
                                             kTeardownMaxDepth, &bad);
        check(!bad.found,
              "MUTATION 4a: a bias short by one subtraction finds NOTHING");
        check(kMutatedBiasMissingOneSubtraction != 0
                  && (kMutatedBiasMissingOneSubtraction & 0xFFF) != 0,
              "MUTATION 4a: the wrong bias is non-zero but NOT page-aligned "
              "(so the alignment guard is what rejects it)");
        uint64_t wrongA = 0;
        const bool wrongAOk = guestTeardownElfOffset(
            kMutatedBiasMissingOneSubtraction,
            kSyntheticBias + kSyntheticElfTarget, &wrongA);
        check(!wrongAOk,
              "MUTATION 4a: the fold declines rather than folding against it");

        // (b) 0x20000 off the correct bias: non-zero, page-aligned, plausible
        // magnitude, same order as every other address. NOTHING cheap rejects
        // this one -- only the walk can, and it is the case that would ship as
        // "the repair silently never fires".
        TeardownWalk bad2{};
        MutatedWalkBiasPlausibleWrong(chain.anchorFp, syntheticSp,
                                      kTeardownMaxDepth, &bad2);
        check(!bad2.found,
              "MUTATION 4b: a page-aligned bias that is 0x20000 wrong finds NOTHING");
        check(kMutatedBiasPlausibleWrong != 0
                  && (kMutatedBiasPlausibleWrong & 0xFFF) == 0
                  && kMutatedBiasPlausibleWrong > 0x10000000ull,
              "MUTATION 4b: the wrong bias passes EVERY cheap guard "
              "(non-zero, page-aligned, plausible magnitude)");
        uint64_t wrongB = 0;
        const bool wrongBOk = guestTeardownElfOffset(
            kMutatedBiasPlausibleWrong, kSyntheticBias + kSyntheticElfTarget,
            &wrongB);
        std::printf("     mutation4b: bias=0x%llx -> off=0x%llx (ok=%d) "
                    "wanted [0x%llx,0x%llx)\n",
                    (unsigned long long)kMutatedBiasPlausibleWrong,
                    (unsigned long long)wrongB, wrongBOk ? 1 : 0,
                    (unsigned long long)kGuestFrameFnLo,
                    (unsigned long long)kGuestFrameFnHi);
        check(wrongBOk && wrongB != kSyntheticElfTarget
                  && !(wrongB >= kGuestFrameFnLo && wrongB < kGuestFrameFnHi),
              "MUTATION 4b: the wrong bias folds the real call site OUT of range");
        // And the real bias still finds it, so mutation 4 is the bias and not a
        // broken chain or a broken walk.
        check(walk.found && walk.depth == chain.targetDepth,
              "MUTATION 4: the REAL bias still finds it (so mutation 4 is the bias)");
    }

    // ---- 8. The verdict, into the ARCHIVED log, with every field printed. ---
    // A control whose result is a bare 0/1 cannot be argued with afterwards, and
    // this project has been bitten by a field that was computed and then not
    // printed. Every computed value appears in this line.
    {
        char verdict[448];
        const int n = __builtin_snprintf(
            verdict, sizeof(verdict),
            "pass=%d expected=1 checks_passed=%u checks_failed=%u "
            "anchor_fp=%016llx target_fp=%016llx frame_fp=%016llx "
            "synthetic_sp=%016llx found=%d depth=%u expected_depth=%u "
            "elf_off=0x%llx target_elf=0x%llx bias=0x%llx "
            "render_target=0x%llx render_elf_va=0x%llx "
            "elf_range=[0x%llx,0x%llx) slot_off=[%d,%d,%d] "
            "mut1_found=%d mut2_found=%d mut4_found=%d verdict=%s",
            failed == 0 ? 1 : 0, passed, failed,
            (unsigned long long)chain.anchorFp, (unsigned long long)walk.savedFp,
            (unsigned long long)walk.frameFp, (unsigned long long)syntheticSp,
            walk.found ? 1 : 0, walk.depth, chain.targetDepth,
            (unsigned long long)walk.elfOff, (unsigned long long)kSyntheticElfTarget,
            (unsigned long long)bias,
            (unsigned long long)kSyntheticRenderTarget,
            (unsigned long long)kSyntheticRenderElfVa,
            (unsigned long long)kGuestFrameFnLo,
            (unsigned long long)kGuestFrameFnHi,
            kTeardownSlotBegin, kTeardownSlotEnd, kTeardownSlotCap,
            // Re-run the mutations for the log so the line states the mutations'
            // verdicts rather than asserting them in prose.
            [&] { TeardownWalk m{}; MutatedWalkNoBaseSubtraction(
                      bias, chain.anchorFp, syntheticSp, kTeardownMaxDepth, &m);
                  return m.found ? 1 : 0; }(),
            [&] { TeardownWalk m{}; MutatedWalkOneHopShort(
                      bias, chain.anchorFp, syntheticSp, chain.targetDepth - 1u, &m);
                  return m.found ? 1 : 0; }(),
            [&] { TeardownWalk m{}; MutatedWalkBiasMissingOneSubtraction(
                      chain.anchorFp, syntheticSp, kTeardownMaxDepth, &m);
                  return m.found ? 1 : 0; }(),
            failed == 0 ? "TEARDOWN_REPAIR_PASS" : "TEARDOWN_REPAIR_FAIL");
        repairLogBounded(verdict, n, sizeof(verdict));
    }

    return failed == 0 ? 1 : 0;
}

} // namespace rr3
