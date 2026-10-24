#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE RACE-STATE PROBE.
//
// The guest's HUD telemetry is never drawn (the port's own text layer sees no
// race text at all), so the lap counter cannot be read off the screen. It has
// to be read out of the guest's own memory, and the object that holds it was
// found in the guest BINARY, not guessed:
//
//   RuleSetModifier_RaceMetrics   (RTTI "27RuleSetModifier_RaceMetrics")
//       +0x08  m_nLapDistance
//       +0x20  m_nTotalLaps
//       +0x38  m_nPreviousSplineDistance
//       +0x50  m_nTotalSplineDistanceTravelled
//       +0x68  m_nTopGameSpeed
//       +0x88  m_nTotalTime
//
//   RuleSet_RaceMetrics            (RTTI "19RuleSet_RaceMetrics")
//       +0x40  the metrics modifier, built IN PLACE by the guest
//
//   GameMode                       (RTTI "8GameMode")
//       +0x128 std::map<std::string, RuleSet*>
//
//   G (the CGlobal), reached through the GOT slot at ELF 0x1f6d5d8.
//       G+0x10d88 the GameMode POINTER
//
// WHY THE PROBE FOLLOWS A POINTER INSTEAD OF LOOKING FOR A VTABLE
// -------------------------------------------------------------
// The obvious way to find a live object in C++ memory is to search for its
// vtable address. That is what the first version of this probe did, and it
// could never work:
//
//   * The guest's "8GameMode" typeinfo has THIRTEEN vtable address points,
//     because GameMode has virtual bases. A live object can hold any of
//     them, and the port only knew one.
//   * More decisively: the guest NEVER instantiates a bare GameMode. It
//     instantiates 34 DERIVED classes (SoloMode, StandardRaceMode,
//     TimeTrialMode, ...), and a derived object's vptr is its OWN class's
//     address point, not the base's. So a search for the base's address
//     point (0x1e98168) is structurally guaranteed to find nothing.
//
// The probe therefore follows the pointer the guest itself stores: the
// CGlobal's member at +0x10d88 holds the current GameMode. That is the
// guest's own bookkeeping, so it cannot be wrong about where the GameMode
// is. The vptr is then read only to SAY WHICH MODE it is.
//
// The offset comes from TWO independent readings of the guest (the teardown
// at ELF 0x10af1d4 and the mode creation at ELF 0xb08960), which agree.
//
// The map is walked rather than sampled: the guest also inserts a ruleset
// under the key "grid", so the root node is not necessarily the race
// metrics entry. The decision is the value's vptr (RuleSet_RaceMetrics has
// no derived classes, so its address point is exact), and the key is
// reported as an independent cross-check.
//
// WHAT THE PROBE REPORTS, PER FRAME
// ---------------------------------
//   the GameMode's address point (so a reader can name the mode class), the
//   lap distance, total laps, previous/total spline distance, top speed and
//   total time, for EVERY metrics object it finds, plus the derived rate of
//   progress along the track's spline. The rate is what the closed-loop
//   steering in source/guest_input.cpp steers on.
// ---------------------------------------------------------------------------
namespace rr3 {

// One metrics object, as the guest's own memory holds it.
struct RaceMetricsSample {
    bool valid{};          // the object was found and reads cleanly
    uintptr_t address{};   // the RuleSetModifier_RaceMetrics object
    uintptr_t vptr{};      // its vptr, which must equal the expected vtable
    // The guest's members are read as 32-bit words AND as floats, because the
    // guest's own member names ("m_n...") say "integer" while the values
    // themselves decide. Both readings are reported; nothing is assumed.
    uint32_t lapDistance{};
    uint32_t totalLaps{};
    uint32_t previousSplineDistance{};
    uint32_t totalSplineDistance{};
    uint32_t topSpeed{};
    uint32_t totalTime{};
    float lapDistanceF{};
    float totalLapsF{};
    float previousSplineDistanceF{};
    float totalSplineDistanceF{};
    float topSpeedF{};
    float totalTimeF{};
    // The rate of change of the progress metric, in units per frame.
    double progressRate{};
    // The progress metric the rate was computed from.
    double progress{};
    bool progressFromTotal{};
};

// Everything the probe found this frame.
struct RaceStateSample {
    bool gameModeFound{};
    uintptr_t gameMode{};
    uintptr_t gameModeVptr{};
    bool metricsFound{};
    int metricsCount{};
    RaceMetricsSample metrics[4]{};
    // The metrics object the steering uses: the one whose progress moves.
    int activeIndex{-1};
};

// Finds the GameMode and the metrics objects. Safe to call every frame; it
// only re-searches when the previous search failed or the objects went away.
void pumpRaceStateProbe(unsigned long long frame);

// The most recent sample. Never null.
const RaceStateSample& raceStateSample();

// The progress rate of the active metrics object, in units per frame, and the
// progress value itself. The steering uses this.
bool raceStateProgress(unsigned long long frame, float* rate, double* progress,
                       bool* fromTotal);

// TRUE when the closed-loop steering is allowed to drive: the control file
// exists and says so, and a live metrics object has been found.
bool raceStateDriveEnabled();

// The steering the closed loop currently wants, in the guest's axis-0 units
// (-1..1). Only meaningful when raceStateDriveEnabled() is true.
float raceStateSteering();

// The closed loop's own state, for the log line.
struct RaceDriveState {
    bool enabled{};
    bool discovered{};
    bool brake{};          // the recovery asks for the brake
    float steer{};
    float magnitude{};
    float bestMagnitude{};
    int sweepDirection{1};
    int sign{1};
    float bestRate{};
    float rate{};
    int flips{};
    int stalls{};
    unsigned long long frames{};
    unsigned long long sinceProgress{};
};
const RaceDriveState& raceDriveState();

}  // namespace rr3
