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
// further along the loop.  False when the line is not usable, when the car
// is not near it, or when the target would be behind the car.
bool raceLineTarget(const RaceLine& line, const SplineFile& spline,
                    double carX, double carY, double lookAheadMetres,
                    double* targetX, double* targetY);

}  // namespace rr3
