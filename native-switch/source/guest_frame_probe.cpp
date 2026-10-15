// Guest frame reader. See include/rr3/guest_frame_probe.hpp for why this exists
// and what the frame looks like, and source/guest_frame_probe.s for the stub
// that hands us the guest's x29.
//
// FAIL-OPEN IS THE WHOLE DESIGN CONSTRAINT
// ---------------------------------------
// This runs inside the guest's own memcpy, on the thread that is already
// wedged. It must not be able to make anything worse. Concretely, that rules
// out and rules in:
//
//   no std::mutex          -- a lock on this path can deadlock the exact
//                              thread we are trying to diagnose. Every counter
//                              is a relaxed atomic increment instead, which on
//                              this CPU compiles to a load/add/store and never
//                              spins.
//   no syscall             -- the entire point. svcGetThreadContext3 on Eden
//                              returns the reader's own registers, and
//                              svcSetThreadActivity never returns.
//   no fsync per line      -- on a simulated SD card an fsync is a whole-card
//                              sync, and per-line fsync on the guest's I/O path
//                              has already dragged the guest itself to a stop
//                              once. appendGuestBootLine keeps its handle open
//                              and only fflushes; the budget below bounds how
//                              many times we ever go near it.
//   no allocation          -- the log line is a stack buffer and snprintf.
//   -fno-exceptions        -- already the project-wide setting, and nothing in
//                              here may throw.
//
// The failure surface is therefore exactly one thing: reading through a bad
// x29. Every check below exists to make that impossible rather than unlikely,
// and anything that fails a check increments a counter and returns.

#include "rr3/guest_frame_probe.hpp"

#include "rr3/recovery_guard.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

// Relaxed counters. Relaxed is enough and is the point: these are statistics,
// not synchronisation, and an acquire fence here would cost the guest's hot
// path more than the whole probe.
uint32_t g_seen = 0;
uint32_t g_inRange = 0;
uint32_t g_rejected = 0;
uint32_t g_logged = 0;
uint32_t g_writeFail = 0;
uint32_t g_budget = 0;
// Runtime addresses that could not be folded into any loaded guest image, so
// the range test could not even be attempted. This is the counter that makes
// "the plan did not cover this address" a visible, countable state rather than
// something that blends into the same 0 that used to mean everything.
uint32_t g_noFold = 0;

// Set only while guestFrameProbeSelfTest is running on this thread. While it is
// set, the observer records instead of logging, so the self test can never
// spend the run's log budget.
thread_local bool t_selfTest = false;
uintptr_t g_selfTestCallerFp = 0;
uintptr_t g_selfTestCapturedFp = 0;

constexpr const char* kLogPath = "sdmc:/switch/rr3-native-rehost.guest-view.log";
// How many calls say "I am alive" before the log goes back to being quiet. Eight
// is enough to show the shape of the return-address stream; more would just be
// log volume for a shim on a hot libc entry point.
constexpr uint32_t kGuestFrameLivenessLines = 8;

inline void bump(uint32_t& slot) {
    __atomic_add_fetch(&slot, 1U, __ATOMIC_RELAXED);
}

inline uint32_t read(const uint32_t& slot) {
    return __atomic_load_n(&slot, __ATOMIC_RELAXED);
}

// Our own sp, exactly. __builtin_frame_address(0) would also work but implies a
// frame, and this function deliberately has none worth the instruction.
inline uintptr_t currentSp() {
    uintptr_t sp = 0;
    __asm__("mov %0, sp" : "=r"(sp));
    return sp;
}

// Why a candidate was thrown away. Kept as an enum rather than a string so the
// report can print a number and the reader can go look up what it means.
enum Reject : uint32_t {
    kRejectNull = 0,
    kRejectMisaligned,
    kRejectBelowSp,
    kRejectTooFar,
    kRejectFrameWrap,
    kRejectSlotOutsideFrame,
    kRejectCount,
};

// One counter per reason, so "rejected 900" is never a mystery.
uint32_t g_reject[kRejectCount] = {};

