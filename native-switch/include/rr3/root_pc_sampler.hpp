// Root-thread PC sampler: the decision half.
//
// This is split in two on purpose. The half that touches the guest -- pause the
// root thread, read its context, resume it, read B and E off its stack -- lives
// in pthread_bridge.cpp and needs libnx. Everything that decides what a sample
// MEANS, and turns it into the bytes that land on the SD card, is here with no
// platform dependency at all, so the exact code the port runs can also be
// compiled and EXECUTED on a host (tests/host/root_pc_sampler_selftest.cpp)
// rather than being taken on trust. A probe whose only evidence is "the log had
// no OUT-OF-RANGE lines" is a probe that has proved nothing, and the cheapest
// way to not be that probe is to make the miss path testable off-device.
//
// THE LINE TEXTS BELOW ARE THE CENSUS'S. Every string is a copy of the
// corresponding string in pthread_bridge.cpp censusSampleThreadPcs and the two
// must stay byte-identical, or the sampler's lines and the census's lines stop
// being comparable and the whole point of running both is lost. The census is
// deliberately NOT refactored to call in here: it has to keep its output
// exactly, and this change adds a sampler next to it rather than rewiring the
// pass it already runs.
//
// The one text that has no census counterpart is FormatNotGuest. It exists
// because the census is structurally silent in the exact case a negative
// control needs: see the note on it below.

