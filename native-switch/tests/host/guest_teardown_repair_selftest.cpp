// Host-side self-test for the teardown repair's frame-chain walk.
//
// This compiles the SAME header the Switch build includes
// (include/rr3/guest_teardown_repair.hpp) and runs its walk and its target
// acceptance check against a synthetic frame chain laid out in real memory, so
// the fix for the wedged guest is verifiable without an emulator -- and, more to
// the point, so the MUTATION can be run here.
//
// A control that can only run on the one platform where the thing is broken is
// not a control. The synthetic chain reproduces the real layout exactly:
//
//   targetFp        Manager's x29, with a 0x5e0 frame below it and the three
//                   poison words in it
//   frameFp         [frameFp]    = targetFp
//                   [frameFp+8]  = mainBase + 0xcd2a9c     <- in range: the hit
//   loggerFp        [loggerFp]   = frameFp
//                   [loggerFp+8] = mainBase + 0x109fac4    <- folds, NOT in range
//   anchorFp        [anchorFp]   = loggerFp
//                   [anchorFp+8] = a host address           <- fold fails, keep going
//
// so the walk has to do what it does on target: ignore a fold failure, ignore a
// fold that succeeds but lands outside the function, and stop on the one record
// whose return address is inside the constructor.
//
// Build and run (from the repo root):
//   g++ -std=c++20 -Wall -Wextra -O1 -I native-switch/include
//       native-switch/tests/host/guest_teardown_repair_selftest.cpp -o tdrepair
//   ./tdrepair
//
// Exit code 0 means every case below held.

#include "rr3/guest_teardown_repair.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int gFailures = 0;
int gChecks = 0;

void Check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    ++gChecks;
    if (!ok) {
        ++gFailures;
    }
}

// The guest's LOAD BIAS, far above every ELF offset, so a fold that forgets to
// subtract it cannot accidentally land inside the function range.
//
// This is the only translation input the whole test uses. There is no
// GuestStartupPlan anywhere in it, deliberately: the claim under test is that the
// walk locates the target frame from the load bias alone, and a control that
// quietly depended on a plan would be testing a different configuration than the
// one that runs on target.
constexpr uintptr_t kMainBias = 0x2106559000ull;

// Production derives the bias as (g_renderTarget - onViewRenderJNI's ELF VA).
// Reproduced here so the derived-bias control is self-consistent instead of
// asserting a constant.
constexpr uint64_t kRenderElfVa = 0x12B3614ull;
constexpr uint64_t kRenderTarget = kMainBias + kRenderElfVa;

// Two wrong biases, mirroring guest_teardown_repair.cpp:
//
//   (a) the export's runtime address used as the image base -- non-zero, so it
//       passes an availability check, but not page-aligned;
//   (b) 0x20000 off the correct bias -- non-zero, page-aligned, plausible
//       magnitude, same order as every other address. NOTHING cheap rejects (b);
//       only the walk does, and (b) is the one that would ship as "the repair
//       silently never fires".
constexpr uintptr_t kMutatedBiasMissingSubtraction = kRenderTarget;
constexpr uintptr_t kMutatedBiasPlausibleWrong = kMainBias - 0x20000;

// The two call sites the chain replays, out of the guest ELF.
constexpr uint64_t kElfLoggerReturn = 0x109fac4ull;  // ret into 0x109fa18
constexpr uint64_t kElfManagerCall = 0x000cd2a9cull;  // 0xcd2a9c: bl 0x109fa18

// The deliberately broken fold, duplicated here verbatim from
// guest_teardown_repair.cpp so this test exercises the same mistake. Same input,
// forgets to subtract the load bias.
bool MutatedFoldNoBaseSubtraction(uintptr_t bias, uintptr_t runtimeAddr,
                                  uint64_t* outOff) {
    uint64_t ignored = 0;
    if (!rr3::guestTeardownElfOffset(bias, runtimeAddr, &ignored)) {
        return false;
    }
    *outOff = static_cast<uint64_t>(runtimeAddr);
    return true;
}