// Returns false for anything that is not provably a live frame pointer inside
// [0xcd1014, 0xcd3648)'s frame. Every test here is a property the real frame
// has and a wild pointer almost certainly does not.
bool accept(uintptr_t fp, uintptr_t sp) {
    if (fp == 0) {
        bump(g_reject[kRejectNull]);
        return false;
    }
    // AAPCS64 keeps sp 16-byte aligned at every public interface, and the guest
    // prologue only ever adds 0x10 to it, so a real x29 is 16-byte aligned.
    if ((fp & 0xF) != 0) {
        bump(g_reject[kRejectMisaligned]);
        return false;
    }
    // We are executing INSIDE the caller's frame, so its base is above us.
    // A captured x29 below our own sp is not this function's frame pointer.
    if (fp < sp) {
        bump(g_reject[kRejectBelowSp]);
        return false;
    }
    // Bounded, because "above us" alone would still admit a wild pointer that
    // happens to land in mapped memory. The real distance is the frame plus
    // whatever the call chain between the `bl` and here added, well under a
    // few KiB; 0x20000 is two orders of magnitude of headroom.
    if (fp - sp > kGuestFrameMaxDistance) {
        bump(g_reject[kRejectTooFar]);
        return false;
    }
    // sp_final = x29 - 0x5e0 must be a representable, 16-byte aligned address.
    if (fp < kGuestFrameFpMinusSp || ((fp - kGuestFrameFpMinusSp) & 0xF) != 0) {
        bump(g_reject[kRejectFrameWrap]);
        return false;
    }
    // And the three slots must land inside [sp_final, x29): the live frame.
    // This is the check that makes the read safe rather than merely plausible.
    const uintptr_t spFinal = fp - kGuestFrameFpMinusSp;
    const uintptr_t slots[3] = {
        fp + static_cast<uintptr_t>(kGuestFrameSlotArg),
        fp + static_cast<uintptr_t>(kGuestFrameSlotB),
        fp + static_cast<uintptr_t>(kGuestFrameSlotE),
    };
    for (uintptr_t slot : slots) {
        if (slot < spFinal || slot + sizeof(uint64_t) > fp) {
            bump(g_reject[kRejectSlotOutsideFrame]);
            return false;
        }
    }
    return true;
}

} // namespace

