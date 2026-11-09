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
    // THE CLASS NAME, RESOLVED FROM THE GUEST'S OWN RTTI AT RUNTIME.
    //
    // The log used to print a CONSTANT here ("cls=27RuleSetModifier_RaceMetrics"
    // was part of the format string), so it said the class was right while the
    // vptr check in the same line said it was wrong.  MEASURED 2026-11-01
    // (q92..q95): the object the port had was not the RaceMetrics at all --
    // what it read there was ASCII text ("532#355#"), and the car's pointer
    // came out as 0x3533352335333523.
    //
    // ⇒ The name is now a MEASUREMENT: the guest's own RTTI, read at runtime.
    char className[96]{};
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
    // Which of the six members were read out of the guest's value store
    // (bit 0 = lap distance, 1 = total laps, 2 = previous spline distance,
    // 3 = total spline distance, 4 = top speed, 5 = total time).  A member
    // that is NOT in the mask kept its raw descriptor reading.
    uint32_t membersResolved{};
    // Which of the six members the port proved BY NAME at runtime, from the
    // guest's own descriptor (the name hash and len^hash it wrote there).
    uint32_t membersIdentified{};
    // WHY the members did not resolve.
    //
    // "mres=0x0" on its own is a dead end: it says that all six lookups
    // failed but not what any of them hit.  The guest's value store knows
    // exactly why ("the-chain-ended", "the-node-is-in-another-bucket",
    // "the-table-has-no-buckets", ...), and the port threw that away.
    //
    // So the FIRST failure is reported in full: the reason, the member id the
    // slot held, the four keys the lookup derived from it, the four table
    // counts it read, the four chain nodes it found and the four value cells.
    // Every one of those is a measured value, and together they say whether
    // the slot's id was stale, the registry was empty, or the chain was
    // walked wrongly.
    //
    // The strings are static literals, so the sample stays a plain value type.
    const char* memberFailure{nullptr};
    uint64_t memberId{};
    uint64_t memberKeys[4]{};
    uint64_t memberCounts[4]{};
    uint64_t memberNodes[4]{};
    uint64_t memberCells[4]{};
    // THE GUEST'S OWN PER-CAR STATE.
    //
    // The chain, each step proved by the guest's own code:
    //     RuleSet_RaceMetrics + 0x20   -> CAR         (ELF 0xb35c94)
    //     CAR + 0x288                   -> CAR_STATE   (ELF 0xb35ce8)
    //     CAR_STATE + 0x1a8             = the SPLINE NODE INDEX   (int)
    //     CAR_STATE + 0x1c8             = the DISTANCE TRAVELLED  (int)
    // and the car itself carries its CURRENT and PREVIOUS spline node
    // (CAR+0x24 / CAR+0x20) and its WORLD POSITION (CAR+0x30/+0x34/+0x38).
    //
    // The position is what a lateral offset would be computed FROM: the
    // guest computes the racing-line offset in its projection and does not
    // store it, but it stores everything the projection needs.
    uintptr_t carAddress{};
    uint32_t carPrevNode{};
    uint32_t carNode{};
    // The car's WORLD POSITION and the point it is AIMING AT, both 32-bit
    // integers in the guest's 1/256 metre unit.  The guest's own AI drives
    // by this pair, so the port can drive by it too.
    int32_t carX{};
    int32_t carY{};
    int32_t carZ{};
    int32_t carTargetX{};
    int32_t carTargetY{};
    int32_t carTargetZ{};
    // The car's own number, which the guest reads at 0xb35ca8
    // (`ldr w1,[x9,#0x49c]`).  It is a cheap identity check: the value the
    // port reads has to be the one the guest itself would read, and it
    // changes when the player changes car.
    uint32_t carNumber{};
    // Whether the position that was just read is PLAUSIBLE as a world
    // position: inside the gate the port checks, and not all zero.  It is
    // reported so that a run which read the wrong object says so in the
    // log instead of driving on noise.
    bool positionPlausible{false};
    // Whether the AIM TARGET that was just read is plausible: inside the
    // guest's 1/256 metre range AND a sensible distance AHEAD of the car.
    //
    // MEASURED 2026-11-01 (q81): the offset the port reads for the target
    // holds 8992131 -- 35 km -- so a target that is not gated would steer
    // the car on noise.  The pursuit has to be refused, not merely
    // unhelpful, when the target fails this.
    bool targetPlausible{false};
    // How far the target is from the car, in metres, when the two are
    // believed to be in the same units.  Reported so a run can say "the
    // target is 35 km away" instead of leaving it to be deduced.
    double targetDistance{};
    // Whether the target was taken from the TRACK'S RACING LINE rather than
    // from the guest's own memory.
    //
    // MEASURED 2026-11-01 (q81): the guest's own aim target is unreadable
    // (8992131 in its Y word -- 35 km -- while the car is at 207 m from the
    // origin), so the target comes from the `.spline` the guest opened, and
    // the port fits that line to the car's own positions at run time
    // (source/race_line.cpp).  This flag says which of the two the target
    // actually came from, so a run cannot claim the line is in use when the
    // line was refused.
    bool targetFromLine{false};
    // The fit's residual, in metres: the mean distance from the car's own
    // positions to the fitted line.  On a real circuit this is the car's
    // lateral offset from the centre line; with a WRONG scale it is
    // hundreds of metres, which is what tells the two apart.
    double lineResidual{};
    // The car's distance from the track's own RACING line (the spline's
    // "ai_spline" chunk), in metres, in the same frame and with the same
    // transform as the line the car is being steered along.  Negative means
    // the chunk is not in this file.
    //
    // WHY: the port follows the CENTRE line, and the two are different
    // lines -- MEASURED 2026-11-04 on Spa: 67.0 m shorter a lap and up to
    // 8.25 m apart.  The car hits the barriers about ten times a race while
    // sitting within 2 m of the centre line, so this is the number that says
    // whether it is on the wrong line.
    double racingResidual{-1.0};
    // THE CAR'S DISTANCE TO THE CHUNK THE TARGET CAME FROM, IN METRES.
    //
    // WHY IT IS REPORTED: the line is fitted on ONE chunk, and the car can be
    // on another one -- MEASURED 2026-11-10 (round pursuit1): the fitted chunk
    // was 13,215 m from the car, so the port had no line to aim at and the
    // pursuit was off for the whole round.  The port now picks the chunk the
    // car is on (the transform is a property of the track, so no re-fit is
    // needed), and this number says how far the car is from THAT chunk.
    double chunkForTarget{-1.0};
    // THE LOOK-AHEAD DISTANCE THE PURSUIT USED, IN METRES.
    //
    // WHY IT IS REPORTED: the look-ahead is the single number that decides
    // where the car aims, and it is either constant or derived from the
    // curvature of the road ahead.  A run that does not say which distance
    // it used cannot be compared with another run, so the number goes in the
    // log with the rest of the pursuit state.
    //
    // MEASURED 2026-11-04 (q147): on the track's own racing line a
    // constant 20 m look-ahead gave a pursuit error of 0.7 rad on the
    // corners, and the controller steers by AIMING AT A POINT -- so that is
    // a steering command 40 degrees wrong.
    float lookAheadMetres{0.0f};
    uintptr_t trackStateAddress{};
    uint32_t splineNode{};
    uint32_t splineDistance{};
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

