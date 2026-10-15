#pragma once

// THE ROOT-CAUSE REPAIR: hand the guest's unconstructed local vector the one
// initial state it is allowed to have, once, from inside the guest's own call
// path.
//
// WHAT IS BROKEN, IN THE GUEST'S OWN TERMS
// -----------------------------------------
// CC_Helpers::Manager::Manager(...) spans ELF [0xcd1014, 0xcd3648) -- 2445
// instructions. Before it returns it destroys a LOCAL
// std::vector<std::pair<std::string,std::string>> living at x29-0x80, whose
// three words are:
//
//   [x29-0x48]  begin     0 writes in the whole function
//   [x29-0x40]  end       1 write, in the destructor's OWN path at 0xcd2b04
//   [x29-0x38]  capacity  0 writes in the whole function
//
// So the vector is never constructed and never used, yet its destructor runs:
//
//   0xcd2aa0  ldur x20, [x29,#-0x48]    ; begin
//   0xcd2aa4  cbz  x20, #0xcd2b0c       ; begin == 0 -> skip the whole block
//   ...
//   0xcd2ad4  sub  x23, x21, #0x30
//   0xcd2ae4  bl   _ZdlPv               ; stride 0x30 release walk
//
// The first `cbz` is the guest's own "an empty container has nothing to
// release" test. It is the ONLY thing standing between a normal return and a
// release loop whose exit is `cmp x20, x23` -- pure pointer equality, no modulo
// guard. Measured on target: (end - begin) = 0x25A77C, and 0x25A77C mod 0x30 =
// 0x1C, so the exit is unreachable and the loop never ends.
//
// The member vector at Manager+0x30 IS fully initialised at 0xcd1080/0xcd108c
// and all 21 DeviceInfo:: strings go into IT. The three slots above belong to a
// different, never-constructed object.
//
// WHAT THIS REPAIR IS, AND WHAT IT IS NOT
// ---------------------------------------
// It writes 0 into those three stack slots, which restores the one state a
// default-constructed std::vector can legally hold (begin == end == capacity ==
// nullptr) and then lets the guest's own `cbz` take its empty-container branch.
// The guest's destructor is not patched, skipped, or short-circuited: it runs,
// reads a coherent empty vector, and falls through it exactly as it would have
// on Android. Nothing is forged, no integrity value is reproduced, and not one
// byte of the guest image is written. The stack slots belong to a local
// variable of a function that is currently executing on this very thread, so
// they are host-visible scratch the way a local variable's frame always is.
//
// WHY THE REPAIR RUNS HERE AND NOT IN guestViewNoteAndroidLog
// ----------------------------------------------------------
// The guest's last log before the destructor is
//
//   0xcd2a9c  bl 0x109fa18      ; "DeviceInfo::Tier: %s"
//
// and 0x109fa18 is the NDK logging shim, whose body ends in
//
//   0x109fac0  bl 0x1e17ed0     ; __android_log_vprint
//
// 0x1e17ed0 is a guest PLT slot that symbol_resolver.cpp binds to
// &rr3::androidLogVPrint. So androidLogVPrint runs ON THE GUEST'S THREAD, in
// the guest's own call path, with the Manager frame still live below it.
//
// The archived log proves the path is live rather than assumed: run
// 20261006-144356-free-seq carries
//
//   phase=guest-log-any ordinal=686 ... DeviceInfo::Tier: AndroidHigh
//
// and then nothing -- which is exactly "reached 0xcd2a9c, then wedged in the
// destructor".
//
// androidLogVPrint is the anchor rather than guestViewNoteAndroidLog because
// only androidLogVPrint has a frame record AT ENTRY. Both were disassembled in
// the built ELF:
//
//   rr3::androidLogVPrint       a100: stp x29,x30,[sp,#-368]!
//                               a104: mov x29, sp          <- record exists
//                               a198: ldp x29,x30,[sp],#368
//
//   rr3::guestViewNoteAndroidLog 1ebe0: cbz x0, ...         <- NO record here
//                               1ed88: stp x29,x30,[sp,#-128]!
//                               1ed90: mov x29, sp          <- too late
//
// guestViewNoteAndroidLog establishes its frame 0x1a8 bytes INTO the function,
// on a branch. A walk started there would have no frame record under its feet
// and would read whatever the caller's stack held. That is the difference
// between a walk that works and a wild read, so the anchor is androidLogVPrint
// and the caller passes its own x29 in with one `mov`.
//
// THE CHAIN, AND WHY TWO HOPS ARE ENOUGH
// ---------------------------------------
// AAPCS64 makes x29 callee-saved, and a frame record at address F holds
// [F] = the CALLER's frame pointer and [F+8] = the return address INTO the
// caller. So:
//
//   Manager        fp = M        [M]    = M's caller's fp
//                                       [M+8]  = ret to M's caller
//    `bl 0x109fa18   fp = F109   [F109] = M                <-- our target
//                                       [F109+8] = 0xcd2aa0 <-- folds in range
//     `bl __android_log_vprint@plt   (PLT: a branch, no record, chain intact)
//      -> androidLogVPrint   fp = F1  [F1]   = F109
//                                       [F1+8] = ret into 0x109fa18
//
// 0x109fa18 does establish its own frame -- measured, not assumed:
//
//   0x109fa18  sub  sp, sp, #0x180
//   0x109fa1c  stp  x29, x30, [sp, #0x150]
//   0x109fa28  add  x29, sp, #0x150
//   0x109fac0  bl   0x1e17ed0
//   0x109fadc  ldp  x29, x30, [sp, #0x150]
//
// so starting from F1, hop 0 examines F1 (saved LR folds to 0x109fac4-ish, not
// in the function range), hop 1 examines F109, whose saved LR folds to ~0xcd2aa0
// -- inside [0xcd1014, 0xcd3648). Two hops. There is no host frame between the
// anchor and the guest beyond androidLogVPrint's own, which is why this design
// depends on exactly ONE function emitting a frame pointer, and that one is
// verified above against the built ELF rather than against a compiler flag.
//
// THE COORDINATE SYSTEM
// ---------------------
// A guest frame's saved LR is a RUNTIME address: the guest's .text has been
// relocated. Comparing it directly against 0xcd1014..0xcd3648 is comparing two
// coordinate systems, a runtime address is always above 0xcd3648, and the test
// is then structurally false on every single call. This project has already
// shipped that exact bug once (see guest_frame_probe.hpp). So the translation
// comes FIRST and the ELF-space offset is what gets compared.
//
// THE TRANSLATION IS resolveMainImageLoadBias(), NOT THE CENSUS PLAN, and the
// difference is not a preference -- it is the only thing that can run here.
//
// The previous version of this file read the plan through guestFrameCensusPlan().
// That is censusCachedPlan() (pthread_bridge.cpp:4210), whose body is three
// function-local statics with NO lock:
//
//     static GuestStartupPlan cached{}; static uint64_t capturedAtPass = 0;
//     static bool havePlan = false;
//     if (!havePlan || pass < capturedAtPass
//         || pass - capturedAtPass >= kPlanRefreshPasses) {
//         cached = captureGuestStartupPlan();      // <-- in-place assignment
//     }
//     return cached;                                // <-- a reference to `cached`
//
// Three consequences, all of them load-bearing here:
//
//   1. It is NOT read-only. The header that declares it claims "the probe never
//      triggers a capture of its own", and that claim holds only for the thread
//      that already primed the cache. From the guest's thread the FIRST call has
//      havePlan == false and captures, and then every kPlanRefreshPasses (60)
//      passes after that.
//   2. captureGuestStartupPlan() is not free. Its first statement is an
//      unconditional appendGuestBootLine (multi_dso_linker.cpp:2554), which takes
//      a libnx mutex and does an fopen/fprintf/fflush on the SD card. Calling it
//      from here puts that INSIDE a guest log call, on the guest's own thread.
//      recovery_guard.cpp:401 documents that this project already paid for
//      exactly that shape of mistake once: census fsyncs starved the guest's own
//      file I/O and the main thread ended up parked in a kernel sync request.
//   3. `cached` is assigned IN PLACE while every caller holds a reference to it.
//      pthread_bridge.cpp:3879 says so in as many words -- "That cache is
//      documented as touched only by the reporter thread and carries no lock, so
//      calling it from a second thread would be an unsynchronised race on a
//      struct it assigns in place". Doing that from the guest thread while the
//      reporter thread is mid-assignment is a torn read of the image base list.
//
// resolveMainImageLoadBias() has none of these properties. It is a bounds-checked
// read of one already-loaded image's bias (multi_dso_linker.cpp:2871): no lock,
// no log line, no fsync, no allocation, no capture, and it returns 0 when the
// image is not loaded -- which is the fail-open sentinel the walk needs anyway.
//
// IT IS THE SAME NUMBER AS `g_renderTarget - 0x12B3614`, PROVABLY. findExport
// computes an export's runtime address as `image.loadBias() + symbol.value`
// (multi_dso_linker.cpp:678) and 0x12B3614 IS onViewRenderJNI's st_value, so
//
//     g_renderTarget - 0x12B3614 == loadBias() == resolveMainImageLoadBias()
//
// with no hidden term. That identity is not taken on faith: in archived run
// 20261006-144356-free-seq the log carries both derivations and they agree to the
// last digit --
//
//     render=0x2107c0c614  ->  render - 0x12B3614 = 0x2106959000
//     pc=0x210762bad0 pc_elfva=0xcd2ad0  ->  pc - elfva = 0x2106959000
//
// The second line is the census's own independent fold of a sampled guest PC, so
// the two are derived from unrelated code paths and still agree. It also proves
// minimumAddress == 0 for this image, which is why the plan's base
// (executableStart() == loadBias() + minimumAddress) and the bias are the same
// number and why the ELF offsets compare directly.
//
// FAILURE MODE OF THE BIAS, stated plainly: it goes stale if libRealRacing3.so is
// ever unloaded and reloaded at a different address while the repair is still
// unsettled. Then a saved LR folds against the old bias and lands outside
// [kGuestFrameFnLo, kGuestFrameFnHi) -- the walk finds nothing, which is the
// no-write outcome, not a wrong write. See guestTeardownAcceptTarget for why the
// write itself cannot be aimed at a bad address even then.
//
// (The fold itself lives below, after <cstdint>, because the header's includes
// sit after this comment block.)
//
// FAIL-OPEN
// ---------
// This sits on the guest's log path. It must not be able to make anything
// worse, so: no std::mutex (a lock here can deadlock the thread we are trying to
// save), no allocation, no syscall, no exception, no fsync (appendGuestBootLine
// keeps its handle open and only fflushes), no recursion back into the guest,
// and a hard line budget. Every read is of an address that was validated before
// it was dereferenced, so the only failure surface is a read through a bad
// pointer and every test below exists to make that impossible rather than
// unlikely.
//
// The three ways this can decline to act, and what each one leaves behind:
//
//   translation unavailable (bias == 0)
//       No walk, no log write beyond one budgeted line, no counter movement, no
//       latch. The repair stays armed and re-tries on the next guest log line.
//   chain broken (no record folds into the constructor)
//       The walk stops on a named reason, the guest is not touched, and the
//       attempt is charged -- bounded, and the latch only fires once the budget
//       is genuinely spent on walks that had a precondition.
//   target frame rejected (any of the five accept() checks fails)
//       Nothing is read or written through it. One budgeted line names which
//       check failed.
//
// In all three the guest's own destructor still runs and still takes its own
// decision on its own bytes. Nothing here can leave the guest worse than it was
// without the repair.

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_frame_probe.hpp"