void guestFrameObserve(uintptr_t guestFp, uintptr_t guestLr, uintptr_t arg0,
                       const char* symbol) {
    bump(g_seen);

    // LIVENESS + CUMULATIVE COUNTERS, before anything else can return.
    //
    // Without this, "the shim is never called" and "the shim is called four
    // hundred times and no caller is in range" produce byte-identical logs:
    // silence. A 240-second run came and went with zero `guest-frame` lines and
    // no way to say why.
    //
    // So the first few calls always report, and after that the line reappears at
    // exponentially spaced call counts carrying the running seen/inrange/rejected
    // totals. That is what separates the three cases that used to look alike:
    //   seen grows, inrange stays 0  -> the guest never calls this import from
    //                                  the function under study (a static-site
    //                                  problem, not a probe problem)
    //   inrange grows, rejected grows-> the capture works but the address gates
    //                                  are refusing it
    //   inrange grows, both 0        -> an observation should exist; if none does,
    //                                  the log-card path is broken
    const uint32_t seenNow = read(g_seen);
    const bool milestone = seenNow <= kGuestFrameLivenessLines
        || seenNow == 64u || seenNow == 512u || seenNow == 4096u
        || seenNow == 32768u || seenNow == 262144u;
    if (milestone) {
        // Fold FIRST, then report in_range against the ELF-space offset. The
        // old line printed `in_range` computed from the raw runtime address,
        // which is the field that read 0 forever and told us nothing; both the
        // raw address and the folded one are printed so the two coordinate
        // systems are visible side by side in one line and cannot be confused
        // again. `fold` says whether the translation happened at all, which is
        // what separates "the guest never calls this import from there" from
        // "we could not place the address in any loaded image".
        uint64_t elfOff = 0;
        const bool folded = censusGuestElfOffset(guestLr, &elfOff);
        const bool inRange =
            folded && elfOff >= kGuestFrameFnLo && elfOff < kGuestFrameFnHi;
        char live[320];
        __builtin_snprintf(live, sizeof(live),
            "seen=%u inrange=%u rejected=%u lr=0x%016llx fp=0x%016llx "
            "elf_off=0x%llx fold=%d in_range=%d elf_range=[0x%llx,0x%llx) "
            "sym=%s note=this_line_proves_the_stub_is_live",
            seenNow, read(g_inRange), read(g_rejected),
            (unsigned long long)guestLr, (unsigned long long)guestFp,
            (unsigned long long)elfOff, folded ? 1 : 0, inRange ? 1 : 0,
            (unsigned long long)kGuestFrameFnLo,
            (unsigned long long)kGuestFrameFnHi,
            symbol != nullptr ? symbol : "?");
        (void)appendGuestBootLine(kLogPath, "guest-frame-liveness", 0, 0, live);
    }

    // The self-test capture comes FIRST, before the range filter.
    //
    // It used to sit after it, which made the self-test unable to pass by
    // construction: the self-test caller is a host function, so its return
    // address is a host address, so it could never pass the range test and the
    // assignment was unreachable. The first on-target run duly reported
    // `captured_fp=0 verdict=SELFTEST_FAIL` -- a reading about the TEST's
    // control flow, not about the stub. The liveness lines from that same run
    // carry real guest x29 values, and those are the evidence the stub works.
    //
    // This order is now load-bearing for a second reason: the range test
    // depends on the startup plan, which is not captured yet at boot. Capturing
    // first keeps the self-test independent of the plan entirely, so the stub
    // control still works on a cold boot even though the translation control
    // below honestly reports "plan unavailable" there.
    if (t_selfTest) {
        g_selfTestCapturedFp = guestFp;
        return;
    }

    // The filter, and the only thing that makes this cheap. One range compare
    // rejects the overwhelming majority of guest memcpys before any validation
    // runs, which is what keeps a shim on memcpy affordable at all.
    //
    // THE ORDER IS THE FIX: fold the runtime address into the guest ELF's own
    // address space, THEN compare. Comparing guestLr directly against
    // [0xcd1014, 0xcd3648) compared two different coordinate systems and could
    // never match -- a runtime address is always above 0xcd3648 -- so inrange
    // was structurally 0 and this return was taken on every single call. That
    // is why inrange=0 carried no information at all and must not be read as
    // evidence about the guest.
    //
    // Fail-open, and it has to be: the fold returns false for an address that
    // is in no loaded image, and that is counted separately rather than
    // guessed at. Substituting 0 for "unknown" here would fold an unmapped
    // address onto the very bottom of the first image, and 0 is below
    // kGuestFrameFnLo, so the error would have been invisible.
    uint64_t elfOff = 0;
    if (!censusGuestElfOffset(guestLr, &elfOff)) {
        bump(g_noFold);
        return;
    }
    if (elfOff < kGuestFrameFnLo || elfOff >= kGuestFrameFnHi) {
        return;
    }
    bump(g_inRange);

    const uintptr_t sp = currentSp();
    if (!accept(guestFp, sp)) {
        bump(g_rejected);
        return;
    }

    GuestFrameSample sample{};
    sample.lr = guestLr;
    sample.elfOff = elfOff;
    sample.fp = guestFp;
    sample.sp = sp;
    sample.fpsp = guestFp - sp;
    sample.arg60 = *reinterpret_cast<const uint64_t*>(guestFp + kGuestFrameSlotArg);
    sample.b = *reinterpret_cast<const uint64_t*>(guestFp + kGuestFrameSlotB);
    sample.e = *reinterpret_cast<const uint64_t*>(guestFp + kGuestFrameSlotE);
    sample.a0 = arg0;

    // Hard budget. Past it the counters still advance but the card is never
    // touched again, and one summary line says how much was dropped so a
    // truncated log can never be mistaken for a frame that never came.
    const uint32_t ordinal = __atomic_fetch_add(&g_budget, 1U, __ATOMIC_RELAXED);
    if (ordinal >= kGuestFrameLogBudget) {
        if (ordinal == kGuestFrameLogBudget) {
            char summary[192];
            const int n = std::snprintf(
                summary, sizeof(summary),
                "frame-v1 cap=%u seen=%u inrange=%u rejected=%u nofold=%u "
                "null=%u misalign=%u below_sp=%u too_far=%u wrap=%u "
                "outside=%u reason=budget_spent",
                kGuestFrameLogBudget, read(g_seen), read(g_inRange), read(g_rejected),
                read(g_noFold),
                read(g_reject[kRejectNull]), read(g_reject[kRejectMisaligned]),
                read(g_reject[kRejectBelowSp]), read(g_reject[kRejectTooFar]),
                read(g_reject[kRejectFrameWrap]),
                read(g_reject[kRejectSlotOutsideFrame]));
            if (n > 0 && static_cast<size_t>(n) < sizeof(summary)) {
                bump(g_writeFail);
                (void)appendGuestBootLine(kLogPath, "guest-frame-drop",
                                          static_cast<unsigned long long>(ordinal), 0,
                                          summary);
            }
        }
        return;
    }

    // Every field the probe computed is in this line. A field that is computed
    // but not printed is a field that was not recorded, which is the specific
    // mistake this file is not allowed to repeat.
    //
    // elf_off is the one this filter is now built on, so it is printed beside
    // the raw lr: if a future run matches again, the log shows the ELF address
    // that justified the match, and not merely that a match happened.
    char details[352];
    const int n = std::snprintf(
        details, sizeof(details),
        "frame-v1 sym=%s lr=%016llx elf_off=%llx fp=%016llx sp=%016llx "
        "fpsp=%llx arg60=%016llx b=%016llx e=%016llx dst=%016llx "
        "b_minus_e=%016llx dst_eq_b=%d seen=%u",
        symbol != nullptr ? symbol : "?", (unsigned long long)sample.lr,
        (unsigned long long)sample.elfOff,
        (unsigned long long)sample.fp, (unsigned long long)sample.sp,
        (unsigned long long)sample.fpsp, (unsigned long long)sample.arg60,
        (unsigned long long)sample.b, (unsigned long long)sample.e,
        (unsigned long long)sample.a0,
        (unsigned long long)(sample.b - sample.e),
        sample.a0 == sample.b ? 1 : 0, read(g_seen));

    if (n <= 0 || static_cast<size_t>(n) >= sizeof(details)) {
        // A truncated format is a lost sample, not a lost run.
        bump(g_writeFail);
        return;
    }
    if (appendGuestBootLine(kLogPath, "guest-frame",
                            static_cast<unsigned long long>(ordinal), sample.b,
                            details)) {
        bump(g_logged);
    } else {
        bump(g_writeFail);
    }
}

