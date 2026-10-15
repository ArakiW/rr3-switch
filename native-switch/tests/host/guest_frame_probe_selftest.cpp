// Host-side self-test for the guest-frame probe's runtime -> ELF translation.
//
// This compiles the SAME header the Switch build includes
// (include/rr3/guest_frame_probe.hpp) and runs its translation control on the
// host, so the fix for the coordinate-system bug is verifiable without an
// emulator -- and, more to the point, so the MUTATION can be run here. The
// mutation is the whole value of the control: a fold that forgets to subtract
// the image base is exactly what shipped for a whole run, and on the guest it
// can only be observed as "inrange=0", which is indistinguishable from the
// guest simply never calling the import. Here it is a red line that must
// appear, and it does.
//
// Build and run:
//   g++ -std=c++20 -Wall -Wextra -O1 -I native-switch/include
//       native-switch/tests/host/guest_frame_probe_selftest.cpp -o gfps
//   ./gfps
//
// Exit code 0 means every case below held.

#include "rr3/guest_frame_probe.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int gFailures = 0;

void Check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++gFailures;
    }
}

// One loaded guest image, shaped like the real plan's entries.
rr3::GuestStartupImage MakeImage(uint64_t base, uint64_t bytes, const char* soname) {
    rr3::GuestStartupImage img{};
    img.base = base;
    img.imageBytes = bytes;
    std::snprintf(img.soname, sizeof(img.soname), "%s", soname);
    return img;
}

// THE BASE IS NOT A CONSTANT, AND THAT IS THE POINT.
//
// Each run rebases the main image somewhere different, and the three archived
// runs below disagree by megabytes:
//
//   20261006-043152-mask-fix       implied base 0x2106b59000
//   20261006-050852-frame-stub     implied base 0x2105d59000
//   20261006-051443-frame-count   sampler-image base=0x2106559000
//
// This is why no hardcoded bias can be written down anywhere, and why the old
// filter could never be rescued by adjusting a constant: it was comparing
// across two coordinate systems. So this file NEVER assumes a base. The
// measured pairings below each carry their own run's base, and the
// self-consistent control derives every address from the plan under test.
constexpr uint64_t kMainImageBytes = 0x1fc8000ull;

rr3::GuestStartupPlan MakeRealisticPlan() {
    rr3::GuestStartupPlan plan{};
    plan.available = true;
    plan.resolutionPlanHash = 0x5eed;
    plan.imageCount = 4;
    plan.images[0] = MakeImage(0x21086b0000ull, 0x101000ull, "libc++_shared.so");
    plan.images[1] = MakeImage(0x2108522000ull, 0x3000ull, "libNimble.so");
    plan.images[2] = MakeImage(0x2108526000ull, 0x189000ull, "libfmodex.so");
    plan.images[3] = MakeImage(0x2106559000ull, kMainImageBytes, "libRealRacing3.so");
    return plan;
}

// A single-image plan for one run, carrying that run's own main-image base.
rr3::GuestStartupPlan MakePlanForRun(uint64_t mainBase) {
    rr3::GuestStartupPlan plan{};
    plan.available = true;
    plan.imageCount = 1;
    plan.images[0] = MakeImage(mainBase, kMainImageBytes, "libRealRacing3.so");
    return plan;
}

// The deliberately broken fold, duplicated here verbatim from
// guest_frame_probe.cpp so this test exercises the same mistake.
bool MutatedFoldNoBaseSubtraction(const rr3::GuestStartupPlan& plan,
                                  uintptr_t runtimeAddr, uint64_t* outOff) {
    uint64_t ignored = 0;
    if (!rr3::guestFrameElfOffsetInPlan(plan, runtimeAddr, &ignored)) {
        return false;
    }
    *outOff = static_cast<uint64_t>(runtimeAddr);
    return true;
}

}  // namespace