// The walk wired to the broken fold, so the mutation is a difference in code
// rather than a flag someone can switch off.
void MutatedWalkNoBaseSubtraction(uintptr_t bias, uintptr_t anchorFp,
                                  uintptr_t sp, uint32_t maxDepth,
                                  rr3::TeardownWalk* out) {
    rr3::TeardownWalk w{};
    w.anchorFp = anchorFp;
    w.sp = sp;
    w.stop = rr3::kTeardownStopAnchorUnusable;
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
        if (MutatedFoldNoBaseSubtraction(bias, savedLr, &off)
            && off >= rr3::kGuestFrameFnLo && off < rr3::kGuestFrameFnHi) {
            w.found = true;
            w.stop = rr3::kTeardownStopFound;
            w.frameFp = fp;
            w.savedFp = savedFp;
            w.savedLr = savedLr;
            w.elfOff = off;
            *out = w;
            return;
        }
        if (savedFp <= fp) {
            w.stop = rr3::kTeardownStopNotAscending;
            *out = w;
            return;
        }
        if ((savedFp & 0xF) != 0) {
            w.stop = rr3::kTeardownStopMisaligned;
            *out = w;
            return;
        }
        if (savedFp < sp) {
            w.stop = rr3::kTeardownStopBelowSp;
            *out = w;
            return;
        }
        if (savedFp - sp > rr3::kGuestFrameMaxDistance) {
            w.stop = rr3::kTeardownStopTooFar;
            *out = w;
            return;
        }
        fp = savedFp;
    }
    w.stop = rr3::kTeardownStopDepthExhausted;
    *out = w;
}

constexpr size_t kArenaWords = 1024;

struct Chain {
    alignas(16) uint64_t arena[kArenaWords]{};
    uintptr_t anchorFp = 0;
    uintptr_t loggerFp = 0;
    uintptr_t frameFp = 0;
    uintptr_t targetFp = 0;
};

// Lays the chain out in real memory, target frame at the bottom (the stack grows
// down), anchor at the top, so every hop genuinely ascends.
void BuildChain(Chain& c, uint64_t loggerElf, uint64_t managerElf,
                uint64_t b, uint64_t e, uint64_t cap) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(&c.arena[0]);
    const uintptr_t hi = lo + sizeof(c.arena);
    c.targetFp = (hi - rr3::kGuestFrameFpMinusSp) & ~static_cast<uintptr_t>(0xF);
    c.frameFp = c.targetFp - 0x200;
    c.loggerFp = c.frameFp - 0x200;
    c.anchorFp = c.loggerFp - 0x200;

    *reinterpret_cast<uint64_t*>(c.targetFp + rr3::kTeardownSlotBegin) = b;
    *reinterpret_cast<uint64_t*>(c.targetFp + rr3::kTeardownSlotEnd) = e;
    *reinterpret_cast<uint64_t*>(c.targetFp + rr3::kTeardownSlotCap) = cap;

    *reinterpret_cast<uint64_t*>(c.frameFp) = c.targetFp;
    *reinterpret_cast<uint64_t*>(c.frameFp + 8) = kMainBias + managerElf;

    *reinterpret_cast<uint64_t*>(c.loggerFp) = c.frameFp;
    *reinterpret_cast<uint64_t*>(c.loggerFp + 8) = kMainBias + loggerElf;

    *reinterpret_cast<uint64_t*>(c.anchorFp) = c.loggerFp;
    *reinterpret_cast<uint64_t*>(c.anchorFp + 8) = 0x0000000000401a30ull;
}

}  // namespace

