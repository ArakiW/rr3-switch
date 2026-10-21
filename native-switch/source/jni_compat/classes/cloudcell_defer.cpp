#include <switch.h>

#include <atomic>
#include <vector>

#include "../../eap_port/trace_adapter.h"
#include "cloudcell_defer.h"

/* The one bounded probe below needs a sink that lands in Eden's debug output,
 * because that is where every other alert-pump measurement line goes
 * (jni_bridge.cpp:2122 and its siblings). The obvious candidate is rawLog --
 * and this is NOT a copy of it by choice: rawLog at jni_bridge.cpp:478 sits
 * inside the anonymous namespace opened at jni_bridge.cpp:73, so it has
 * internal linkage and a call to it from this translation unit does not link
 * ("undefined reference to `rawLog(char const*)'"). Moving it out of that
 * namespace would mean editing a file this change does not own, for one call
 * site, so the same two operations it performs are done here directly:
 * svcOutputDebugString over a bounded NUL-terminated buffer. Same primitive,
 * no stdio, no malloc, no TLS, no file handle -- which is also what makes it
 * safe on the raw threadCreate thread that runs cloudcell_pump("alert-pump"). */
static void debugLine(const char *text)
{
    if (text != nullptr) {
        /* Bounded on purpose: rawStrlen at jni_bridge.cpp:470 caps at 192 for the
         * same reason, and the only buffer ever passed in is a 256-byte local
         * below, so the cap is a belt-and-braces guard and not a limit that
         * can truncate a real line. */
        size_t n = 0;
        while (text[n] != '\0' && n < 256) {
            ++n;
        }
        svcOutputDebugString(text, n);
    }
}

/* Cloudcell owns worker threads (CC_Thread_Class) and some of these facades are
 * reachable from them, so the queue is locked even though the drain is always
 * the game thread.
 *
 * libnx Mutex, NOT the C++ std mutex, and the difference is measured rather
 * than stylistic: a std mutex on this path took host CPU from 13.7 s to 80.7 s
 * by starving the Eden scheduler, because the host's pthread_mutex_lock is
 * shimmed through pthread_bridge into guestPthreadMutexLock -- a std mutex here
 * drags the guest's threading machinery into a host lock with guest-visible
 * latency.
 * This matters beyond the rule: cloudcell_pump is also called by alertPumpWorker,
 * a raw threadCreate host thread (jni_bridge.cpp:2045), whose only lock on the
 * tick-1 path is this one. Same primitive and same idiom as the working
 * reporter threads: pthread_bridge.cpp:95 (gRegistryLock) and
 * guest_view_probe.cpp:667 (gSamplerWaitLock).
 *
 * A libnx Mutex is not recursive, so the discipline below is load-bearing rather
 * than a convention: take, copy, drop, and never acquire it twice in one call.
 * Every acquisition in this file is paired on the same path, including the
 * empty-queue early return, which unlocks explicitly.
 *
 * mutexTryLock is not used AS the acquisition. On the contended per-frame path
 * (always_log=false) a failed try would return without emitting the pump line,
 * which is exactly the readable "depth was always 0" negative result the
 * comment on that branch exists to preserve. It is used once, as a
 * non-blocking PROBE in front of the drain's acquisition: the probe reports,
 * and the same blocking mutexLock still happens, so the number of acquisitions
 * and the outcome of every pump are unchanged. */
static Mutex g_queue_lock{};

/* One deferred item. `tag` is the name of the site that produced it -- a
 * string literal, so it outlives the process and the pump can print it
 * without owning it -- and `id` is the enqueue sequence number, which makes
 * an item identifiable even if two sites use the same tag.
 *
 * MEASURED 2026-10-20, archive 20261020-223250-eden-ccmodebg: the frame-1
 * batch held exactly ONE item (enqueued_total=1, entered_total=1,
 * drained_total=0, post-run pump line absent) and the process made exactly
 * ONE enqueue in 300 s. The counters said "one item, and it never
 * returned"; they could not say WHICH one. That is what the tag is for. */
