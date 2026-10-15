#pragma once

// Guest frame reader: the caller's ARM64 frame pointer, captured on the guest's
// own thread, in the guest's own context, with no kernel call of any kind.
//
// WHY A SEPARATE PROBE INSTEAD OF ANOTHER ATTEMPT AT THE WEDGE
// -----------------------------------------------------------
// The two routes to the teardown loop's two cursor words are both closed, and
// this is the note explaining what is left:
//
//   host cross-thread sampling needs svcSetThreadActivity + svcGetThreadContext3.
//   On Eden the first never returns, and the second hands back the READER's
//   own registers, so it cannot name the guest thread's PC at all.
//
//   patching the guest's .text needs svcSetMemoryPermission to return an
//   executable page. It never does. The ladder bottoms out at Perm_Rw and the X
//   bit is gone permanently, so a patched page is a page the guest can no
//   longer run.
//
// What is left is the ABI. AAPCS64 makes x29 callee-saved, so the guest's own
// frame pointer is live in x29 at every one of the 291 call sites inside
// [0xcd1014, 0xcd3648) -- and disassembling that whole function shows x29 is
// written exactly once (0xcd1030) and restored exactly once (0xcd3450), so
// there is no window in the middle where it could be anything else.
//
// The assembly stub in source/guest_frame_probe.s captures that x29 before its
// own prologue overwrites it, and hands it here. See that file for the layout.
//
// THE FRAME, AS MEASURED
// ----------------------
// From the guest ELF, not from a brief:
//
//   0xcd1014  str  d8, [sp, #-0x70]!      sp_entry = sp_in - 0x70
//   0xcd1018  stp  x29, x30, [sp, #0x10]
//   0xcd1030  add  x29, sp, #0x10         x29 = sp_entry + 0x10 = sp_in - 0x60
//   0xcd1034  sub  sp, sp, #0x5d0         sp_final = x29 - 0x5e0
//   0xcd3450  ldp  x29, x30, [sp, #0x10]
//
// which reproduces both measured observations exactly: x29 = STACK0 - 0x60 and
// fp - sp = 0x5e0. Every slot this probe reads is inside [sp_final, x29), so
// every read is of a live frame that the guest is currently standing in.
//
// The three slots, and what they are for:
//
//   [x29-0x60]  14 accesses in-range. The candidate argument the root-cause
//               hypothesis is betting on.
//   [x29-0x48]   2 accesses: read at 0xcd2aa0, read again at 0xcd2b00. This is
//               the vector begin, B, and the thing free()/operator delete is
//               handed at 0xcd2b08.
//   [x29-0x40]   2 accesses: read at 0xcd2aa8, stored at 0xcd2b04. This is the
//               vector end, E, and the value that assignment installs AFTER the
//               delete has already been handed B.
//
// THE COORDINATE SYSTEM, AND THE BUG THAT ATE THE WHOLE OBSERVATION ROUTE
// ---------------------------------------------------------------------
// The function range above is in the guest ELF's OWN address space. The stub
// hands this probe the guest's x30, which is a RUNTIME address. Those are
// different coordinate systems, and the version of this filter that compared
// them directly was comparing 0xcd1014..0xcd3648 against 0x2107...:
//
//   if (guestLr < kGuestFrameFnLo || guestLr >= kGuestFrameFnHi) return;
//
// A runtime address is always far above 0xcd3648, so `in_range` was
// STRUCTURALLY 0 on every call, `inrange=0` on every milestone, and the
// filter could never admit anything. The three counters were then read as
// "the guest never calls this import from the function under study", which is
// a statement about the GUEST, and it was in fact a statement about the
// filter. The stub was working the whole time -- the archived run
// 20261006-051443-frame-count carries 32768 captures with real guest x29.
//
// So the filter now runs the other way round: fold the runtime address into
// the ELF's space FIRST, then compare. The fold is not a second opinion about
// where the guest images are loaded -- it reads the bases out of the census's
// own GuestStartupPlan, the same structure censusImageOffsetOf reads, and it
// does the same attribution-then-subtraction in the same order. Proven on
// target by the pairing in the archives:
//
//   sampler-image idx=3 soname=libRealRacing3.so base=0x2106559000
//   census-pc  ... pc=0x210782babc pc_img=libRealRacing3.so pc_elfva=0xcd2abc
//
// 0x210782babc - 0xcd2abc is that run's base, so `address - base` IS the ELF
// virtual address and it feeds straight into the range constants and into a
// disassembler.
//
// guestFrameElfOffsetInPlan is inline and takes the plan as an argument on
// purpose: that is what lets the positive control and the mutation test run
// on the HOST, against this exact code, with no emulator (see
// tests/host/guest_frame_probe_selftest.cpp). A control that can only run on
// the one platform where the thing is broken is not a control.

