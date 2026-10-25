//
// THE RACE-METRICS VALUE PROBE.
//
// The race-state probe (source/race_state_probe.cpp) finds the guest's
// RuleSet_RaceMetrics object and reports its member SLOTS, but a slot holds a
// DESCRIPTOR, not a number:
//
//     slot+0x00  a 64-bit member id
//     slot+0x08  the hash of the member's name
//     slot+0x10  len ^ hash
//
// The number lives in the guest's global member VALUE store, a four-table
// hash registry in the guest's .bss. The layout, the key derivation and the
// decode were decoded from the guest's own code and are in
// include/rr3/member_value_store.hpp, with the evidence in the comments
// there.
//
// WHY THIS PROBE DOES NOT CALL THE GUEST
// -------------------------------------
// The guest's own accessor for a member (ELF 0x991c8c) does not merely read:
// it RE-KEYS the member. It looks the value up, mints a new id, re-inserts
// the value under the new id, supersedes the old one and stores the new id
// back into the slot. Two consequences:
//
//   * the 64-bit word in the slot is a ROTATING TOKEN. It is read fresh on
//     every sample here and is NEVER cached, because a cached id maps to
//     nothing after the next read.
//   * calling the accessor would MUTATE the guest's state from a diagnostic
//     hook, so this probe does the lookup itself instead: a pure read, no
//     guest call, no guest state touched.
//
// WHY EVERY READ GOES THROUGH THE PORT'S SAFE READ
// -------------------------------------------------
// A pointer the guest published can point at a page that is no longer there,
// and a diagnostic that can crash the run it is measuring is worse than no
// diagnostic. Every read below goes through raceStateReadWord(), which is the
// race-state probe's own safe read: the plausibility gate, then the
// guest-image check, then svcQueryMemory, then the read.
//
// WHAT THE LINE IS FOR
// --------------------
// The port has to be able to tell a human "you have completed N laps". This
// line is where N comes from, and it carries every raw piece of the lookup
// (the ids, the keys, the chain nodes and the cells) so that a lookup which
// goes wrong can be debugged from the log alone. The raw pieces are on EVERY
// line rather than only on the first one and on a change, because the ids
// rotate: the guest reads the member itself, so "a change" is nearly every
// sample.
//
#include "rr3/race_metrics_probe.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/member_value_store.hpp"
#include "rr3/race_state_probe.hpp"
#include "rr3/recovery_guard.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

// ---- the guest's layout, from the guest's own code ------------------------
//
// The RuleSet_RaceMetrics object is 0xe0 bytes and its vtable address point
// is 0x1e4b378 (the class "19RuleSet_RaceMetrics", through the guest's own
// Itanium RTTI). The vptr is READ, not assumed: the probe's identity check
// has to be a measurement, or it checks nothing.
//
// The member slots are the RuleSet's own, not the modifier's. The telemetry
// function (ELF 0xb35800) reads them through the accessor:
//
//     0xb358cc  add  x0, x19, #0x60 ; m_nTotalLaps
//     0xb358d0  bl   0x991c8c
//     0xb358a0  add  x0, x19, #0xa8 ; m_nTopGameSpeed
//     0xb358a4  bl   0x991c8c
//
// (x19 is the RuleSet_RaceMetrics there, and the modifier lives inside it at
// +0x40, so these are the RuleSet-relative offsets +0x60 and +0xa8.)
constexpr uintptr_t kRuleSetRaceMetricsVptrElfVa = 0x1E4B378ULL;
constexpr uintptr_t kRuleSetTotalLapsOff = 0x60ULL;
constexpr uintptr_t kRuleSetTopSpeedOff = 0xA8ULL;

// The cadence. The guest's own race-state line is written every 30 frames
// (see source/race_state_probe.cpp), and this probe reads about twenty words
// per sample, so the same cadence costs nothing measurable and still samples
// a lap counter that changes once per lap many times over.
constexpr unsigned long long kSamplePeriodFrames = 30ULL;

// The guest's telemetry scale for the top speed (ELF 0xb35874 / 0xb358bc):
// |raw| * 900 * 2^-16, printed as "Top Speed (km/h)". The PHYSICAL unit of
// the raw value is not established, so the raw number is reported too.
constexpr int64_t kTelemetryScale = 900;
constexpr double kTelemetryScaleShift = 65536.0;  // 2^-16

constexpr const char* kGuestViewLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";
// The phase ordinals the race-state probe already uses are 100 (race-state),
// 101 (race-lap) and 102 (race-raw); this one is the next.
constexpr unsigned long long kPhaseOrdinal = 103;
constexpr const char* kPhaseName = "race-metrics";

// ---- the probe's own state ------------------------------------------------
RaceMetricsValueSample g_sample;
unsigned long long g_lastSampleFrame = 0;
bool g_haveSample = false;