int main() {
    // ---- 1. The positive control, per image, at three probe points. --------
    // Self-consistent by construction: the expected offset is used to BUILD the
    // runtime address, so this cannot pass by agreeing with a transcribed
    // constant.
    const rr3::GuestStartupPlan plan = MakeRealisticPlan();
    uint32_t pass = 0;
    uint32_t fail = 0;
    uint32_t images = 0;
    for (uint64_t i = 0; i < plan.imageCount && i < rr3::kGuestStartupMaximumImages; ++i) {
        const rr3::GuestStartupImage& img = plan.images[i];
        if (img.base == 0 || img.imageBytes == 0) {
            continue;
        }
        ++images;
        const uint64_t span = img.imageBytes;
        const uint64_t probes[3] = {0, span / 2, span - 1};
        for (uint64_t p = 0; p < 3; ++p) {
            uint64_t got = 0;
            const uint64_t runtime = img.base + probes[p];
            if (rr3::guestFrameElfOffsetInPlan(plan, runtime, &got)
                && got == probes[p]) {
                ++pass;
            }
            else {
                ++fail;
            }
        }
    }
    std::printf("     images=%u pos_pass=%u pos_fail=%u\n", images, pass, fail);
    Check(images == 4, "all four archived images are exercised");
    Check(fail == 0 && pass == images * 3, "every image folds back to its own offset");

    // ---- 2. THE MEASURED PAIRINGS, each with its OWN run's base. -----------
    // These are real readings from the archives. The base is not assumed: each
    // pairing is checked against a plan carrying the base its own run reported,
    // so this still proves "runtime - base == elfva" without ever writing a
    // bias down as if it were a constant.
    struct Measured {
        const char* what;
        uint64_t runtime;
        uint64_t elfva;
        uint64_t base;      // the base that run reported
    };
    const Measured measured[] = {
        // 20261006-043152-mask-fix, census-pc line:
        //   pc=0x210782babc pc_img=libRealRacing3.so pc_elfva=0xcd2abc
        {"043152 census-pc pc", 0x210782babcull, 0xcd2abcull, 0x2106b59000ull},
        // 20261006-050852-frame-stub, definfo-install line:
        //   site=0x2106a2baa0 for the ELF site 0xcd2aa0
        {"050852 definfo site", 0x2106a2baa0ull, 0xcd2aa0ull, 0x2105d59000ull},
        // 20261006-051443-frame-count, the base it reported itself:
        //   sampler-image idx=3 soname=libRealRacing3.so base=0x2106559000
        {"051443 sampler base", 0x2106559000ull + 0xcd2abcull, 0xcd2abcull,
         0x2106559000ull},
    };
    for (const Measured& m : measured) {
        const rr3::GuestStartupPlan runPlan = MakePlanForRun(m.base);
        uint64_t off = 0;
        const bool ok =
            rr3::guestFrameElfOffsetInPlan(runPlan, m.runtime, &off)
            && off == m.elfva;
        std::printf("     %-24s 0x%llx (base 0x%llx) -> 0x%llx, want 0x%llx\n",
                    m.what, (unsigned long long)m.runtime,
                    (unsigned long long)m.base, (unsigned long long)off,
                    (unsigned long long)m.elfva);
        Check(ok, "measured runtime address folds to its own archived elfva");
    }
    {
        // The site the deinfo probe names, folded in its own run's space.
        const rr3::GuestStartupPlan run050852 = MakePlanForRun(0x2105d59000ull);
        uint64_t off = 0;
        const bool ok =
            rr3::guestFrameElfOffsetInPlan(run050852, 0x2106a2baa0ull, &off);
        Check(ok && off >= rr3::kGuestFrameFnLo && off < rr3::kGuestFrameFnHi,
              "the deinfo-install site folds INSIDE the function range");
    }

    // ---- 3. THE MUTATION, which must FAIL. --------------------------------
    // The old filter's mistake, reproduced: no base subtraction, so the fold
    // hands back the runtime address itself.
    {
        uint32_t mutationAccepted = 0;
        uint32_t mutationRejected = 0;
        uint64_t exampleRuntime = 0;
        uint64_t exampleGood = 0;
        uint64_t exampleBad = 0;
        uint64_t exampleBase = 0;
        for (uint64_t i = 0; i < plan.imageCount && i < rr3::kGuestStartupMaximumImages; ++i) {
            const rr3::GuestStartupImage& img = plan.images[i];
            if (img.base == 0 || img.imageBytes == 0) {
                continue;
            }
            const uint64_t probes[3] = {0, img.imageBytes / 2, img.imageBytes - 1};
            for (uint64_t p = 0; p < 3; ++p) {
                const uint64_t runtime = img.base + probes[p];
                uint64_t good = 0;
                uint64_t bad = 0;
                const bool goodOk =
                    rr3::guestFrameElfOffsetInPlan(plan, runtime, &good);
                const bool badOk = MutatedFoldNoBaseSubtraction(plan, runtime, &bad);
                if (badOk && bad == probes[p]) {
                    ++mutationAccepted;   // the control is VOID
                }
                else {
                    ++mutationRejected;
                }
                if (i == 0 && p == 1) {
                    exampleRuntime = runtime;
                    exampleGood = goodOk ? good : 0;
                    exampleBad = badOk ? bad : 0;
                    exampleBase = img.base;
                }
            }
        }
        std::printf("     example runtime=0x%llx good=0x%llx mutated=0x%llx "
                    "(delta=0x%llx, image base=0x%llx)\n",
                    (unsigned long long)exampleRuntime,
                    (unsigned long long)exampleGood,
                    (unsigned long long)exampleBad,
                    (unsigned long long)(exampleBad - exampleGood),
                    (unsigned long long)exampleBase);
        Check(mutationAccepted == 0,
              "MUTATION: fold without base subtraction is REJECTED by the control");
        Check(mutationRejected == images * 3,
              "MUTATION: every one of the 12 probe points failed under mutation");
        // The error is EXACTLY the image base it forgot to subtract, which for
        // the main image is a 0x2106xxxxxx runtime address. That is the whole
        // bug in one number: the offset came back as a runtime address, and a
        // runtime address is always above kGuestFrameFnHi, so inrange read 0
        // on every single call and nobody could tell it from "no call site".
        Check(exampleBad - exampleGood == exampleBase
              && exampleBase > rr3::kGuestFrameFnHi,
              "MUTATION: error == the forgotten image base, and that is > FnHi");
        // And this is the structural argument for why the old filter could never
        // have matched: the mutated value is a runtime address, every runtime
        // address is above kGuestFrameFnHi.
        const bool structurallyDead = exampleBad >= rr3::kGuestFrameFnHi;
        std::printf("     mutated=0x%llx vs kGuestFrameFnHi=0x%llx -> "
                    "in_range_would_be=%d\n",
                    (unsigned long long)exampleBad,
                    (unsigned long long)rr3::kGuestFrameFnHi,
                    structurallyDead ? 0 : 1);
        Check(structurallyDead,
              "old filter was structurally 0: untranslated runtime addr is always > Hi");
    }

    // ---- 4. Fail-open: no guessing, no 0 substituted for unknown. ---------
    {
        uint64_t off = 0xdeadbeefull;
        const bool a = rr3::guestFrameElfOffsetInPlan(plan, 0, &off);
        Check(!a, "address 0 is rejected");

        off = 0xdeadbeefull;
        const bool b = rr3::guestFrameElfOffsetInPlan(plan, 0x7fffffff0000ull, &off);
        Check(!b, "an address in no image is rejected");
        Check(off == 0xdeadbeefull, "a rejected fold does not write 0 into outOff");

        off = 0xdeadbeefull;
        const bool c = rr3::guestFrameElfOffsetInPlan(plan, 0x2106558000ull, &off);
        Check(!c, "one byte below the image base is rejected (no underflow)");
        Check(off == 0xdeadbeefull, "the underflow case wrote nothing");

        rr3::GuestStartupPlan empty{};
        off = 0xdeadbeefull;
        const bool d = rr3::guestFrameElfOffsetInPlan(empty, 0x210782babcull, &off);
        Check(!d, "an unavailable plan is rejected (fail-open at boot)");

        rr3::GuestStartupPlan zeroSpan = plan;
        zeroSpan.images[3].imageBytes = 0;
        off = 0xdeadbeefull;
        const bool e = rr3::guestFrameElfOffsetInPlan(zeroSpan, 0x210782babcull, &off);
        Check(!e, "a zero-span image is skipped rather than folding to base");
    }

    // ---- 5. Exactly the boundary the range filter uses. -------------------
    {
        uint64_t off = 0;
        const bool lo = rr3::guestFrameElfOffsetInPlan(
            plan, plan.images[3].base + rr3::kGuestFrameFnLo, &off);
        Check(lo && off == rr3::kGuestFrameFnLo, "kGuestFrameFnLo folds to itself");

        off = 0;
        const bool hi = rr3::guestFrameElfOffsetInPlan(
            plan, plan.images[3].base + rr3::kGuestFrameFnHi, &off);
        Check(hi && off == rr3::kGuestFrameFnHi, "kGuestFrameFnHi folds to itself");

        // The guest's own wedge address 0xcd2abc, which is what the census saw
        // as the frozen PC, must land inside the function.
        const uint64_t wedge = 0xcd2abcull;
        Check(wedge >= rr3::kGuestFrameFnLo && wedge < rr3::kGuestFrameFnHi,
              "the observed guest wedge PC 0xcd2abc is inside the function range");
    }

    std::printf("\n%s (%d failure%s)\n", gFailures == 0 ? "ALL PASS" : "FAILURES",
                gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}