#include "rr3/elf_preflight.hpp"

#include <cstddef>
#include <cstdint>

namespace rr3 {

// The guest function whose frame this probe reads, as ELF virtual addresses.
// The filter is exact and closed at both ends: the return address the guest
// left in x30 must land inside it.
inline constexpr uint64_t kGuestFrameFnLo = 0x000cd1014ull;
inline constexpr uint64_t kGuestFrameFnHi = 0x000cd3648ull;

// x29 - sp_final, straight out of `sub sp, sp, #0x5d0` plus the 0x10 the
// prologue adds. Used only to sanity-check the captured x29, never to trust it.
inline constexpr uintptr_t kGuestFrameFpMinusSp = 0x5e0;

// How far above our own sp the caller's x29 is allowed to be. We are executing
// INSIDE that function's frame, so x29 is above us by construction; a captured
// value further away than this is not a frame pointer, and reading through it
// is exactly the wild-address read this project has already paid for twice.
inline constexpr uintptr_t kGuestFrameMaxDistance = 0x20000;

// Slot offsets from the captured x29.
inline constexpr int kGuestFrameSlotArg = -0x60;
inline constexpr int kGuestFrameSlotB = -0x48;
inline constexpr int kGuestFrameSlotE = -0x40;

// Hard ceiling on log lines for the whole run. The probe is a diagnosis aid, not
// a telemetry stream: 16 lines is more than enough to see the values settle, and
// the counters below keep counting after the cap so a truncated log still says
// how much was dropped. This is the only thing standing between the probe and
// the SD card, so it is a compile-time constant and not a runtime knob.
inline constexpr uint32_t kGuestFrameLogBudget = 16;

// One accepted reading of the teardown function's frame.
struct GuestFrameSample {
    uint64_t lr;      // guest return address (x30 on entry to the stub)
    uint64_t elfOff;  // lr folded into the guest ELF's own address space
    uint64_t fp;      // the captured guest x29
    uint64_t sp;      // our own sp at the moment of the capture
    uint64_t fpsp;    // fp - sp, the frame-size cross-check
    uint64_t arg60;   // [x29-0x60]
    uint64_t b;       // [x29-0x48]
    uint64_t e;       // [x29-0x40]
    uint64_t a0;      // the import's first argument (memcpy's destination)
};

// Folds a RUNTIME address into the guest ELF's own address space, using the
// image bases the census's startup plan already maintains.
//
// Returns false -- never a guess, never a 0 substituted for "unknown" -- when
// the address is not inside any loaded guest image, when the plan is not
// available, or when the image's span is degenerate. A caller that gets false
// must skip the sample and count it, because a wrong offset here would be
// compared against kGuestFrameFnLo/Hi and would silently invent a match.
//
// This mirrors censusImageOffsetOf's rule and order exactly: attribute to an
// image FIRST, subtract SECOND. A single rule, so the probe and the census
// cannot disagree about where an image starts.
inline bool guestFrameElfOffsetInPlan(const GuestStartupPlan& plan,
                                      uintptr_t runtimeAddr,
                                      uint64_t* outOff) {
    if (outOff == nullptr || runtimeAddr == 0 || !plan.available) {
        return false;
    }
    const uint64_t count = plan.imageCount < kGuestStartupMaximumImages
        ? plan.imageCount : kGuestStartupMaximumImages;
    for (uint64_t i = 0; i < count; ++i) {
        const GuestStartupImage& img = plan.images[i];
        const uintptr_t base = static_cast<uintptr_t>(img.base);
        const uintptr_t span = static_cast<uintptr_t>(img.imageBytes);
        if (base == 0 || span == 0 || runtimeAddr < base
            || runtimeAddr - base >= span) {
            continue;
        }
        *outOff = static_cast<uint64_t>(runtimeAddr - base);
        return true;
    }
    return false;
}

// The live plan, as the census caches it. The probe reads the SAME cached
// instance the census reporter pass primes, so the probe never triggers a
// capture of its own: captureGuestStartupPlan appends an fsync'd log line on
// every call, and that cost belongs to the reporter thread only.
const GuestStartupPlan& guestFrameCensusPlan();

// runtime -> ELF offset, via the cached census plan. Fail-open by contract:
// see guestFrameElfOffsetInPlan.
inline bool censusGuestElfOffset(uintptr_t runtimeAddr, uint64_t* outOff) {
    return guestFrameElfOffsetInPlan(guestFrameCensusPlan(), runtimeAddr, outOff);
}

// Called from guestViewTracedMemcpyBody on every guest memcpy, before any of
// the real work. Fails open in every direction: a return address outside the
// function, a frame pointer that fails any of the checks below, or a log write
// that fails all end in a counter increment and a return. No allocation, no
// lock, no syscall, no exception, no recursion back into the guest.
void guestFrameObserve(uintptr_t guestFp, uintptr_t guestLr, uintptr_t arg0,
                       const char* symbol);

// Counters, for the periodic report. Relaxed atomics only: this runs on the
// guest's hot path and must never block a thread.
uint32_t guestFrameSeen();       // times the stub was entered at all
uint32_t guestFrameInRange();    // return address landed inside the function
uint32_t guestFrameRejected();   // in range, but the x29 failed a check
uint32_t guestFrameLogged();     // lines actually written
uint32_t guestFrameWriteFail();  // lines the log refused to take

// Positive control. Calls the assembly stub from a C function that has its own
// frame pointer and returns 1 only if the x29 the stub captured is exactly that
// function's x29. Returns 0 on any mismatch, so a control that cannot fail
// would show up here immediately.
int guestFrameProbeSelfTest();

// The value the self test compares against, for reporting.
uintptr_t guestFrameSelfTestCallerFp();
uintptr_t guestFrameSelfTestCapturedFp();

// ---------------------------------------------------------------------------
// THE TRANSLATION CONTROL
//
// guestFrameProbeSelfTest proves the STUB captures the caller's x29. It says
// nothing about the runtime->ELF fold, which is the thing that was silently
// rejecting every sample. So the fold gets its own control, with the property
// that makes it worth having: it is built to FAIL.
//
// Positive: for every image in the live plan, take a known ELF offset inside
// that image, add the image's base to get the runtime address the stub would
// really have handed us, and assert the fold returns that same offset. This is
// self-consistent -- it uses the plan's own numbers rather than a number
// transcribed from a log -- so it cannot pass by agreeing with a typo.
//
// Mutation: the same assertion is then re-run through a deliberately broken
// fold (one that forgets to add, or subtracts, the base). It MUST fail. If the
// mutated fold ever passed, the positive control above would be measuring
// nothing, and this function would return 0 to say so.
//
// Returns 1 only when the plan is available, every positive case holds, and
// the mutated control fails. Returns 0 when the plan is not captured yet --
// the self test runs at boot, before the census's frames==0 capture -- and
// says so in the log rather than reporting a pass it did not earn.
// ---------------------------------------------------------------------------
int guestFrameTranslationSelfTest();

// The translation control's verdict and the numbers behind it, for the log.
struct GuestFrameTranslationReport {
    bool planAvailable{};
    uint32_t imagesTested{};
    uint32_t positivePass{};
    uint32_t positiveFail{};
    bool mutationRejected{};   // the broken fold failed, as it must
    bool mutationAccepted{};   // the broken fold PASSED: the control is void
    uint64_t firstBase{};
    uint64_t firstSpan{};
    uint64_t firstElfOff{};
    uint64_t firstRuntime{};
    uint64_t firstFolded{};
    uint64_t mutatedFolded{};
};
const GuestFrameTranslationReport& guestFrameTranslationReport();

} // namespace rr3