// The reader the value store uses. EVERY guest read in this file goes through
// it, and through nothing else.
struct GuestReader {
    bool operator()(uintptr_t address, uint64_t* out) const {
        return raceStateReadWord(address, out);
    }
};

// The guest's own telemetry scale for the top speed.
int32_t telemetrySpeed(int32_t raw) {
    const int64_t magnitude =
        (raw < 0) ? -static_cast<int64_t>(raw) : static_cast<int64_t>(raw);
    const double scaled =
        static_cast<double>(magnitude * kTelemetryScale) / kTelemetryScaleShift;
    return static_cast<int32_t>(scaled);
}

// THE LINE.
//
// ONE line per sample. The fields the port needs come first (the frame, the
// object, the two ids, the two numbers and the verdict), then the raw pieces
// of the lookup, so that a lookup which goes wrong can be debugged from the
// log alone.
//
// A FAILURE SAYS WHY: the verdict is "ok=0 FAILED reason=<why>" and the
// reason names the step that failed, not just that something did.
void logRaceMetrics(unsigned long long frame, const RaceMetricsValueSample& sample,
                    const member_value_store::Lookup* laps,
                    const member_value_store::Lookup* topspeed) {
    char status[160];
    if (sample.ok) {
        std::snprintf(status, sizeof(status), "ok=1");
    } else {
        std::snprintf(status, sizeof(status), "ok=0 FAILED reason=%s",
                      sample.reason != nullptr ? sample.reason : "unknown");
    }

    const uint64_t lapsId = (laps != nullptr) ? laps->id : 0;
    const uint64_t topspeedId = (topspeed != nullptr) ? topspeed->id : 0;
    const uint64_t lapsKey0 = (laps != nullptr) ? laps->key0 : 0;
    const uint64_t lapsKey1 = (laps != nullptr) ? laps->key1 : 0;
    const uint64_t lapsNode1 = (laps != nullptr) ? laps->node1 : 0;
    const uint64_t lapsNode2 = (laps != nullptr) ? laps->node2 : 0;
    const uint64_t lapsCell1 = (laps != nullptr) ? laps->cell1 : 0;
    const uint64_t lapsCell2 = (laps != nullptr) ? laps->cell2 : 0;
    const uint64_t speedKey0 = (topspeed != nullptr) ? topspeed->key0 : 0;
    const uint64_t speedKey1 = (topspeed != nullptr) ? topspeed->key1 : 0;
    const uint64_t speedNode1 = (topspeed != nullptr) ? topspeed->node1 : 0;
    const uint64_t speedNode2 = (topspeed != nullptr) ? topspeed->node2 : 0;
    const uint64_t speedCell1 = (topspeed != nullptr) ? topspeed->cell1 : 0;
    const uint64_t speedCell2 = (topspeed != nullptr) ? topspeed->cell2 : 0;

    char detail[1024];
    std::snprintf(
        detail, sizeof(detail),
        "f=%llu rs=0x%llx lapsId=0x%llx laps=%d topspeedId=0x%llx topspeed=%d %s "
        "vptr=0x%llx vptrOk=%d reg=0x%llx "
        "lapsOk=%d lapsWhy=%s lapsKey0=0x%llx lapsKey1=0x%llx "
        "lapsNode1=0x%llx lapsNode2=0x%llx lapsCell1=0x%llx lapsCell2=0x%llx "
        "topspeedOk=%d topspeedWhy=%s topspeedTelemetry=%d "
        "speedKey0=0x%llx speedKey1=0x%llx "
        "speedNode1=0x%llx speedNode2=0x%llx speedCell1=0x%llx speedCell2=0x%llx",
        frame, static_cast<unsigned long long>(sample.ruleSet),
        static_cast<unsigned long long>(lapsId), sample.totalLaps,
        static_cast<unsigned long long>(topspeedId), sample.topSpeed, status,
        static_cast<unsigned long long>(sample.vptr), sample.vptrOk ? 1 : 0,
        static_cast<unsigned long long>(
            resolveMainImageLoadBias() + member_value_store::kRegistryBssVa),
        (laps != nullptr && laps->ok) ? 1 : 0,
        sample.lapsReason != nullptr ? sample.lapsReason : "none",
        static_cast<unsigned long long>(lapsKey0),
        static_cast<unsigned long long>(lapsKey1),
        static_cast<unsigned long long>(lapsNode1),
        static_cast<unsigned long long>(lapsNode2),
        static_cast<unsigned long long>(lapsCell1),
        static_cast<unsigned long long>(lapsCell2),
        (topspeed != nullptr && topspeed->ok) ? 1 : 0,
        sample.topspeedReason != nullptr ? sample.topspeedReason : "none",
        sample.topSpeedTelemetry,
        static_cast<unsigned long long>(speedKey0),
        static_cast<unsigned long long>(speedKey1),
        static_cast<unsigned long long>(speedNode1),
        static_cast<unsigned long long>(speedNode2),
        static_cast<unsigned long long>(speedCell1),
        static_cast<unsigned long long>(speedCell2));
    appendGuestBootLine(kGuestViewLogPath, kPhaseName, kPhaseOrdinal, 0, detail);
}

}  // namespace