uint32_t guestFrameSeen() { return read(g_seen); }
uint32_t guestFrameInRange() { return read(g_inRange); }
uint32_t guestFrameRejected() { return read(g_rejected); }
uint32_t guestFrameLogged() { return read(g_logged); }
uint32_t guestFrameWriteFail() { return read(g_writeFail); }

uintptr_t guestFrameSelfTestCallerFp() { return g_selfTestCallerFp; }
uintptr_t guestFrameSelfTestCapturedFp() { return g_selfTestCapturedFp; }

// ---------------------------------------------------------------------------
// POSITIVE CONTROL
//
// A control that cannot fail is worse than no control, so this one is built to
// be broken on purpose. The caller below is an ordinary noinline C function
// with volatile locals, so the compiler is forced to give it a real frame and a
// real x29. It calls the assembly stub; the stub's first act is to save the
// incoming x29; the body hands that value to guestFrameObserve, which records
// it while t_selfTest is set. The assertion is then the whole point of the
// exercise: the x29 the stub captured must be EXACTLY this function's x29, not
// the stub's, not the body's, and not zero.
//
// If the stub were changed to capture x28, or to capture x29 after its own
// prologue had already written it, the two numbers would differ and this
// returns 0.
// ---------------------------------------------------------------------------
extern "C" void* guestViewTracedMemcpy(void* destination, const void* source,
                                       size_t length);

extern "C" __attribute__((noinline))
void* guestFrameSelfTestTarget(void* destination, const void* source, size_t length) {
    // Volatile so the frame is real and cannot be optimised into a leaf with no
    // stack at all, which is the failure mode a naive version of this test has.
    volatile char scratch[64];
    scratch[0] = 1;
    scratch[sizeof(scratch) - 1] = 2;
    // Read x29 DIRECTLY, after the prologue has run.
    //
    // This is the second bug this self test found, and it was in the TEST, not
    // in the stub. __builtin_frame_address(0) is specified to return the
    // function's CFA, not its frame-pointer register, and on AArch64 those
    // differ by exactly this function's frame size. The archived reading was
    // caller_fp=0x1082afff50 against captured_fp=0x1082afffa0 -- a difference
    // of precisely 0x50, the frame this function establishes. So the reference
    // value was the CFA, the stub's number was x29, and "the stub did not
    // capture the caller's frame pointer" was a statement about the
    // reference, not about the capture.
    //
    // Reading x29 after the prologue makes the two the same value BY
    // CONSTRUCTION: the prologue has done `add x29, sp, #0x50`, so x29 now
    // holds this function's frame pointer, which is exactly what the stub saw
    // in the incoming x29 before it saved it. The `volatile` keeps the read
    // from being hoisted above the prologue, and the surrounding volatile
    // locals keep the frame from being optimised away entirely.
    uintptr_t fp = 0;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    g_selfTestCallerFp = fp;
    (void)scratch[0];
    return guestViewTracedMemcpy(destination, source, length);
}