int main() {

    static Chain chain;
    BuildChain(chain, kElfLoggerReturn, kElfManagerCall,
               0x1111111111111111ull, 0x2222222222222222ull,
               0x3333333333333333ull);

    // sp placed so targetFp sits at EXACTLY the distance bound. The bound check
    // has to be genuinely satisfied, not trivially so.
    const uintptr_t sp = chain.targetFp - rr3::kGuestFrameMaxDistance;

    std::printf("chain: anchor=0x%llx logger=0x%llx frame=0x%llx target=0x%llx "
                "sp=0x%llx\n",
                (unsigned long long)chain.anchorFp,
                (unsigned long long)chain.loggerFp,
                (unsigned long long)chain.frameFp,
                (unsigned long long)chain.targetFp, (unsigned long long)sp);

    // ---- 1. POSITIVE ------------------------------------------------------
    rr3::TeardownWalk walk{};
    rr3::guestTeardownWalkFrames(kMainBias, chain.anchorFp, sp, rr3::kTeardownMaxDepth,
                                 &walk);
    std::printf("walk : found=%d depth=%u stop=%u frame=0x%llx target=0x%llx "
                "elf_off=0x%llx\n",
                walk.found ? 1 : 0, walk.depth, walk.stop,
                (unsigned long long)walk.frameFp, (unsigned long long)walk.savedFp,
                (unsigned long long)walk.elfOff);

    // depth counts RECORDS EXAMINED, and the anchor's own record is examined
    // first, so the match lands on the third hop:
    //   1  anchorFp  savedLr = a host address   -> fold fails,  keep going
    //   2  loggerFp  savedLr = 0x109fac4        -> folds, out of range
    //   3  frameFp   savedLr = 0xcd2a9c         -> in range, found
    Check(walk.found, "POSITIVE: finds the record whose saved LR is in Manager");
    Check(walk.depth == 3, "POSITIVE: found at hop 3 (logger hop folds but is out of range)");
    Check(walk.stop == rr3::kTeardownStopFound, "POSITIVE: stop == found");
    Check(walk.savedFp == chain.targetFp, "POSITIVE: recovered fp == planted Manager x29");
    Check(walk.elfOff == kElfManagerCall, "POSITIVE: elf_off == 0xcd2a9c");
    Check(walk.frameFp == chain.frameFp, "POSITIVE: matched record == frameFp");

    // The one that matters for the write: the exact three addresses.
    Check(chain.targetFp + rr3::kTeardownSlotBegin == chain.targetFp - 0x48
              && chain.targetFp + rr3::kTeardownSlotEnd == chain.targetFp - 0x40
              && chain.targetFp + rr3::kTeardownSlotCap == chain.targetFp - 0x38,
          "POSITIVE: slots are exactly [fp-0x48] [fp-0x40] [fp-0x38]");
    Check(*reinterpret_cast<uint64_t*>(chain.targetFp + rr3::kTeardownSlotBegin)
              == 0x1111111111111111ull
              && *reinterpret_cast<uint64_t*>(chain.targetFp + rr3::kTeardownSlotEnd)
                     == 0x2222222222222222ull
              && *reinterpret_cast<uint64_t*>(chain.targetFp + rr3::kTeardownSlotCap)
                     == 0x3333333333333333ull,
          "POSITIVE: the planted poison reads back (a zero-reading control is void)");
    Check(rr3::guestTeardownAcceptTarget(chain.targetFp, sp)
              == rr3::kTeardownTargetOk,
          "POSITIVE: the planted frame is accepted for a write");

    // ---- 2. MUTATION 1: the fold forgets the image base. MUST NOT FIND. ----
    // The bug this project shipped for a whole run. A runtime address came back
    // as itself; every runtime address is above kGuestFrameFnHi; so inrange read
    // 0 on every call and nobody could tell it from "the guest never calls here".
    {
        rr3::TeardownWalk bad{};
        MutatedWalkNoBaseSubtraction(kMainBias, chain.anchorFp, sp,
                                     rr3::kTeardownMaxDepth, &bad);
        std::printf("mut1 : found=%d depth=%u stop=%u (elf range [0x%llx,0x%llx))\n",
                    bad.found ? 1 : 0, bad.depth, bad.stop,
                    (unsigned long long)rr3::kGuestFrameFnLo,
                    (unsigned long long)rr3::kGuestFrameFnHi);
        Check(!bad.found, "MUTATION 1: no-base-subtraction fold finds NOTHING");
        Check(kMainBias > rr3::kGuestFrameFnHi,
              "MUTATION 1: the error IS the forgotten base, and base > FnHi");
        Check(walk.found,
              "MUTATION 1: the REAL fold still finds it, so the fold is the mutation");
    }

    // ---- 3. MUTATION 2: the walk goes one hop short. MUST NOT FIND. --------
    // "Wrong by one step" in the most literal sense: the cap is one hop below
    // the target, so the target record is never examined.
    {
        // One hop short means cap = 2: the frameFp record is never examined.
        // (The first host test of this mutation used cap = 1 and asserted a
        // hop count of 2, and FAILED -- correctly. depth counts records
        // examined and the anchor's own record is one of them, so the match is
        // on hop 3. The control caught a wrong expectation, which is what it is
        // for; the walk itself was right both times.)
        rr3::TeardownWalk bad{};
        rr3::guestTeardownWalkFrames(kMainBias, chain.anchorFp, sp, 2, &bad);
        std::printf("mut2 : depth_cap=2 found=%d depth=%u stop=%u\n",
                    bad.found ? 1 : 0, bad.depth, bad.stop);
        Check(!bad.found, "MUTATION 2: a walk one hop short finds NOTHING");
        Check(bad.stop == rr3::kTeardownStopDepthExhausted,
              "MUTATION 2: it stops as depth_exhausted, not as a crash");
        Check(walk.found && walk.depth == 3,
              "MUTATION 2: the real walk needs exactly one more hop");
    }

    // ---- 4. MUTATION 3: a slot offset shifted by 8 is a DIFFERENT address. --
    // The near-miss a "did it find something" test waves through: still in-frame,
    // wrong word. This is why the positive case compares literal addresses.
    {
        const uintptr_t want = chain.targetFp + rr3::kTeardownSlotBegin;
        const uintptr_t shifted = want + 8;
        Check(shifted != want, "MUTATION 3: +8 is a different address than [fp-0x48]");
        Check(rr3::kTeardownSlotBegin != rr3::kTeardownSlotEnd
                  && rr3::kTeardownSlotEnd != rr3::kTeardownSlotCap
                  && rr3::kTeardownSlotBegin != rr3::kTeardownSlotCap,
              "MUTATION 3: begin/end/cap offsets are pairwise distinct");
    }

    // ---- 5. FAIL-OPEN ------------------------------------------------------
    {
        rr3::TeardownWalk bad{};
        rr3::guestTeardownWalkFrames(kMainBias, 0, sp, rr3::kTeardownMaxDepth, &bad);
        Check(!bad.found && bad.stop == rr3::kTeardownStopAnchorUnusable,
              "FAIL-OPEN: a null anchor is refused, not dereferenced");

        rr3::guestTeardownWalkFrames(kMainBias, chain.anchorFp + 8, sp,
                                     rr3::kTeardownMaxDepth, &bad);
        Check(!bad.found && bad.stop == rr3::kTeardownStopAnchorUnusable,
              "FAIL-OPEN: a misaligned anchor is refused");

        // A host-only chain: every saved LR is a host address, so no fold ever
        // succeeds. The walk must terminate quietly.
        static Chain hostChain;
        BuildChain(hostChain, 0x401a30ull, 0x401a30ull, 1, 2, 3);
        // Overwrite the two guest-runtime return addresses with host ones.
        *reinterpret_cast<uint64_t*>(hostChain.frameFp + 8) = 0x0000000000401a30ull;
        *reinterpret_cast<uint64_t*>(hostChain.loggerFp + 8) = 0x0000000000401a30ull;
        const uintptr_t hostSp = hostChain.targetFp - rr3::kGuestFrameMaxDistance;
        rr3::guestTeardownWalkFrames(kMainBias, hostChain.anchorFp, hostSp,
                                     rr3::kTeardownMaxDepth, &bad);
        Check(!bad.found, "FAIL-OPEN: a purely host chain finds nothing");
        Check(bad.stop == rr3::kTeardownStopDepthExhausted
                  || bad.stop == rr3::kTeardownStopNotAscending,
              "FAIL-OPEN: a host chain terminates on a named stop reason");

        // A descending chain stops instead of looping forever. The descent has
        // to be planted on a record the walk has to ADVANCE FROM -- the first
        // version of this test pointed frameFp downward, which is the record
        // that MATCHES, so the walk returned before the guard was ever reached
        // and the control failed. Planting it on loggerFp exercises the guard
        // for real: loggerFp folds but is out of range, so the walk must advance
        // and then meets a downward savedFp.
        static Chain descChain;
        BuildChain(descChain, kElfLoggerReturn, kElfManagerCall, 1, 2, 3);
        *reinterpret_cast<uint64_t*>(descChain.loggerFp) = descChain.anchorFp;
        const uintptr_t descSp = descChain.targetFp - rr3::kGuestFrameMaxDistance;
        rr3::guestTeardownWalkFrames(kMainBias, descChain.anchorFp, descSp,
                                     rr3::kTeardownMaxDepth, &bad);
        std::printf("desc : found=%d depth=%u stop=%u\n", bad.found ? 1 : 0,
                    bad.depth, bad.stop);
        Check(!bad.found && bad.stop == rr3::kTeardownStopNotAscending,
              "FAIL-OPEN: a descending chain stops as not_ascending");

        // Target acceptance refuses each bad shape on its own.
        Check(rr3::guestTeardownAcceptTarget(chain.targetFp, chain.targetFp + 0x10)
                  == rr3::kTeardownTargetBelowSp,
              "FAIL-OPEN: target below sp refused");
        Check(rr3::guestTeardownAcceptTarget(chain.targetFp + 8, sp)
                  == rr3::kTeardownTargetMisaligned,
              "FAIL-OPEN: misaligned target refused");
        Check(rr3::guestTeardownAcceptTarget(chain.targetFp + rr3::kGuestFrameMaxDistance + 0x10,
                                             sp) == rr3::kTeardownTargetTooFar,
              "FAIL-OPEN: target beyond the distance bound refused");
        Check(rr3::guestTeardownAcceptTarget(chain.targetFp, sp)
                  == rr3::kTeardownTargetOk,
              "FAIL-OPEN: a target exactly AT the bound is accepted (guard is >)");
        Check(rr3::guestTeardownAcceptTarget(0x40, 0) != rr3::kTeardownTargetOk,
              "FAIL-OPEN: a low target with no frame below it refused");
        Check(rr3::guestTeardownAcceptTarget(0, sp) == rr3::kTeardownTargetNull,
              "FAIL-OPEN: a null target refused");
    }

    // ---- 6. THE STRUCTURAL ARGUMENT, in one number. -----------------------
    {
        const uint64_t runtime = kMainBias + kElfManagerCall;
        uint64_t good = 0;
        uint64_t bad = 0;
        const bool goodOk = rr3::guestTeardownElfOffset(kMainBias, runtime, &good);
        const bool badOk = MutatedFoldNoBaseSubtraction(kMainBias, runtime, &bad);
        std::printf("fold : runtime=0x%llx real=0x%llx mutated=0x%llx delta=0x%llx\n",
                    (unsigned long long)runtime, (unsigned long long)good,
                    (unsigned long long)bad, (unsigned long long)(bad - good));
        Check(goodOk && good == kElfManagerCall,
              "FOLD: the real fold recovers the planted ELF offset");
        Check(badOk && bad == runtime && bad >= rr3::kGuestFrameFnHi,
              "FOLD: the mutated fold returns the runtime address, which is > FnHi");
        Check(kElfManagerCall >= rr3::kGuestFrameFnLo
                  && kElfManagerCall < rr3::kGuestFrameFnHi,
              "FOLD: 0xcd2a9c is inside the constructor range");
        // The logger return address must fold SUCCESSFULLY and still be
        // REJECTED by the range test. That is the hop the walk has to survive:
        // a filter that only asked "did the fold work?" would stop here and
        // report a frame that is 0x44 KiB below the one it wanted.
        //
        // (outOff cannot be null here: guestTeardownElfOffset returns false
        // for a null outOff on purpose, so "did it fold?" has to be asked with a
        // real destination.)
        uint64_t loggerOff = 0;
        const bool loggerFolds =
            rr3::guestTeardownElfOffset(kMainBias, kMainBias + kElfLoggerReturn,
                                           &loggerOff);
        Check(loggerFolds && loggerOff == kElfLoggerReturn,
              "FOLD: the logger return address folds to 0x109fac4");
        Check(!(kElfLoggerReturn >= rr3::kGuestFrameFnLo
                 && kElfLoggerReturn < rr3::kGuestFrameFnHi),
              "FOLD: 0x109fac4 is OUTSIDE the constructor range, so hop 2 is skipped");
        Check(walk.frameFp != chain.loggerFp,
              "FOLD: the walk did NOT stop on the logger hop");
    }

    // ---- 7. POSITIVE WITH NO PLAN: the bias alone. -----------------------
    // The bias is built the way production builds it -- (g_renderTarget -
    // onViewRenderJNI's ELF VA) -- and the whole walk is re-run with that
    // derived value. This is the acceptance case the brief asks for: locate the
    // target frame with the load bias and no GuestStartupPlan anywhere.
    {
        const uintptr_t derivedBias =
            static_cast<uintptr_t>(kRenderTarget) - kRenderElfVa;
        std::printf("bias : render_target=0x%llx - elf_va=0x%llx = 0x%llx\n",
                    (unsigned long long)kRenderTarget,
                    (unsigned long long)kRenderElfVa,
                    (unsigned long long)derivedBias);
        Check(derivedBias == kMainBias,
              "BIAS: derived bias == the bias the chain was planted against");

        rr3::TeardownWalk derived{};
        rr3::guestTeardownWalkFrames(derivedBias, chain.anchorFp, sp,
                                     rr3::kTeardownMaxDepth, &derived);
        Check(derived.found,
              "BIAS: the DERIVED bias alone locates the target frame");
        Check(derived.depth == 3, "BIAS: derived bias finds it at hop 3");
        Check(derived.savedFp == chain.targetFp && derived.frameFp == chain.frameFp,
              "BIAS: derived bias recovers the same fp and record");
        Check(derived.elfOff == kElfManagerCall,
              "BIAS: derived bias recovers the ELF offset 0xcd2a9c");
        Check(rr3::guestTeardownAcceptTarget(derived.savedFp, sp)
                  == rr3::kTeardownTargetOk,
              "BIAS: the frame found via the derived bias is writable-legal");
    }

    // ---- 8. MUTATION 4: the bias is wrong. MUST NOT FIND. ----------------
    // Two wrong biases, because one is easy and one is the dangerous one.
    {
        // (a) An export's runtime address used as the image base. Non-zero, so
        // it passes any availability check -- and not page-aligned, so the fold
        // rejects it outright.
        rr3::TeardownWalk bad{};
        rr3::guestTeardownWalkFrames(kMutatedBiasMissingSubtraction,
                                     chain.anchorFp, sp, rr3::kTeardownMaxDepth,
                                     &bad);
        std::printf("mut4a: bias=0x%llx found=%d depth=%u stop=%u\n",
                    (unsigned long long)kMutatedBiasMissingSubtraction,
                    bad.found ? 1 : 0, bad.depth, bad.stop);
        Check(!bad.found,
              "MUTATION 4a: a bias short by one subtraction finds NOTHING");
        Check(kMutatedBiasMissingSubtraction != 0
                  && (kMutatedBiasMissingSubtraction & 0xFFF) != 0,
              "MUTATION 4a: the wrong bias is non-zero but NOT page-aligned "
              "(the alignment guard is what rejects it)");
        uint64_t wrongA = 0;
        Check(!rr3::guestTeardownElfOffset(
                  kMutatedBiasMissingSubtraction,
                  static_cast<uintptr_t>(kMainBias + kElfManagerCall), &wrongA),
              "MUTATION 4a: the fold declines rather than folding against it");

        // (b) 0x20000 off the correct bias: non-zero, page-aligned, plausible
        // magnitude, same order as every other address in the process. NOTHING
        // cheap rejects this one -- only the walk can, and it is the case that
        // would ship as "the repair silently never fires".
        rr3::TeardownWalk bad2{};
        rr3::guestTeardownWalkFrames(kMutatedBiasPlausibleWrong, chain.anchorFp,
                                     sp, rr3::kTeardownMaxDepth, &bad2);
        std::printf("mut4b: bias=0x%llx found=%d depth=%u stop=%u\n",
                    (unsigned long long)kMutatedBiasPlausibleWrong,
                    bad2.found ? 1 : 0, bad2.depth, bad2.stop);
        Check(!bad2.found,
              "MUTATION 4b: a page-aligned bias that is 0x20000 wrong finds NOTHING");
        Check(kMutatedBiasPlausibleWrong != 0
                  && (kMutatedBiasPlausibleWrong & 0xFFF) == 0
                  && kMutatedBiasPlausibleWrong > 0x10000000ull,
              "MUTATION 4b: the wrong bias passes EVERY cheap guard "
              "(non-zero, page-aligned, plausible magnitude)");
        uint64_t wrongB = 0;
        const bool wrongBOk = rr3::guestTeardownElfOffset(
            kMutatedBiasPlausibleWrong,
            static_cast<uintptr_t>(kMainBias + kElfManagerCall), &wrongB);
        std::printf("mut4b: off=0x%llx ok=%d wanted [0x%llx,0x%llx)\n",
                    (unsigned long long)wrongB, wrongBOk ? 1 : 0,
                    (unsigned long long)rr3::kGuestFrameFnLo,
                    (unsigned long long)rr3::kGuestFrameFnHi);
        Check(wrongBOk && wrongB != kElfManagerCall
                  && !(wrongB >= rr3::kGuestFrameFnLo
                       && wrongB < rr3::kGuestFrameFnHi),
              "MUTATION 4b: the wrong bias folds the real call site OUT of range");
        Check(walk.found && walk.depth == 3,
              "MUTATION 4: the REAL bias still finds it (so mutation 4 is the bias)");
    }

    std::printf("\n%s (%d/%d checks, %d failure%s)\n",
                gFailures == 0 ? "ALL PASS" : "FAILURES", gChecks - gFailures,
                gChecks, gFailures, gFailures == 1 ? "" : "s");
    return gFailures == 0 ? 0 : 1;
}