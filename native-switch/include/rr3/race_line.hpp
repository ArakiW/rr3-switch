// The racing line, fitted into the port's world frame.
//
// WHY THIS EXISTS
// ---------------
// The port's controller steers by following a TARGET POINT.  The guest's
// own aim target is unreadable (measured 2026-11-01, q81: the offset read
// holds 8992131 in its Y word, a target 35 km away), so the target has to
// come from the track's racing line instead -- and the racing line is in
// the guest's `.spline` file, which the port now decodes
// (source/guest_spline.cpp).
//
// THE PROBLEM: TWO FRAMES
// -----------------------
// The spline's nodes are int32 in some stored unit U.  The port's car
// position is in 1/256 m.  So the target has to be mapped:
//
//     p_port = s * q_stored + t        (per axis)
//
// with s the scale and t the offset, and the axes may be swapped or
// flipped.  The scale is a property of the guest (the node accessor at ELF
// 0x1425d54 multiplies the coordinates by 16), and the offset is a
// property of the TRACK.
//
// ⇒ THE FIT IS DONE AT RUN TIME, ON THE TRACK THAT IS ACTUALLY LOADED:
// the port has both the captured spline and the car's own positions, and
// the car has to lie ON the line.  That fixes s, t and the axis variant
// together, and it needs no track name, no table and no per-track data.
//
// THE GUARD
// ---------
// A pursuit that aims at a point 35 km away is not a pursuit, it is a
// steering command derived from noise -- and the port already learned that
// the hard way.  So the line is used only when the fit says the car is ON
// it: the residual (the mean distance from the car's positions to the
// polyline) has to be small, and the target has to be ahead of the car.
// Otherwise the line is REFUSED and the log says why.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>

#include "rr3/guest_spline.hpp"