#include <cstdint>

namespace rr3 {

// Fallback upper bound for the main image's ELF address span, used only when
// resolveMainImageSpanBytes() cannot report the real one. It is the loader's own
// per-image limit (multi_dso_linker.cpp:58), so no genuine guest address can fold
// past it; it is simply looser than the ~33 MB the real span actually is.
inline constexpr uint64_t kTeardownMainImageMaxElfBytes = 128ull * 1024ull * 1024ull;

// PT_LOAD vaddrs are page-aligned and svcMapMemory returns a page-aligned base, so
// loadBias() = executableLinkBase - minimumAddress is ALWAYS page-aligned. Every
// value this project has measured agrees (0x2106959000, 0x2105d5900, ...).
//
// This turns "the bias was derived wrong" from a silent whole-run failure into a
// cheap rejection, at the cost of one AND and one compare on the attempt path.
// It is not a substitute for the walk -- a wrong bias can be page-aligned too, and
// MUTATION 4 in the self-test is exactly that case -- but it catches the common
// version, where the bias came from an export's runtime address instead of the
// image base.
inline constexpr uintptr_t kTeardownImagePageSize = 4096;

// runtime -> ELF offset for the MAIN image, via its load bias. Fail-open by
// contract, and by the same rule guestFrameElfOffsetInPlan uses: attribute to the
// image FIRST, subtract SECOND, and on failure return false rather than
// substituting a value.
//
// Never writes 0 for "unknown". That is not pedantry: 0 is below
// kGuestFrameFnLo, so a 0 substituted here would be rejected by the range test
// anyway -- but a 0 substituted into a caller that only tests `ok` would be read
// as "address 0 is in no image", which is a different claim from "we could not
// tell".
inline bool guestTeardownElfOffset(uintptr_t bias, uintptr_t runtimeAddr,
                                   uint64_t* outOff) {
    if (outOff == nullptr || bias == 0 || runtimeAddr == 0) {
        return false;
    }
    // A load bias is always page-aligned. If this one is not, it did not come
    // from the image base -- it is an address inside the image, most likely a
    // resolved export. Reject here rather than fold every address against it.
    if ((bias & (kTeardownImagePageSize - 1)) != 0) {
        return false;
    }
    // Below the image. Checked BEFORE the subtraction so the subtraction cannot
    // underflow into a huge offset that would then pass a magnitude test.
    if (runtimeAddr < bias) {
        return false;
    }
    const uint64_t off = static_cast<uint64_t>(runtimeAddr - bias);
    // The host NRO and the guest images share one 64-bit address space and the
    // NRO can sit above the guest image, so the lower bound alone does not
    // attribute the address. This span check is what does.
    //
    // Prefer the image's REAL span (maximumAddress - minimumAddress, ~33 MB
    // here) because kMaximumImageSize (128 MiB) is loose enough that a host
    // address can fold into the target function's offset range, and repairing
    // the wrong frame is exactly what this routine exists to avoid.
    //
    // But resolveMainImageSpanBytes() returns 0 when the span is unknown, and a
    // bound of 0 would reject EVERY address -- which is what happened on the
    // first run with this code: the on-target self-test went 26/26 -> 22/38 and
    // the repair never fired. So a real span tightens the bound and a 0 falls
    // back to the loader's own cap, which is what the previous revision used and
    // which cannot be tighter than reality in a way that matters here.
    const uintptr_t realSpan = resolveMainImageSpanBytes();
    const uint64_t span = realSpan != 0 ? static_cast<uint64_t>(realSpan)
                                        : kTeardownMainImageMaxElfBytes;
    if (off >= span) {
        return false;
    }
    *outOff = off;
    return true;
}

// THE ONLY FOLD THAT IS WORTH WRITING TO.
//
// kGuestFrameFnLo / kGuestFrameFnHi (guest_frame_probe.hpp:101-102) describe the
// WHOLE 0x2634-byte constructor, so a saved LR folding anywhere inside it passes
// the walk. That is the correct test for "is this the Manager frame" and the
// wrong test for "is this the destructor" -- and run 20261009-201304-readonly is
// what proved the difference costs the entire fix:
//
//   58 folds, 26 distinct elf_off values, 0xcd13a0 .. 0xcd2aa0
//
// Every one of them is the same Manager frame, and 25 of the 26 are NOT the
// destructor.
//
// WHY WRITING AT THE WRONG ONE OF THOSE 26 IS WORTHLESS. A capstone scan of all
// 2445 instructions of the function settles it per slot:
//
//   [x29-0x48] begin      2 reads,  0 writes -- both at 0xcd2aa0 and 0xcd2b00
//   [x29-0x40] end        1 read,   1 write  -- both at 0xcd2aa8 and 0xcd2b04
//   [x29-0x38] capacity   0 reads,  0 writes -- never touched anywhere
//
// Every read and the only write live INSIDE the destructor block. Zero the slots
// at 0xcd13a0 and the guest's own code overwrites `end` at 0xcd2b04 with whatever
// the register held, so the store is not merely premature, it is undone. There is
// no constructor read of the vector to disturb either -- begin has zero writes in
// the whole function and capacity is never named -- which is what makes the gate
// free of collateral damage: writing here cannot perturb anything.
//
// WHY NOT JUST WRITE ON EVERY FOLD. Because "1 of 26 folds is the right one" is
// not a guess you can leave in a hot path on a wedged machine, and because the
// 25 wrong writes would each be three stores into a guest frame plus a log line
// for an outcome indistinguishable from a no-op. The gate turns a probabilistic
// argument ("the first fold of the last walk before the wedge is usually the
// destructor") into an equality test on an ELF offset read from the guest's own
// instruction pointer.
//
// 0xcd2aa0 is the `ldur x20, [x29,#-0x48]` that LOADS begin -- i.e. the saved LR
// of the frame whose caller instruction is the `bl 0x109fa18` at 0xcd2a9c, the
// guest's last log before the teardown. Exactly one call site returns there, so
// at most one guest log line per Manager instance can pass this gate.
inline constexpr uint64_t kTeardownDestructOff = 0x000cd2aa0ull;

// The three words to restore to "empty container". Note the third one: the
// existing guest_frame_probe accept() checks [x29-0x60], [x29-0x48] and
// [x29-0x40] but has never had a reason to look at [x29-0x38], so the check
// below is new rather than reused. Reusing a two-slot check for a three-slot
// write is exactly the kind of near-miss that turns into a wild write.
inline constexpr int kTeardownSlotBegin = -0x48;
inline constexpr int kTeardownSlotEnd = -0x40;
inline constexpr int kTeardownSlotCap = -0x38;

// Frame-chain ceiling. The real answer is 2 (see above); 32 is slack, not a
// target. It bounds the walk in the one case where the chain is long: a host
// caller, or a guest function compiled without a frame pointer, where the chain
// would otherwise have to be walked until something stopped ascending.
inline constexpr uint32_t kTeardownMaxDepth = 32;

// Attempts before the repair gives up for the whole process.
//
// It does NOT latch on the first call, because the main image's bias is not
// available until the loader has mapped it, and a one-shot latch would turn that
// transient into a permanent no-op -- the guest would wedge with no repair and no
// way to tell why.
//
// A MISSING BIAS DOES NOT COUNT AS AN ATTEMPT. This is the whole reason the budget
// is separate from the log budget. In run 20261006-153146-teardown-repair the four
// attempts were all spent on plan_available=0, the budget emptied, and
// settled=1 retired the repair permanently -- four walks that never had a
// precondition, producing no information and disabling the fix. A missing
// precondition is not evidence about the guest; charging it as an attempt is how
// a retry loop turns a transient into a permanent failure. When the bias is
// unavailable the caller skips in place, leaves the state word alone, and tries
// again on the next guest log line.
//
// 4 real walks is enough to ride out the loader and still bounds the work at a
// handful per run.
//
// This is also the reason kTeardownMaxDepth is not the only bound: a walk that
// cannot succeed still costs depth reads, and this is what caps how many of
// those there can be. One walk per call, never a loop inside the attempt.
inline constexpr uint32_t kTeardownMaxAttempts = 4;

// Hard line ceiling for the whole run. After it, counters keep advancing and
// one line says how much was dropped, so a quiet log can never be mistaken for
// a repair that never triggered.
inline constexpr uint32_t kTeardownLogBudget = 12;

// Why the walk stopped. Numeric on purpose, so the log says a number.
enum TeardownWalkStop : uint32_t {
    kTeardownStopFound = 0,
    kTeardownStopDepthExhausted = 1,
    kTeardownStopNotAscending = 2,
    kTeardownStopMisaligned = 3,
    kTeardownStopBelowSp = 4,
    kTeardownStopTooFar = 5,
    kTeardownStopAnchorUnusable = 6,
    kTeardownStopTargetRejected = 7,
    // The translation is unavailable, so no walk was attempted. This is NOT a
    // failed attempt and must not consume the attempt budget -- see
    // kTeardownMaxAttempts and guestTeardownRepairAttempt.
    kTeardownStopNoBias = 8,
};

// Why a candidate target frame pointer was refused.
enum TeardownTargetReject : uint32_t {
    kTeardownTargetOk = 0,
    kTeardownTargetNull = 1,
    kTeardownTargetMisaligned = 2,
    kTeardownTargetBelowSp = 3,
    kTeardownTargetTooFar = 4,
    kTeardownTargetWrap = 5,
    kTeardownTargetSlotOutsideFrame = 6,
};

struct TeardownWalk {
    bool found{};          // a frame record's saved LR folded into the function
    uint32_t depth{};      // frame records examined (the hop count)
    uint32_t stop{};       // TeardownWalkStop
    uint32_t targetReject{};   // TeardownTargetReject, when found && rejected
    uintptr_t anchorFp{};  // where the walk started
    uintptr_t sp{};        // our own sp at the walk
    uintptr_t frameFp{};   // the record whose saved LR matched
    uintptr_t savedFp{};   // its [F]   == the Manager's x29
    uintptr_t savedLr{};   // its [F+8]
    uint64_t elfOff{};     // fold(savedLr)
};

// Walks the AArch64 frame chain upward from anchorFp looking for the frame
// record whose saved LR lands inside the Manager constructor.
//
// THE ORDER OF THE TWO TESTS IS THE WHOLE SAFETY ARGUMENT. The "is this the
// frame" test runs FIRST, on each record, and the guards that license the NEXT
// dereference run second. So every address this function dereferences is one
// that was validated as a frame pointer before it was read: anchorFp is the
// caller's own x29, and every later fp is a savedFp that already passed
// non-null, 16-byte alignment, ascending-above-sp and bounded-distance. The
// range test never reads through a pointer it has not already justified.
//
// A fold that fails is NOT an error and is NOT substituted with 0: it means this
// record's saved LR belongs to no loaded guest image, which is the normal case
// for the host frames at the bottom of the chain. The record is simply not the
// one and the walk continues. Substituting 0 there would fold an unmapped
// address onto the bottom of the first image, and 0 is below kGuestFrameFnLo,
// so the mistake would be invisible -- the exact failure this project already
// paid for.
//
// `bias` is a parameter, not a global and not a plan, so that the host self-test
// runs this exact code against synthetic frames with no emulator AND with no
// startup plan in existence. A control that can only run where the bug is cannot
// establish that it would have caught it.
// Defined in source/guest_teardown_repair.cpp, which owns the single-writer log
// handle. Declared here because the walk below is inline in the header and
// cannot reach that anonymous namespace directly.
void guestTeardownHopTrace(uint32_t hop, uintptr_t fp, uintptr_t savedFp,
                           uintptr_t savedLr, bool folded, uint64_t off,
                           bool inRange);

inline void guestTeardownWalkFrames(uintptr_t bias, uintptr_t anchorFp,
                                   uintptr_t sp, uint32_t maxDepth,
                                   TeardownWalk* out) {
    if (out == nullptr) {
        return;
    }    TeardownWalk w{};
    w.anchorFp = anchorFp;
    w.sp = sp;
    w.stop = kTeardownStopAnchorUnusable;

    // The anchor must itself be a usable frame record: the walk dereferences it
    // on the very first iteration, so it gets the same tests the chain members
    // get rather than being trusted because it came from a register.
    if (anchorFp == 0 || (anchorFp & 0xF) != 0) {
        *out = w;
        return;
    }
    uintptr_t fp = anchorFp;
    for (uint32_t hop = 0; hop < maxDepth; ++hop) {
        w.depth = hop + 1;
        const uintptr_t savedFp = *reinterpret_cast<const volatile uintptr_t*>(fp);
        const uintptr_t savedLr = *reinterpret_cast<const volatile uintptr_t*>(fp + 8);

        uint64_t off = 0;
        const bool folded = guestTeardownElfOffset(bias, savedLr, &off);
        const bool inRange = folded && off >= kGuestFrameFnLo && off < kGuestFrameFnHi;

        // PER-HOP TRACE, unconditional, no budget, no filter.
        //
        // Everything outside this function is exhausted: the entry trace proves
        // the attempt is entered, every one of its six exits is instrumented,
        // and none of them ever prints -- which is only consistent with the
        // function not returning. If it is not returning, the only place left is
        // in here. This writes before any branch, so the LAST line in the file
        // names the hop the walk stopped making progress on.
        guestTeardownHopTrace(hop, fp, savedFp, savedLr, folded, off, inRange);

        if (inRange) {
            w.found = true;
            w.stop = kTeardownStopFound;
            w.frameFp = fp;
            w.savedFp = savedFp;
            w.savedLr = savedLr;
            w.elfOff = off;
            *out = w;
            return;
        }

        // Guards that license the NEXT dereference. Not in range, so move up.
        if (savedFp <= fp) {
            // A caller's frame is always above its callee's. Anything else is a
            // loop, a sentinel, or the end of the chain. Stopping here also
            // makes the walk terminate on a cyclic chain.
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

// Accepts a candidate Manager x29 as safe to READ and WRITE.
//
// These are the five checks the fix is not allowed to perform without. All of
// them are properties the real frame provably has (see guest_frame_probe.hpp
// for the measured prologue) and a wild pointer almost certainly does not:
//
//   1. non-zero, and 16-byte aligned (AAPCS64 keeps sp 16-aligned; the guest
//      prologue only adds 0x10 to it);
//   2. above our own sp -- we are executing inside that frame;
//   3. within kGuestFrameMaxDistance of it, so "above us" alone does not admit
//      a wild pointer that merely happens to be mapped;
//   4. sp_final = fp - 0x5e0 is representable and aligned, i.e. no wrap;
//   5. ALL THREE slots land inside [fp-0x5e0, fp) -- the live frame.
//
// Check 5 is what turns "probably safe to write" into "the write is inside a
// frame that is currently on this thread's stack". It is also the one that has to
// cover [x29-0x38], which no pre-existing check in this project covered.
inline uint32_t guestTeardownAcceptTarget(uintptr_t fp, uintptr_t sp) {
    if (fp == 0) {
        return kTeardownTargetNull;
    }
    if ((fp & 0xF) != 0) {
        return kTeardownTargetMisaligned;
    }
    if (fp < sp) {
        return kTeardownTargetBelowSp;
    }
    if (fp - sp > kGuestFrameMaxDistance) {
        return kTeardownTargetTooFar;
    }
    if (fp < kGuestFrameFpMinusSp) {
        return kTeardownTargetWrap;
    }
    const uintptr_t spFinal = fp - kGuestFrameFpMinusSp;
    const int offsets[3] = {kTeardownSlotBegin, kTeardownSlotEnd, kTeardownSlotCap};
    for (int i = 0; i < 3; ++i) {
        const uintptr_t slot = fp + static_cast<uintptr_t>(offsets[i]);
        if (slot < spFinal || slot + sizeof(uint64_t) > fp) {
            return kTeardownTargetSlotOutsideFrame;
        }
    }
    return kTeardownTargetOk;
}

// THE STATE WORD. 0 = idle or retrying, non-zero = never call the slow path
// again. Relaxed, and read exactly once per guest log line.
extern uint32_t g_guestTeardownRepairSettled;

// Attempts made so far, relaxed. Only the slow path touches it.
extern uint32_t g_guestTeardownRepairAttempts;

// The out-of-line attempt. Walks, validates, and either restores the three slots
// or gives up -- never both, never partially. Declared noinline so the hot path
// below stays a load and a compare.
void guestTeardownRepairAttempt(uintptr_t anchorFp) noexcept;

// THE HOT PATH.
//
// Everything above is the safety argument; this is the cost argument. Once
// settled -- repaired, or given up on -- the entire added cost to a guest log
// line is one relaxed load, one compare and a return, with no call at all:
// __atomic_load_n with __ATOMIC_RELAXED on a plain uint32_t compiles to a plain
// LDR, which on this CPU is not even a barrier.
//
// `anchorFp` is the CALLER's frame pointer, which the caller reads out of x29
// after its own prologue has run. It is passed rather than read here because
// reading x29 inside this function would yield THIS function's frame, and
// because inlining it into a caller that has a real frame pointer is what makes
// the whole thing a handful of instructions.
inline void guestTeardownRepairOnLog(uintptr_t anchorFp) noexcept {
    if (__atomic_load_n(&g_guestTeardownRepairSettled, __ATOMIC_RELAXED) != 0u) {
        return;
    }
    guestTeardownRepairAttempt(anchorFp);
}

// Positive control + mutation, run on the host against synthetic frames and on
// target against the real load bias. Returns 1 only if the walk finds the planted
// frame at the expected depth, computes the three slot addresses correctly, and
// REJECTS the mutated walks. See source/guest_teardown_repair.cpp.
int guestTeardownRepairSelfTest();

} // namespace rr3