struct DeferredItem {
    const char *tag;
    unsigned id;
    std::function<void()> fn;
};
static std::vector<DeferredItem> g_queue;

/* Instrumentation (pump-instrumentation run). `g_enqueued_total` exists so the
 * pump line can distinguish "the queue was empty because nothing was ever
 * deferred" from "the queue was empty because the previous pump took it" -
 * without it, an always-zero depth is ambiguous and looks identical to a pump
 * that works perfectly. */
static unsigned long long g_enqueued_total = 0;
static unsigned long long g_entered_total = 0;
static unsigned long long g_drained_total = 0;
static unsigned g_enqueue_lines = 0;

/* One-shot for the life of the process, consumed by the first drain that finds
 * the queue non-empty. Non-empty is the whole gate on purpose: an empty pump
 * takes this lock and drops it again without touching anything else, so it can
 * never be contended and probing it would only burn the one shot on the six
 * bracketing pumps that run at view-creation time. The first contended
 * acquisition of the process is the alert-pump window, which is the window
 * under suspicion.
 *
 * File-scope std::atomic with exchange(), not a function-local static with a
 * non-trivial initialiser: that form is guarded by __cxa_guard_acquire, which
 * takes a lock on first use, and this flag's whole job is to decide whether to
 * probe a lock. A guard would put an unmeasured lock acquisition directly in
 * front of the one place that is allowed to touch a lock here. Same shape and
 * same reason as g_alertPumpEntryReported at jni_bridge.cpp:2050. */
static std::atomic<bool> g_drain_lock_probe_spent{false};

/* The one bounded lock line. Split out of cloudcell_pump so the failure path
 * can emit it BEFORE the blocking acquire it reports on -- a line emitted after
 * that acquire would never appear in exactly the run it exists to explain.
 *
 * The counters are read without the lock on the failure path, because on that
 * path the lock belongs to somebody else and taking it to read them would be
 * the thing being measured. They are consistent on the try-ok path, which runs
 * inside the critical section. The gate that selects this path reads
 * g_queue.empty() unlocked too; that is a member load on a file-scope vector
 * that push_back only ever rewrites and never destroys, so it cannot fault and
 * cannot be anything but a slightly stale yes/no on a line that prints once.
 *
 * handle= is here because it is free and it is decisive.
 *
 * CORRECTION 2026-10-16. The comment that used to stand here was:
 *
 *   "Mutex IS a bare u32 handle in this libnx. Every `Mutex x{}` in this
 *    project is therefore the handle 0, and nothing in this project ever
 *    calls mutexCreate."
 *
 * Every clause of that is either wrong or irrelevant, and the conclusion it
 * invited -- that these locks provide no mutual exclusion and need a
 * mutexCreate -- was wrong. They do provide exclusion, and they must NOT be
 * given a mutexCreate.
 *
 * libnx/kernel/mutex.h says `typedef _LOCK_T Mutex;` and, in mutexInit,
 * "A mutex can also be statically initialized by assigning 0 to it."
 * _LOCK_T is newlib's low-level lock: an integer holding the OWNING THREAD's
 * id. 0 therefore means *unowned*, which is the free state, not an invalid
 * kernel handle. mutexCreate belongs to libnx's kernel Mutex API, which this
 * type is not.
 *
 * The premise that settled it is in this file, not in a header: writing
 * `.handle` fails to compile, so `Mutex` has no members, and line 123 casts
 * one to `unsigned` and has always compiled -- C++ will not cast a struct to
 * an integral type. An integer owner field is exactly what newlib expects.
 *
 * The runtime evidence agrees and was never ambiguous: the 2026-10-04 note at
 * jni_bridge.cpp records the alert-pump thread BLOCKING on this very lock
 * while the guest main thread held it. A lock that does not exclude does not
 * block. That measurement is what made the "handle 0" reading look true.
 *
 * Left alone deliberately: the field prints the owner id, which is 0 when free
 * and the owning thread id when held -- still the cheapest way to tell "free"
 * from "held by somebody". Do not add mutexCreate to make it look like a
 * kernel handle; that would replace a working lock with a broken one.
 */