int guestFrameProbeSelfTest() {
    g_selfTestCallerFp = 0;
    g_selfTestCapturedFp = 0;
    t_selfTest = true;
    char src[64];
    char dst[64];
    for (size_t i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<char>(i);
        dst[i] = 0;
    }
    guestFrameSelfTestTarget(dst, src, sizeof(src));
    t_selfTest = false;
    // Both must be non-zero (a zero x29 means no frame was established at all,
    // which is itself a failure, not a pass) and they must be equal.
    const int pass =
        (g_selfTestCallerFp != 0 && g_selfTestCallerFp == g_selfTestCapturedFp) ? 1 : 0;
    // Say it here, not only through the boot console. Every `bootPrintf` line
    // is absent from the Eden archives -- the archive captures the guest-view log
    // and the Eden log, and the boot console output is in neither -- so a
    // self-test that only prints there is a self-test whose result nobody
    // offline can ever see. This writes the verdict into the archived log,
    // including the raw pair, so "the stub captured the caller's x29 on real
    // aarch64" is a checkable claim and not a hope.
    char verdict[256];
    __builtin_snprintf(verdict, sizeof(verdict),
        "pass=%d expected=1 caller_fp=0x%016llx captured_fp=0x%016llx "
        "match=%d verdict=%s",
        pass, (unsigned long long)g_selfTestCallerFp,
        (unsigned long long)g_selfTestCapturedFp,
        (g_selfTestCallerFp != 0 && g_selfTestCallerFp == g_selfTestCapturedFp)
            ? 1 : 0,
        pass ? "SELFTEST_PASS" : "SELFTEST_FAIL");
    (void)appendGuestBootLine(kLogPath, "guest-frame-selftest", pass ? 0u : 1u, 0,
                              verdict);
    return pass;
}

// ---------------------------------------------------------------------------
// THE TRANSLATION CONTROL, AND WHY IT IS BUILT TO FAIL
//
// The stub self test above proves the stub hands us the caller's x29. It says
// nothing about the runtime -> ELF fold, and the fold is the thing that was
// rejecting 100% of samples while every counter still read "the stub is
// alive". A control that only covers the part that works is how that bug
// survived.
//
// The positive case is deliberately SELF-CONSISTENT rather than transcribed. It
// walks the live plan, and for each image it builds a runtime address from that
// image's OWN base and a chosen offset inside it, then asserts the fold
// recovers exactly that offset. It cannot pass by agreeing with a number
// mistyped out of a log line, and it does not depend on a particular run's
// rebasing -- the bases move between runs, the invariant does not.
//
// The mutation case is what makes the positive case mean anything. The same
// assertion is re-run through a fold that is wrong in the specific way the old
// filter was wrong -- it never subtracts the base, so a runtime address comes
// back as itself. If that broken fold were to PASS, the control would be
// measuring nothing at all, and this function returns 0 to say so rather than
// reporting a green tick it has not earned. A control that cannot fail is
// worse than no control, because it buys the confidence of a check that is not
// running.
//
// The host build (tests/host/guest_frame_probe_selftest.cpp) runs this same
// logic against synthetic plans, so the mutation result is verifiable without
// an emulator, and the target run re-runs it against the real plan.
// ---------------------------------------------------------------------------
namespace {

GuestFrameTranslationReport g_translation{};

// The deliberately broken fold used by the mutation case: it takes the same
// plan and the same inputs, and it forgets to subtract the image base. Kept as
// its own function so the mutation is visible as a DIFFERENCE in code rather
// than a flag someone can switch off.
bool mutatedFoldNoBaseSubtraction(const GuestStartupPlan& plan,
                                  uintptr_t runtimeAddr, uint64_t* outOff) {
    uint64_t ignored = 0;
    if (!guestFrameElfOffsetInPlan(plan, runtimeAddr, &ignored)) {
        return false;
    }
    // "Folded" it into the runtime address it was given. The offset is wrong by
    // exactly the image base, which is ~0x2106559000 for the main image.
    *outOff = static_cast<uint64_t>(runtimeAddr);
    return true;
}

} // namespace