// A SAFE read of one 64-bit word of guest memory.
//
// This is the probe's own safe-read path -- the plausibility gate, then the
// guest-image check, then svcQueryMemory, then the read -- exposed so that
// another diagnostic reads guest memory through exactly the same door
// instead of through a second, subtly different one. False when the address
// cannot be read; the port must never read guest memory any other way.
bool raceStateReadWord(uintptr_t address, uint64_t* out);

// The live CGlobal -- the game global G -- resolved the way this file
// resolves it: the GOT slot at ELF 0x1f6d5d8 -> the .bss cell -> G.
// 0 when the guest is not up yet.
//
// Exposed because the semantic UI probe needs the SAME object to reach the
// FrontEnd2::MainMenuManager (G + 0x17f8).  The chain lives in one place on
// purpose: a second, subtly different chain is a second thing to get wrong,
// and this repository has already paid for one divergence between the two
// copies of the GameMode lookup.
uintptr_t raceStateResolveGlobal();

// The live RuleSet_RaceMetrics object, resolved the way this probe resolves
// it (the CGlobal -> the GameMode -> the ruleset map -> the node whose value
// is a RuleSet_RaceMetrics). 0 when the chain does not lead there yet.
//
// It is resolved ON DEMAND rather than served from the probe's own cache,
// because a new race allocates a NEW object. The value-store probe
// (source/race_metrics_probe.cpp) reads the member values out of it.
uintptr_t raceStateResolveRuleSetRaceMetrics();

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
    // The throttle. FALSE only while the recovery reverses off a barrier:
    // the guest's own message is "hold the brake to reverse", and it only
    // reverses when the throttle is RELEASED -- the port used to hold the
    // throttle down in every frame, so throttle+brake was just braking and
    // the recovery could never back the car off anything.
    bool throttle{true};
    float steer{};
    float magnitude{};
    float bestMagnitude{};
    int sweepDirection{1};
    int sign{1};
    float bestRate{};
    float rate{};
    // The pure-pursuit controller's own numbers: the signed angle from the
    // car's heading to the point it is aiming at, and whether it was in
    // charge at all.
    float steerError{};
    bool pursuit{};
    int flips{};
    int stalls{};
    unsigned long long frames{};
    unsigned long long sinceProgress{};
};
const RaceDriveState& raceDriveState();

}  // namespace rr3