static void reportDrainLockProbe(const char *site, const char *outcome,
                                 bool locked)
{
    char probe[256];
    __builtin_snprintf(probe, sizeof(probe),
                       "cloudcell-pump: queue-lock site=%s outcome=%s"
                       " locked=%d handle=0x%x depth=%zu enqueued_total=%llu"
                       " entered_total=%llu drained_total=%llu",
                       site, outcome, locked ? 1 : 0,
                       (unsigned)g_queue_lock, g_queue.size(),
                       g_enqueued_total, g_entered_total, g_drained_total);
    debugLine(probe);
}

/* entered vs drained, and why both exist.
 *
 * A drained callback is a `blr` into guest ARM64 code. That code does not owe
 * this bridge a return: on the measured run the guest reached its
 * fatal-dialog handler from inside one of these callbacks, and a callback that
 * throws control away never returns to the statement that logs the post-run
 * line. So a single post-run line is not evidence that a drain happened, it is
 * only evidence that a drain FINISHED -- and when the interesting case is the
 * one that does not finish, the log is cut off by the very thing being
 * observed.
 *
 * g_entered_total is therefore bumped under the lock, before any fn() runs:
 * it counts entries RELEASED FOR EXECUTION. g_drained_total is bumped after the
 * loop and counts entries OBSERVED TO RETURN. A run where entered_total >=
 * drained_total with a missing post-run line is a batch that did not finish,
 * and that is now readable instead of silent. */


/* An enqueue line on every defer would flood the log on a chain that re-arms
 * itself per request; the running total in the pump line carries the rest. */
static const unsigned kMaxEnqueueLines = 32;

/* The per-item enter/return lines are the instrument that NAMES the callback
 * that wedges, so they are bounded but generously: 96 lines covers 32 items
 * with both an enter and a return, which is more than the 300 s run ever
 * enqueued.
 *
 * The budget is a plain counter and it is touched from whichever thread
 * drains the queue, so two threads draining at once can overshoot the cap by
 * one or two lines. That is deliberate: making it atomic would put a locked
 * operation in front of the one place this file is allowed to touch shared
 * state, and an overshoot of two lines costs nothing while a lock there costs
 * the run its diagnostic. */
static unsigned g_item_lines = 0;
static const unsigned kMaxItemLines = 96;

static bool itemLineBudget(void)
{
    if (g_item_lines >= kMaxItemLines) {
        return false;
    }
    ++g_item_lines;
    return true;
}

void cloudcell_defer(const char *tag, std::function<void()> fn)
{
    unsigned depth = 0;
    unsigned long long total = 0;
    unsigned id = 0;
    {
        mutexLock(&g_queue_lock);
        total = ++g_enqueued_total;
        id = (unsigned)total;
        g_queue.push_back(DeferredItem{tag, id, std::move(fn)});
        depth = (unsigned)g_queue.size();
        mutexUnlock(&g_queue_lock);
    }

    if (g_enqueue_lines < kMaxEnqueueLines) {
        ++g_enqueue_lines;
        trace("cloudcell_defer: n=%u total=%llu depth=%u tag=%s", g_enqueue_lines,
              total, depth, tag != nullptr ? tag : "(null)");
    }
}

