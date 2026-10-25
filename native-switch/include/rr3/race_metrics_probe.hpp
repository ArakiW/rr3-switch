#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE RACE-METRICS VALUE PROBE.
//
// The guest's HUD is never drawn, so the lap counter cannot be read off the
// screen. The race-state probe finds the guest's RuleSet_RaceMetrics object
// (source/race_state_probe.cpp) but it can only report the member SLOTS, and
// a slot holds a descriptor, not a number: the member's id, the hash of its
// name and len ^ hash.
//
// The number itself lives in the guest's global member VALUE store, a
// four-table hash registry in the guest's .bss (see
// include/rr3/member_value_store.hpp for the layout and the algorithm, which
// were decoded from the guest's own code).
//
// This probe reads it the PURE way:
//   * the id is read FRESH from the slot every sample, because the guest's
//     accessor re-keys the member on every read and the id rotates;
//   * the value is looked up in the registry WITHOUT calling the guest, so
//     the probe never mutates the guest's state;
//   * every guest read goes through the port's existing safe-read path.
//
// WHAT IT REPORTS, PER SAMPLE
// ---------------------------
//   the RuleSet_RaceMetrics address and its vptr (the positive control: the
//   vptr must be the address point of the class "19RuleSet_RaceMetrics"),
//   the member id of m_nTotalLaps and the VALUE it maps to, the member id of
//   m_nTopGameSpeed and the VALUE it maps to, the registry's address and
//   seeds, and every raw piece of the two lookups (the keys, the chain nodes
//   and the cells), so a lookup that goes wrong can be debugged from the log
//   alone.
//
// m_nTotalLaps is a small int that the guest increments by one per completed
// lap. Whether it counts the laps of the race or the laps COMPLETED is not
// established, so the probe reports the number and lets the reader decide.
//
// m_nTopGameSpeed is a raw int32. The guest's own telemetry prints
// |raw| * 900 * 2^-16 as "Top Speed (km/h)", so that scaled number is
// reported too; the physical unit of the raw value is NOT established.
// ---------------------------------------------------------------------------
namespace rr3 {

// One sample of the guest's race metrics, read out of the guest's own
// member value store.
struct RaceMetricsValueSample {
    bool ok{};              // both members were read
    uintptr_t ruleSet{};    // the RuleSet_RaceMetrics object
    uintptr_t vptr{};       // its first word: the class address point
    bool vptrOk{};          // it is the RuleSet_RaceMetrics address point
    int32_t totalLaps{};    // m_nTotalLaps
    int32_t topSpeed{};     // m_nTopGameSpeed, raw
    int32_t topSpeedTelemetry{};  // |raw| * 900 * 2^-16, the guest's scale
    const char* reason{nullptr};        // why the sample failed, when it did
    const char* lapsReason{nullptr};     // why the laps read failed
    const char* topspeedReason{nullptr}; // why the top speed read failed
};

// Samples the guest's race metrics on its own cadence. Safe to call every
// frame: it only reads every kSamplePeriodFrames frames.
void pumpRaceMetricsProbe(unsigned long long frame);

// The most recent sample. Never null.
const RaceMetricsValueSample& raceMetricsValueSample();

}  // namespace rr3