#ifndef RR3_ROOT_PC_SAMPLER_HPP
#define RR3_ROOT_PC_SAMPLER_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace rr3 {
namespace rootpc {

// ---------------------------------------------------------------------------
// The requested sampling rate.
//
// DELIBERATELY SLOW: 10 Hz on ONE thread is one pause/context/resume round trip
// per 100 ms against the thread that may be the one that is wedged, and it is
// already ~127x more invasive than the census read it supplements (~0.079 Hz,
// measured), so anything above it buys resolution this question cannot use and
// pays for it in frozen guest main-thread time.
// ---------------------------------------------------------------------------
constexpr uint64_t kRequestedHz = 10;

// Log budget, and TWO of them rather than one.
//
// The census emits its per-guest-sample line unbounded, which is harmless at
// 0.08 samples/s and is not harmless at 10 Hz. The split is the part that
// matters: a root thread that spends an entire run outside the function must
// not be able to spend the allowance belonging to the samples that finally
// land inside it. Misses get their own small budget; hits get their own large
// one. Both emit an explicit CAPPED line when they engage -- a silent drop
// would leave "out of range" and "stopped logging" looking identical, which is
// the exact ambiguity this sampler exists to remove.
constexpr uint32_t kMissLineBudget = 200;
constexpr uint32_t kHitLineBudget = 400;

// The DeviceInfo function range, copied from pthread_bridge.cpp:2685-2686
// (kDevInfoFdeLo / kDevInfoFdeHi). The census declares its own pair as
// function-local constants and this change does not touch them, so these two
// pairs are a duplicated fact: if they ever diverge, the sampler's
// OUT-OF-RANGE and the census's are answering different questions about the
// same address. That is the thing to check first if the two paths ever
// disagree.
constexpr uint64_t kFdeLo = 0xcd1014ULL;
constexpr uint64_t kFdeHi = 0xcd3648ULL;

// Frame-pointer plausibility guard, copied from pthread_bridge.cpp:2697. The
// guard is REPORTED rather than silent in both paths: "the probe looked and the
// frame was not a frame" and "the probe never looked" must not read the same.
constexpr uint64_t kFpMin = 0x10000ULL;
constexpr uint64_t kFpMax = 0x0000800000000000ULL;

// The census's range test, verbatim, INCLUDING `<=` on the high end rather than
// `<`. The census has always included kFdeHi itself; a `<` here would classify
// the boundary sample differently from the pass it is supposed to agree with.
constexpr bool InFdeRange(uint64_t pcOff) {
    return pcOff >= kFdeLo && pcOff <= kFdeHi;
}

constexpr bool FramePlausible(uint64_t fp) {
    return fp >= kFpMin && fp <= kFpMax;
}

// ---------------------------------------------------------------------------
// The ACHIEVED rate, in hundredths of a Hz, so 1000 is 10.00 Hz.
//
// Hundredths rather than whole Hz because the expected outcome on this
// platform is a rate BELOW 1 (see the rate constant: the pause round trip is
// the dominant cost and is a scheduler cost, not a tunable), and a whole-Hz
// integer would report that as a flat 0.
//
// 0 until a full second of wall clock has passed. ticks/elapsed over the first
// few milliseconds measures thread startup, not sampling, and publishing that
// as a rate is how a probe ends up advertising 400 Hz on line 2.
// ---------------------------------------------------------------------------
constexpr uint64_t kRateWarmupMs = 1000;

constexpr uint64_t RateX100(uint64_t ticks, uint64_t elapsedMs) {
    return elapsedMs < kRateWarmupMs ? 0ULL
                                     : (ticks * 100000ULL) / elapsedMs;
}

// ---------------------------------------------------------------------------
// Line texts. Census copies are marked; the one addition is marked as such.
// ---------------------------------------------------------------------------

// Prefixed onto every sampler line. Census lines carry no prefix, so stripping
// the first two space-separated fields from a census-deinfo-bounds line and
// from a root-pc-sampler line leaves the same text, and the two can be diffed
// field by field. hz_x100= is the rate this loop has actually achieved, not
// kRequestedHz: that number is what separates "sampled often and was never in
// the function" from "did not sample".
inline int FormatPrefix(char* out, size_t cap, uint64_t tick, uint64_t hzX100) {
    return std::snprintf(out, cap, "hz_x100=%llu tick=%llu",
                         static_cast<unsigned long long>(hzX100),
                         static_cast<unsigned long long>(tick));
}

// Census copy: pthread_bridge.cpp:2691.
inline int FormatOutOfRange(char* out, size_t cap, uint64_t pcOff) {
    return std::snprintf(out, cap, "OUT-OF-RANGE pc_off=0x%llx (B/E not read)",
                         static_cast<unsigned long long>(pcOff));
}

// ADDITION, no census counterpart -- and that absence is the reason this
// sampler exists in its present shape.
//
// The census reports nothing at all when the pc is not inside a guest image: the
// whole census-deinfo-bounds block sits behind `if (pcGuest)`. On Eden the root
// thread's pc is reported as a HOST NRO address for long stretches of a run and
// as the real guest address for others -- 20261006-022123-devinfo-install has 11
// host-nro samples and 10 libRealRacing3.so samples for ordinal 0, and the wedge
// is visible as the switch between them. So the periods in which the root thread
// is PROVABLY not in [kFdeLo, kFdeHi] are exactly the periods the census is
// silent, and a negative control built on the census gate could not produce a
// single line. This variant makes that period loud.
inline int FormatNotGuest(char* out, size_t cap, const char* pcImg, uint64_t pc,
                          uint64_t pcOff) {
    return std::snprintf(out, cap,
                         "NOT-GUEST-IMAGE pc_img=%s pc=0x%llx pc_off=0x%llx "
                         "(B/E not read)",
                         pcImg != nullptr ? pcImg : "unknown",
                         static_cast<unsigned long long>(pc),
                         static_cast<unsigned long long>(pcOff));
}

// Census copy: pthread_bridge.cpp:2698.
inline int FormatGuardTripped(char* out, size_t cap, uint64_t fp, uint64_t pcOff) {
    return std::snprintf(out, cap,
                         "GUARD-TRIPPED fp=0x%llx pc_off=0x%llx "
                         "(frame implausible, B/E NOT read)",
                         static_cast<unsigned long long>(fp),
                         static_cast<unsigned long long>(pcOff));
}

// Census copy: pthread_bridge.cpp:2713, double space after expect_0x5e0=%d
// included. expect_0x5e0 is the number this whole sampler exists to capture:
// it answers whether x29 is this function's frame pointer at all.
//
// 48 is NOT a power of two, so the stride test is a modulo. `(span & 47)` is
// wrong here and writing it would be exactly the kind of near-miss that
// produces a confident wrong answer.
inline int FormatInRange(char* out, size_t cap, uint64_t fp, uint64_t sp,
                         uint64_t pcOff, uint64_t bWord, uint64_t eWord) {
    const bool ordered = eWord >= bWord;
    const uint64_t span = ordered ? (eWord - bWord) : 0;
    const bool strideOk = ordered && (span % 48ULL == 0ULL);
    return std::snprintf(
        out, cap,
        "fp=0x%llx sp=0x%llx fp_minus_sp=0x%llx expect_0x5e0=%d  B=0x%llx "
        "E=0x%llx span=0x%llx(%llu) E_ge_B=%d mod48=%d verdict=%s pc_off=0x%llx",
        static_cast<unsigned long long>(fp),
        static_cast<unsigned long long>(sp),
        static_cast<unsigned long long>(fp - sp),
        (fp - sp) == 0x5e0 ? 1 : 0,
        static_cast<unsigned long long>(bWord),
        static_cast<unsigned long long>(eWord),
        static_cast<unsigned long long>(span),
        static_cast<unsigned long long>(span),
        ordered ? 1 : 0, strideOk ? 1 : 0,
        (!ordered) ? "E_LT_B_NEVER_TERMINATES"
                   : strideOk ? "TERMINATES" : "SPAN_NOT_MULTIPLE_OF_48",
        static_cast<unsigned long long>(pcOff));
}

// ADDITION. Emitted once per class, at the moment the cap engages, so a capped
// run says so in the log instead of simply going quiet.
inline int FormatCapped(char* out, size_t cap, const char* which,
                        uint32_t budget, uint64_t tick) {
    return std::snprintf(out, cap,
                         "CAPPED class=%s budget=%u at_tick=%llu "
                         "(further %s lines are NOT logged)",
                         which != nullptr ? which : "?", budget,
                         static_cast<unsigned long long>(tick), which);
}

}  // namespace rootpc
}  // namespace rr3

#endif  // RR3_ROOT_PC_SAMPLER_HPP