void pumpRaceMetricsProbe(unsigned long long frame) {
    if (g_haveSample && frame < g_lastSampleFrame + kSamplePeriodFrames) {
        return;
    }
    g_haveSample = true;
    g_lastSampleFrame = frame;

    RaceMetricsValueSample sample{};
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0) {
        // There is no guest to measure yet. The race-state probe stays quiet
        // in the same situation, so this one does too.
        g_sample = sample;
        return;
    }

    // The RuleSet, resolved the way the race-state probe resolves it (the
    // CGlobal -> the GameMode -> the ruleset map -> the node whose value is a
    // RuleSet_RaceMetrics). It is resolved per sample, not cached: a new race
    // allocates a NEW object, and a cached pointer would be the old one.
    const uintptr_t ruleSet = raceStateResolveRuleSetRaceMetrics();
    sample.ruleSet = ruleSet;
    if (ruleSet == 0) {
        sample.reason = "the-race-metrics-object-was-not-found";
        sample.lapsReason = sample.reason;
        sample.topspeedReason = sample.reason;
        g_sample = sample;
        logRaceMetrics(frame, sample, nullptr, nullptr);
        return;
    }

    // THE POSITIVE CONTROL: the object's first word is its vptr, and the
    // address point of "19RuleSet_RaceMetrics" is exact (the class has no
    // derived class). A word that is not that vptr is not this object, and
    // the probe says so instead of reading a lap count out of it.
    uint64_t vptr = 0;
    if (!raceStateReadWord(ruleSet, &vptr)) {
        sample.reason = "the-race-metrics-object-is-unreadable";
        sample.lapsReason = sample.reason;
        sample.topspeedReason = sample.reason;
        g_sample = sample;
        logRaceMetrics(frame, sample, nullptr, nullptr);
        return;
    }
    sample.vptr = static_cast<uintptr_t>(vptr);
    sample.vptrOk = (vptr == bias + kRuleSetRaceMetricsVptrElfVa);
    if (!sample.vptrOk) {
        sample.reason = "the-object-is-not-a-race-metrics-object";
        sample.lapsReason = sample.reason;
        sample.topspeedReason = sample.reason;
        g_sample = sample;
        logRaceMetrics(frame, sample, nullptr, nullptr);
        return;
    }

    // The two member ids, read FRESH. They are the rotating tokens the guest
    // re-keys on every read of the member, so they are never cached.
    uint64_t lapsId = 0;
    if (!raceStateReadWord(ruleSet + kRuleSetTotalLapsOff, &lapsId)) {
        sample.reason = "the-lap-descriptor-is-unreadable";
        sample.lapsReason = sample.reason;
        sample.topspeedReason = sample.reason;
        g_sample = sample;
        logRaceMetrics(frame, sample, nullptr, nullptr);
        return;
    }
    uint64_t topspeedId = 0;
    if (!raceStateReadWord(ruleSet + kRuleSetTopSpeedOff, &topspeedId)) {
        sample.reason = "the-top-speed-descriptor-is-unreadable";
        sample.lapsReason = sample.reason;
        sample.topspeedReason = sample.reason;
        g_sample = sample;
        logRaceMetrics(frame, sample, nullptr, nullptr);
        return;
    }

    // The two lookups. The registry is in the guest's own image, so it is
    // mapped for the whole run; the bucket arrays and the cells are on the
    // guest's heap, so they go through the safe read's page check.
    const uintptr_t registry = bias + member_value_store::kRegistryBssVa;
    GuestReader read;
    member_value_store::Lookup laps{};
    member_value_store::Lookup topspeed{};
    const char* lapsWhy =
        member_value_store::lookup(read, registry, lapsId, &laps);
    const char* topspeedWhy =
        member_value_store::lookup(read, registry, topspeedId, &topspeed);

    sample.lapsReason = (lapsWhy != nullptr) ? lapsWhy : "none";
    sample.topspeedReason = (topspeedWhy != nullptr) ? topspeedWhy : "none";
    sample.totalLaps = laps.value;
    sample.topSpeed = topspeed.value;
    sample.topSpeedTelemetry = telemetrySpeed(topspeed.value);
    sample.ok = (lapsWhy == nullptr) && (topspeedWhy == nullptr);
    if (!sample.ok) {
        sample.reason = (lapsWhy != nullptr) ? lapsWhy : topspeedWhy;
    }

    g_sample = sample;
    logRaceMetrics(frame, sample, &laps, &topspeed);
}

const RaceMetricsValueSample& raceMetricsValueSample() {
    return g_sample;
}

}  // namespace rr3
