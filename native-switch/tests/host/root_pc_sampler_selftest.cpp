// Host-side self-test for the root-pc-sampler decision and line formats.
//
// This compiles the SAME header the Switch build includes
// (include/rr3/root_pc_sampler.hpp) and runs it on the host, so the miss path
// is exercised without an emulator. That is the point: a sampler that only
// prints on a hit cannot be shown to work by running it until it hits, and the
// archived runs prove the interesting case is the silent one -- in
// 20261006-022123-devinfo-install the root thread's pc is a host NRO address for
// ordinal 0 targets 1..59 (~40 s of wall clock) and the census emitted ZERO
// census-deinfo-bounds lines across exactly that window.
//
// Build and run:
//   g++ -std=c++20 -Wall -Wextra -O1 -I native-switch/include
//       native-switch/tests/host/root_pc_sampler_selftest.cpp -o rps
//   ./rps
//
// Exit code 0 means every case below held.

#include "rr3/root_pc_sampler.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int gFailures = 0;

void Check(bool ok, const char* what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++gFailures;
    }
}

bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    using namespace rr3::rootpc;

    char buf[512];

    // ---- 1. The range test, including both boundaries. -------------------
    // `<=` on the high end is deliberate and inherited from the census: a `<`
    // here would classify kFdeHi itself differently from the pass it must agree
    // with.
    Check(!InFdeRange(kFdeLo - 1), "range rejects kFdeLo-1 (0xcd1013)");
    Check(InFdeRange(kFdeLo), "range accepts kFdeLo (0xcd1014)");
    Check(InFdeRange(0xcd2abc), "range accepts the wedge pc (0xcd2abc)");
    Check(InFdeRange(0xcd2ad0), "range accepts the wedge pc (0xcd2ad0)");
    Check(InFdeRange(kFdeHi), "range accepts kFdeHi (inclusive, as the census)");
    Check(!InFdeRange(kFdeHi + 1), "range rejects kFdeHi+1 (0xcd3649)");

    // ---- 2. NEGATIVE CONTROL: the miss path prints. ----------------------
    // pc_off taken from an archived PROVABLY out-of-range root-thread sample:
    // 20261006-022123-devinfo-install, census-pc ordinal=0 target=9, pc
    // 0x805f0480, pc_img=host-nro, pc_elfva=host+0x546480 -- a host NRO
    // address, categorically not inside [0xcd1014, 0xcd3648).
    const uint64_t kArchivedHostPcOff = 0x546480ULL;
    Check(!InFdeRange(kArchivedHostPcOff), "archived host pc_off is out of range");
    FormatOutOfRange(buf, sizeof(buf), kArchivedHostPcOff);
    std::printf("     negative-control line: %s\n", buf);
    Check(Contains(buf, "OUT-OF-RANGE"), "OUT-OF-RANGE text emitted off-device");
    Check(Contains(buf, "pc_off=0x546480"), "OUT-OF-RANGE carries the offset");

    // The other miss shape: pc not in a guest image at all. The census is
    // silent here (its block sits behind `if (pcGuest)`); the sampler is not.
    FormatNotGuest(buf, sizeof(buf), "host-nro", 0x805f0480ULL,
                   kArchivedHostPcOff);
    std::printf("     not-guest line:       %s\n", buf);
    Check(Contains(buf, "NOT-GUEST-IMAGE"), "NOT-GUEST-IMAGE text emitted");
    Check(Contains(buf, "pc_img=host-nro"),
          "NOT-GUEST-IMAGE names the image the pc was attributed to");

    // ---- 3. The in-range line, on the archived B/E. ----------------------
    // fp and B/E are the real values from
    // 20261006-022123-devinfo-install, so this line is directly comparable with
    // the archived census-deinfo-bounds line below it.
    const uint64_t kFp = 0x10822fc4a0ULL;
    const uint64_t kB = 0x8061d9dcULL;
    const uint64_t kE = 0x80877c58ULL;
    const uint64_t kSpFromBrief = 0x10830fbec0ULL;
    FormatInRange(buf, sizeof(buf), kFp, kSpFromBrief, 0xcd2abcULL, kB, kE);
    std::printf("     in-range line:        %s\n", buf);

    // The B/E half of the line must be BYTE-IDENTICAL to the archived one. The
    // whole line cannot be: the current build inserts `sp=`, `fp_minus_sp=` and
    // `expect_0x5e0=` between fp and B, which is the new information this
    // sampler exists to add. Everything from B onwards is the census's text,
    // unchanged, and that is the half that has to match.
    const char* kArchivedTail =
        "B=0x8061d9dc E=0x80877c58 span=0x25a27c(2466428) "
        "E_ge_B=1 mod48=0 verdict=SPAN_NOT_MULTIPLE_OF_48 pc_off=0xcd2abc";
    Check(Contains(buf, kArchivedTail),
          "in-range B/E half is byte-identical to the archived line's");
    Check(Contains(buf, "fp=0x10822fc4a0 sp="),
          "in-range line leads with the census's fp= then the new sp=");
    Check(Contains(buf, "expect_0x5e0="), "in-range line carries expect_0x5e0");

    // The span arithmetic reproduces the archived verdict exactly.
    Check((kE - kB) == 0x25a27cULL && (kE - kB) == 2466428ULL,
          "span arithmetic matches the archived span=0x25a27c(2466428)");

    // `mod48=` is a BOOLEAN FLAG (strideOk), not the remainder -- pthread_bridge
    // passes `strideOk ? 1 : 0`. So the archived mod48=0 means "span is NOT a
    // multiple of 48", which is exactly what verdict=SPAN_NOT_MULTIPLE_OF_48
    // says on the same line. The real remainder is 44. Pinned because the field
    // name invites reading it as the remainder, and reading it that way turns a
    // correct log into an apparent contradiction.
    Check(((kE - kB) % 48ULL) == 44ULL,
          "span % 48 is really 44: mod48=0 is the strideOk flag, not a remainder");
    Check(Contains(buf, "mod48=0 verdict=SPAN_NOT_MULTIPLE_OF_48"),
          "flag 0 and the SPAN_NOT_MULTIPLE_OF_48 verdict agree");

    // ---- 4. expect_0x5e0, and an arithmetic contradiction worth pinning ---
    // With the brief's own two numbers, fp - sp is NEGATIVE (0x10822fc4a0 <
    // 0x10830fbec0), so fp_minus_sp is the wrapped subtraction and
    // expect_0x5e0 is 0. A line claiming fp_minus_sp=0x5e0 with these two
    // values cannot have come from one sample. Pinned here so the check is
    // re-runnable rather than a claim in a message.
    const uint64_t kBriefDelta = kFp - kSpFromBrief;
    std::printf("     brief's fp - sp =     0x%llx (unsigned wrap)\n",
                static_cast<unsigned long long>(kBriefDelta));
    Check(kFp < kSpFromBrief, "brief's fp is BELOW its sp (0x10822fc4a0 < 0x10830fbec0)");
    Check(kBriefDelta != 0x5e0ULL,
          "brief's fp - sp != 0x5e0, so its expect_0x5e0=1 line is not one sample");
    Check(Contains(buf, "verdict=SPAN_NOT_MULTIPLE_OF_48"),
          "a wrapped fp-sp does not change the verdict: the B/E words still decide it");
    Check(Contains(buf, "expect_0x5e0=0"),
          "a wrapped fp-sp yields expect_0x5e0=0, not a wrapped 1");

    // A frame pointer that IS the function's frame reports 1.
    FormatInRange(buf, sizeof(buf), 0x10830fbec0ULL + 0x5e0ULL, 0x10830fbec0ULL,
                  0xcd2abcULL, kB, kE);
    Check(Contains(buf, "fp_minus_sp=0x5e0 expect_0x5e0=1"),
          "expect_0x5e0=1 exactly when fp-sp==0x5e0");

    // ---- 5. The other two in-range verdicts are reachable. ---------------
    FormatInRange(buf, sizeof(buf), kFp, kSpFromBrief, 0xcd2abcULL, kB,
                  kB + 48ULL * 7ULL);
    Check(Contains(buf, "verdict=TERMINATES"), "E-B==48*7 reports TERMINATES");
    FormatInRange(buf, sizeof(buf), kFp, kSpFromBrief, 0xcd2abcULL, kB, kB - 8ULL);
    Check(Contains(buf, "verdict=E_LT_B_NEVER_TERMINATES"),
          "E<B reports E_LT_B_NEVER_TERMINATES");

    // ---- 6. The frame guard. --------------------------------------------
    Check(!FramePlausible(0x4ULL), "guard rejects fp=0x4 (platform sentinel)");
    Check(!FramePlausible(0ULL), "guard rejects fp=0");
    Check(FramePlausible(kFp), "guard accepts a real stack frame pointer");
    FormatGuardTripped(buf, sizeof(buf), 0x4ULL, 0xcd2abcULL);
    Check(Contains(buf, "GUARD-TRIPPED") && Contains(buf, "B/E NOT read"),
          "GUARD-TRIPPED text emitted and says B/E was not read");

    // ---- 7. The achieved-rate arithmetic. -------------------------------
    Check(RateX100(1, 0) == 0ULL, "rate is 0 before the warmup second");
    Check(RateX100(10, 999) == 0ULL, "rate is 0 at 999 ms, not a huge number");
    Check(RateX100(10, 1000) == 1000ULL, "10 ticks in 1 s is exactly 10.00 Hz");
    Check(RateX100(4, 10000) == 40ULL,
          "4 ticks in 10 s is 0.40 Hz -- sub-1 Hz keeps its digits");
    Check(RateX100(1, 2000) == 50ULL, "1 tick in 2 s is 0.50 Hz");

    // ---- 8. The cap line is explicit. -----------------------------------
    FormatCapped(buf, sizeof(buf), "miss", kMissLineBudget, 200);
    std::printf("     capped line:          %s\n", buf);
    Check(Contains(buf, "CAPPED") && Contains(buf, "budget=200") &&
              Contains(buf, "NOT logged"),
          "CAPPED line names the class, the budget and the drop");

    // ---- 9. The two budgets are separate. -------------------------------
    Check(kMissLineBudget != kHitLineBudget,
          "miss and hit budgets are distinct, so misses cannot spend the hits");
    Check(kHitLineBudget > kMissLineBudget,
          "hit budget exceeds miss budget");
    Check(kRequestedHz == 10ULL, "requested rate is 10 Hz");

    std::printf("\n%s (%d failure(s))\n", gFailures == 0 ? "PASS" : "FAIL",
                gFailures);
    return gFailures == 0 ? 0 : 1;
}