const GuestFrameTranslationReport& guestFrameTranslationReport() {
    return g_translation;
}

int guestFrameTranslationSelfTest() {
    g_translation = GuestFrameTranslationReport{};
    const GuestStartupPlan& plan = guestFrameCensusPlan();
    g_translation.planAvailable = plan.available && plan.imageCount != 0;

    const uint64_t count = plan.imageCount < kGuestStartupMaximumImages
        ? plan.imageCount : kGuestStartupMaximumImages;
    for (uint64_t i = 0; i < count; ++i) {
        const GuestStartupImage& img = plan.images[i];
        if (img.base == 0 || img.imageBytes == 0) {
            continue;
        }
        ++g_translation.imagesTested;
        if (g_translation.imagesTested == 1) {
            g_translation.firstBase = img.base;
            g_translation.firstSpan = img.imageBytes;
        }
        // Three probe points per image: the first byte, the middle (where a
        // real call site lives), and the last byte. First and last are the
        // boundary cases -- an off-by-one in the containment test passes the
        // middle and fails these.
        const uint64_t span = img.imageBytes;
        const uint64_t probes[3] = {0, span / 2, span - 1};
        for (uint64_t p = 0; p < 3; ++p) {
            const uint64_t want = probes[p];
            const uint64_t runtime = img.base + want;

            uint64_t got = 0;
            if (guestFrameElfOffsetInPlan(plan, runtime, &got) && got == want) {
                ++g_translation.positivePass;
            }
            else {
                ++g_translation.positiveFail;
            }
            if (g_translation.imagesTested == 1 && p == 1) {
                g_translation.firstElfOff = want;
                g_translation.firstRuntime = runtime;
                g_translation.firstFolded = got;
            }

            // And the mutation, on the same inputs. It MUST fail.
            uint64_t bad = 0;
            const bool badOk = mutatedFoldNoBaseSubtraction(plan, runtime, &bad);
            if (badOk && bad == want) {
                g_translation.mutationAccepted = true;
            }
            if (g_translation.imagesTested == 1 && p == 1) {
                g_translation.mutatedFolded = bad;
            }
        }
    }
    // The mutated fold was caught on every case where it produced an answer
    // that should have been wrong, and it never produced a right one.
    g_translation.mutationRejected =
        !g_translation.mutationAccepted && g_translation.imagesTested != 0;

    const int pass =
        (g_translation.planAvailable && g_translation.imagesTested != 0
         && g_translation.positiveFail == 0
         && g_translation.positivePass == g_translation.imagesTested * 3
         && g_translation.mutationRejected) ? 1 : 0;

    // Every number goes in the line, including the mutated fold's own answer.
    // A control whose result is a bare 0/1 cannot be argued with afterwards,
    // and this project has been bitten by a field that was computed and then
    // not printed.
    char verdict[384];
    __builtin_snprintf(verdict, sizeof(verdict),
        "pass=%d expected=1 plan=%d images=%u pos_pass=%u pos_fail=%u "
        "probe_elf=0x%llx probe_rt=0x%llx folded=0x%llx mutated=0x%llx "
        "mut_rejected=%d mut_accepted=%d verdict=%s",
        pass, g_translation.planAvailable ? 1 : 0, g_translation.imagesTested,
        g_translation.positivePass, g_translation.positiveFail,
        (unsigned long long)g_translation.firstElfOff,
        (unsigned long long)g_translation.firstRuntime,
        (unsigned long long)g_translation.firstFolded,
        (unsigned long long)g_translation.mutatedFolded,
        g_translation.mutationRejected ? 1 : 0,
        g_translation.mutationAccepted ? 1 : 0,
        pass ? "TRANSLATION_PASS" : "TRANSLATION_FAIL");
    (void)appendGuestBootLine(kLogPath, "guest-frame-translation", pass ? 0u : 1u,
                              0, verdict);
    return pass;
}

} // namespace rr3
