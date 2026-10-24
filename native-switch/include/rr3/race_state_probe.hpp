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
//       +0x10  std::vector<RuleSetModifier_RaceMetrics>
//       +0x40  the vector's inline storage: the first metrics object
//
//   GameMode                       (RTTI "8GameMode")
//       +0x128 std::map<std::string, RuleSet_RaceMetrics*>
//
//   G (the CGlobal), reached through the GOT slot at ELF 0x1f6d5d8.
//       G+0xa10  the GameMode, constructed in place
//
// WHY THE PROBE SEARCHES INSTEAD OF FOLLOWING THE CHAIN BLINDLY
// -------------------------------------------------------------
// The chain above is real, but a pointer chain that is three links deep and
// two of them are the guest's own std::map is exactly the kind of thing that
// breaks silently: the map's node layout is the guest's, not the C++
// standard's, and the GameMode is constructed at TWO offsets in the guest
// (G+0xa10 and G+0x1d88). So the probe WALKS the object graph from the
// GameMode and accepts an object only when its first word is EXACTLY the
// vtable address of the class it claims to be.
//
// That check is the identity check: an object's first word IS its vptr, and
// the vtable's address was derived from the guest's own RTTI (the typeinfo's
// __name "27RuleSetModifier_RaceMetrics" -> the typeinfo -> the vtable).
// So "the object whose vptr is 0x1e4b3f8" is not a guess about a layout: it
// is a statement about a class, and the class has a name.
//
// WHAT THE PROBE REPORTS, PER FRAME
// ---------------------------------
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