void cloudcell_pump(const char *site, bool always_log)
{
    std::vector<DeferredItem> batch;
    unsigned depth_before = 0;
    unsigned depth_after_swap = 0;
    unsigned long long enqueued_before = 0;
    /* Every read of the three totals goes through a snapshot taken under
     * g_queue_lock, so the pair of lines a human compares is consistent by
     * construction instead of by luck. Sampled once per critical section: in
     * the first block below, and again in the block after the loop. */
    unsigned long long enqueued_at = 0;
    unsigned long long entered_at = 0;
    unsigned long long drained_at = 0;

    {
        /* One acquisition, exactly as it was. The probe in front of it is
         * one-shot for the process and only runs on a non-empty queue, and
         * either way exactly one acquisition of g_queue_lock happens on every
         * path through this block:
         *
         *   try-ok      -> the probe already holds it; nothing to take.
         *   try-failed  -> the line is already out, and the call below is the
         *                  same blocking mutexLock that was here before, with
         *                  the same wait and the same result.
         *   not probed  -> the call below, unchanged.
         *
         * The Result of the blocking call is still discarded, as it was, so no
         * control flow in the pump depends on it. What the probe adds is the
         * one distinction a reader cannot otherwise make: whether this thread
         * ever got the lock. A try-ok line with a non-zero handle and a
         * non-empty queue means the block is NOT the acquisition and everything
         * after this point is still unexplained. A try-failed line means the
         * acquisition is where the block is, and the handle field then says
         * whether the lock is a real kernel mutex or a zero handle that no
         * mutexCreate ever made. Silence means the probe was consumed earlier
         * on an uncontended pump, which is itself the only other reading. */
        bool probed = false;
        bool held_by_probe = false;
        if (!g_queue.empty() &&
            !g_drain_lock_probe_spent.exchange(true, std::memory_order_relaxed)) {
            probed = true;
            held_by_probe = mutexTryLock(&g_queue_lock);
            if (!held_by_probe)
                reportDrainLockProbe(site, "try-failed", false);
        }
        if (!held_by_probe)
            mutexLock(&g_queue_lock);
        if (probed && held_by_probe)
            reportDrainLockProbe(site, "try-ok", true);
        depth_before = (unsigned)g_queue.size();
        enqueued_before = g_enqueued_total;
        if (g_queue.empty()) {
            /* The empty case is the interesting one, so it is logged too: a run
             * where every line reads depth_before=0 with a frozen total of 0
             * enqueues is a negative result about the deferral, not a quiet
             * success. The per-frame site opts out, because an always-empty
             * queue would then write one line per frame forever. An empty batch
             * cannot be hidden by a non-returning callback -- there is no
             * callback -- so this line needs no "entering" twin. */
            if (always_log) {
                trace("cloudcell_pump: site=%s depth_before=0 drained=0 depth_after=0 "
                      "enqueued_total=%llu entered_total=%llu drained_total=%llu error=0",
                      site, g_enqueued_total, g_entered_total, g_drained_total);
            }
            /* Unlocked explicitly here rather than left to a guard: this is the
             * only early exit out of a locked region in this file, and with a
             * non-recursive libnx Mutex a missed unlock wedges the queue for
             * every other thread, not just for this one. */
            mutexUnlock(&g_queue_lock);
            return;
        }
        batch.swap(g_queue);
        /* Sampled in the same critical section as depth_before, so the pair is a
         * consistent before/after of THIS swap and not a value some other
         * thread moved between two reads. */
        depth_after_swap = (unsigned)g_queue.size();
        /* Entered, not completed: bumped here, before any fn() is called, so a
         * callback that never returns is still counted. */
        g_entered_total += batch.size();
        /* Snapshotted after the bump, so entered_at already counts this batch:
         * entered is RELEASED FOR EXECUTION, exactly as it was read before. */
        enqueued_at = g_enqueued_total;
        entered_at = g_entered_total;
        drained_at = g_drained_total;
        mutexUnlock(&g_queue_lock);
    }

    /* Lock released. Confirmed by reading the function: the batch is a local
     * vector filled by swap() under g_queue_lock, and the explicit mutexUnlock
     * at the end of the block above closes the critical section -- every fn()
     * below runs with no lock held.
     * The header claim holds, and this line is deliberately outside the lock so
     * a log write can never be what a callback contends for.
     *
     * THE LINE THAT SURVIVES A NON-RETURNING CALLBACK. It is emitted after the
     * unlock and before the first fn(), so if the batch kills the process on
     * its first entry this is already on disk. Field vocabulary is unchanged
     * (site, depth_before, drained, depth_after, enqueued_total, error) so an
     * existing parser keeps working; phase=entering is what distinguishes it
     * from the post-run line, and drained= is repeated here deliberately -- on
     * THIS line it means "about to run", which the phase= token says and the
     * post-run line's means "returned". error=0 is not a placeholder: no
     * callback has run yet, so nothing can have extended this batch. */
    trace("cloudcell_pump: site=%s phase=entering drained=%zu entering=%zu "
          "depth_before=%u depth_after=%u enqueued_total=%llu entered_total=%llu "
          "drained_total=%llu error=0",
          site, batch.size(), batch.size(), depth_before, depth_after_swap,
          enqueued_at, entered_at, drained_at);

    /* Drained outside the lock: a callback is free to queue the next request,
     * which is exactly what CC_SyncManager_Class does when one blob completes
     * and the next one is posted. Those land in the following frame's batch
     * rather than extending this one, which keeps a failing endpoint from
     * spinning the whole chain inside a single frame.
     *
     * Re-entrancy: the batch is a local vector, never the shared one, so a
     * callback that calls cloudcell_pump() re-entrantly sees only what other
     * threads have queued and cannot observe or re-run its own entry.
     *
     * THE PER-ITEM LINES. One on entry and one on return, each naming the
     * item. They are the difference between "a batch of one did not finish"
     * and "a batch of one DID NOT FINISH, AND IT WAS <this>".
     *
     * The return line is the one that never appears for the wedging item,
     * and that absence IS the measurement: an enter line with no matching
     * return line, followed by the absence of the post-run pump line and
     * drained_total not advancing, is a callback that entered guest code and
     * never came back.
     *
     * The enter line is written BEFORE fn() runs, so a callback that wedges
     * on its first instruction still leaves its name on disk. Both lines are
     * outside g_queue_lock, for the same reason the pump's own lines are. */
    for (auto &item : batch) {
        const char *name = item.tag != nullptr ? item.tag : "(null)";
        if (itemLineBudget()) {
            trace("cloudcell-item: enter id=%u tag=%s site=%s", item.id, name,
                  site);
        }
        item.fn();
        if (itemLineBudget()) {
            trace("cloudcell-item: return id=%u tag=%s", item.id, name);
        }
    }

    unsigned depth_after = 0;
    unsigned long long reentrant = 0;
    {
        mutexLock(&g_queue_lock);
        /* Completed this time, and now under the lock it is read under below:
         * the two totals are compared against each other by a human reading the
         * log, so they have to be sampled consistently. */
        g_drained_total += batch.size();
        depth_after = (unsigned)g_queue.size();
        /* Items enqueued DURING this drain, i.e. by a callback that extended the
         * batch. enqueued_before was already sampled under the lock at the swap
         * (line ~286), and the batch's own items were enqueued BEFORE that
         * sample -- so they are already counted in it. Subtracting batch.size()
         * again double-counts them and underflows: a run where nothing was
         * enqueued during the drain reported error=18446744073709551613
         * (0xFFFFFFFFFFFFFFFD, i.e. -3 for a batch of three) instead of 0.
         * Measured in archive 20261021-121502-eden-mgrprobe4:
         *   phase=cloudcell_pump: site=frame ... enqueued_total=3 entered_total=3
         *     drained_total=3 error=18446744073709551613
         * Every one of the three callbacks had run and returned, so the "error"
         * was the arithmetic, not the drain. */
        reentrant = g_enqueued_total - enqueued_before;
        enqueued_at = g_enqueued_total;
        entered_at = g_entered_total;
        drained_at = g_drained_total;
        mutexUnlock(&g_queue_lock);
    }

    /* `error` is non-zero only if a callback enqueued during the drain - the
     * one condition that would mean a batch extended itself, which the design
     * above is supposed to prevent. */
    trace("cloudcell_pump: site=%s depth_before=%u drained=%zu depth_after=%u "
          "enqueued_total=%llu entered_total=%llu drained_total=%llu error=%llu",
          site, depth_before, batch.size(), depth_after, enqueued_at,
          entered_at, drained_at, reentrant);
}