namespace rr3 {

// The fit of one track's line to the port's world frame.
struct RaceLine {
    bool valid = false;
    double scale = 0.0;    // port units (1/256 m) per stored unit
    double offsetX = 0.0;  // port units
    double offsetY = 0.0;
    bool swapAxes = false;
    bool flipX = false;
    bool flipY = false;
    double residual = 0.0;  // metres: mean distance from the car to the line
    size_t samples = 0;
    char chunk[32] = "";
};

// Fits the captured spline to the car's own positions.
//
// The car's positions are in PORT units (1/256 m).  The scale search walks
// the powers of two around the measured value (the guest multiplies the
// stored coordinates by 16) and the eight axis variants, aligns each
// candidate by its centroid, and keeps the one with the smallest residual.
bool raceLineFit(const SplineFile& spline, const char* chunkName,
                 const double* carX, const double* carY, size_t count,
                 RaceLine* out);

// The look-ahead point on the line, in PORT units.
//
// The nearest node is found first, and the target is the point L metres
// further along the loop IN THE DIRECTION THE CAR IS TRAVELLING.
//
// WHY THE DIRECTION IS AN ARGUMENT AND NOT A CONSTANT
// ----------------------------------------------------
// The fit's residual cannot tell the two directions apart: a closed loop
// traversed backwards has the same SHAPE, so the fit picks the same scale,
// the same axis variant and the same offset either way.  Only the node
// ORDER says which way round the track the car goes, and the spline does
// not necessarily store it that way round.
//
// MEASURED 2026-11-03 over the archived rounds: the pursuit's error sat at
// ~180 degrees in 210 of 210 samples in q119, 71 of 113 in q121 and 21 of
// 133 in q116 -- the look-ahead point was BEHIND the car, so the controller
// steered the wrong way, the car left the track at the first corner, and
// the guest never crossed the line.  That is where "laps = -1" came from:
// the guest only counts a lap when the car crosses it.
//
// The heading is the car's direction of travel, in PORT units -- the same
// vector the pursuit uses for its own error.  A zero heading keeps the
// spline's own order.
//
// THE DIRECTION IS LOCKED, AND THAT IS THE POINT
// ----------------------------------------------
// `direction` is an IN/OUT argument: 0 means "not decided yet", and a
// non-zero value means "this is the direction round the track, and it does
// not change again".  On the way out it holds the decision that was used.
//
// WHY IT HAS TO BE LOCKED
// MEASURED 2026-11-04 (q142, archive
// 20261104-145617-eden-rr3-q142-corners): the pursuit ran 141 samples and the
// error went -0.47 -> +0.44 -> -3.05 -> +2.99 -> -2.73 rad in a few frames
// each.  The car was never more than 2 m from the line and the look-ahead
// point was 16..43 m away, so an error of ~180 degrees is not geometry: it is
// the car's direction of travel having REVERSED.  And because the side of the
// line the target was taken from was re-decided from that same reversing
// heading EVERY FRAME, the target jumped to the other side of the car at the
// same moment, the error became the sum of two flips, and the controller
// slammed full lock first one way and then the other.  The car then drove
// backwards, which reversed the heading again: a closed loop that never
// settles.  The car reached 5.08 % of a lap and stopped.
//
// The track does not change direction while the car drives on it, so the
// decision belongs to the RACE, not to the frame.  The caller owns the lock,
// which also keeps this function free of hidden state (the host self-test
// exercises it directly).
bool raceLineTarget(const RaceLine& line, const SplineFile& spline,
                    double carX, double carY, double headingX, double headingY,
                    int* direction,
                    double lookAheadMetres, double* targetX, double* targetY);

// The look-ahead point on the line, with a LOOK-AHEAD THAT FOLLOWS THE
// CURVATURE.
//
// Same as raceLineTarget() except for the look-ahead distance, which is
// computed from how sharply the road bends AHEAD of the car instead of
// being a constant.
//
// WHY.  MEASURED 2026-11-04 (q147): on the track's own racing line the car
// sat 0.32 m from it -- the right line at last -- and the lap progress got
// WORSE (10.35 % -> 5.14 %), because the look-ahead was a constant 20 m and
// 20 m ahead on a sharp corner is a point 0.7 rad (40 degrees) off the
// car's actual path.  The controller steers by AIMING AT A POINT, so that
// is a steering command 40 degrees wrong.
//
// The distance now SHORTENS as the corner tightens: long on a straight (it
// damps the steering), short in a corner (it aims at the part of the line
// the car is about to drive).  `lookAheadMetres`, when given, receives the
// distance that was used.
//
// The direction lock, the nearest-node search and the "the car has to be ON
// the line" guard are identical to raceLineTarget()'s, because those are
// the parts that were MEASURED to be right.
// `adaptiveLookAhead` picks between the curvature-following distance and
// the constant one, so that there is ONE call site for both.
bool raceLineTargetAdaptive(const RaceLine& line, const SplineFile& spline,
                            double carX, double carY, double headingX, double headingY,
                            int* direction,
                            double* targetX, double* targetY,
                            double* lookAheadMetres,
                            bool adaptiveLookAhead,
                            double fixedLookAheadMetres);

// The car's distance from another chunk of the SAME spline, in metres.
//
// WHY: the fit's transform was established on ONE chunk (the centre line),
// but the file holds more than one and they live in the same stored
// coordinate system -- so the same transform puts them all in the port's
// frame.  The racing line (ai_spline) is a DIFFERENT line from the centre
// line: MEASURED 2026-11-04 on Spa, it is 67.0 m shorter a lap (it cuts the
// corners) and up to 8.25 m away from the centre line.
//
// The port follows the centre line.  Whether that is why the car hits the
// barriers ten times a race is a question this answers directly: the car's
// distance from the racing line, in the same frame, with the same transform.
//
// The distance is to the nearest SEGMENT, not the nearest node, because the
// nodes are ~8 m apart.
bool raceLineDistanceToChunk(const RaceLine& line, const SplineFile& spline,
                             const char* chunkName,
                             double carX, double carY, double* metres);

// THE CHUNK THE CAR IS ACTUALLY ON.
//
// WHY: the fit's transform -- the scale, the offset and the axis variant --
// is a property of the TRACK, not of the chunk: every chunk of the same
// spline file lives in the same stored coordinate system, so the same
// transform puts them all in the port's frame.  That is why the chunk the car
// is on can be found with the transform the fit already established, and NO
// RE-FIT IS NEEDED.
//
// MEASURED 2026-11-10 (round pursuit1): the fitted chunk was 13,215 m from
// the car -- the pursuit's own `ctdist` -- so the port had no line to aim at
// at all, the guest's own (garbage) target was refused, and the pursuit was
// off (pur=0) for the whole round.
//
// The distance to each chunk is to its nearest SEGMENT, in metres, and the
// chunk with the smallest distance wins.
bool raceLineNearestChunk(const RaceLine& line, const SplineFile& spline,
                          double carX, double carY,
                          char* chunkName, size_t chunkNameSize,
                          double* metres);

}  // namespace rr3
