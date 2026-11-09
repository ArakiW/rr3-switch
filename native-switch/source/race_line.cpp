// The racing line, fitted into the port's world frame.  The reasoning, the
// evidence and the guard are in include/rr3/race_line.hpp.
#include "rr3/race_line.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rr3 {
namespace {

constexpr const char* kCentreChunk = "centre_spline";

// THE SCALE SEARCH.
//
// The guest multiplies the stored coordinates by 16 when it uses them (the
// node accessor at ELF 0x1425d54), so 16 port units per stored unit is the
// measured starting point (16 stored units = 1/16 m = 16/256 m).  The
// search walks the powers of two on either side of it and then refines, so
// a wrong guess does not decide the fit.
const double kScales[] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0};

struct Variant {
    bool swap;
    bool flipX;
    bool flipY;
};

const Variant kVariants[] = {
    {false, false, false}, {true, false, false},  {false, true, false},
    {false, false, true},  {true, true, false},   {true, false, true},
    {false, true, true},   {true, true, true},
};

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// THE CHUNK'S NODES IN THE PORT'S FRAME.
//
// The fit's transform (scale, axes, offset) was established by aligning ONE
// chunk -- the centre line -- onto the car's path.  The other chunks of the
// same file live in the SAME stored coordinate system, so the SAME transform
// puts them in the port's frame too.  That is what makes the racing line
// (ai_spline) comparable with the centre line the car is on.
//
// MEASURED 2026-11-04 (Spa): the centre line is 7,016.9 m a lap and the
// racing line 6,949.8 m -- 67.0 m shorter, i.e. it cuts the corners -- and
// the two are 2.92 m apart at the median and 8.25 m at the worst.
void buildChunkPolyline(const RaceLine& line, const SplineChunk& chunk,
                         std::vector<Point>* out) {
    out->clear();
    out->reserve(chunk.nodes.size());
    for (const SplineNode& node : chunk.nodes) {
        double x = static_cast<double>(node.x);
        double y = static_cast<double>(node.y);
        if (line.swapAxes) {
            const double t = x;
            x = y;
            y = t;
        }
        if (line.flipX) x = -x;
        if (line.flipY) y = -y;
        out->push_back(Point{x * line.scale + line.offsetX,
                             y * line.scale + line.offsetY});
    }
}

// The stored nodes, in the candidate's axes and scale.
void transform(const SplineChunk& chunk, const Variant& variant, double scale,
               std::vector<Point>* out) {
    out->clear();
    out->reserve(chunk.nodes.size());
    for (const SplineNode& node : chunk.nodes) {
        double x = static_cast<double>(node.x);
        double y = static_cast<double>(node.y);
        if (variant.swap) {
            const double t = x;
            x = y;
            y = t;
        }
        if (variant.flipX) x = -x;
        if (variant.flipY) y = -y;
        out->push_back(Point{x * scale, y * scale});
    }
}

// The distance from a point to the polyline, and the nearest point on it.
double nearestOnPolyline(const std::vector<Point>& poly, double px, double py,
                         double* nearestX, double* nearestY) {
    double best = 1e30;
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        const double ax = poly[i].x;
        const double ay = poly[i].y;
        const double dx = poly[i + 1].x - ax;
        const double dy = poly[i + 1].y - ay;
        const double denominator = dx * dx + dy * dy;
        if (denominator <= 0.0) continue;
        double t = ((px - ax) * dx + (py - ay) * dy) / denominator;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const double qx = ax + t * dx;
        const double qy = ay + t * dy;
        const double d = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
        if (d < best) {
            best = d;
            if (nearestX != nullptr) *nearestX = qx;
            if (nearestY != nullptr) *nearestY = qy;
        }
    }
    return best;
}

// ALIGN THE POLYLINE ONTO THE CAR, AND MEASURE HOW WELL IT FITS.
//
// The centroid alignment is only the right translation when the car's
// samples are spread evenly over the WHOLE line, and they are not: a car
// that has driven half a lap has its centroid nowhere near the line's.
// MEASURED on a synthetic circle (the positive control in
// tests/host/race_line_selftest.cpp): the centroid alignment alone left a
// residual of 37.8 m on a circle of radius 62.5 m.
//
// ⇒ The translation is refined the way ICP does it: take each car's
// nearest point on the line, and move the line by the mean of the
// differences.  A few iterations converge, and the loop is over a few
// hundred nodes and a few dozen samples.
struct Alignment {
    double residual = 0.0;  // port units
    double shiftX = 0.0;
    double shiftY = 0.0;
};

Alignment alignOntoCar(std::vector<Point> poly, const double* carX,
                       const double* carY, size_t count) {
    Alignment alignment;
    if (poly.empty() || count == 0) {
        alignment.residual = 1e30;
        return alignment;
    }
    double totalShiftX = 0.0;
    double totalShiftY = 0.0;
    for (int iteration = 0; iteration < 8; ++iteration) {
        double sx = 0.0;
        double sy = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double qx = 0.0;
            double qy = 0.0;
            nearestOnPolyline(poly, carX[i], carY[i], &qx, &qy);
            sx += carX[i] - qx;
            sy += carY[i] - qy;
        }
        const double dx = sx / static_cast<double>(count);
        const double dy = sy / static_cast<double>(count);
        if (std::sqrt(dx * dx + dy * dy) < 0.01) break;
        for (Point& p : poly) {
            p.x += dx;
            p.y += dy;
        }
        totalShiftX += dx;
        totalShiftY += dy;
    }
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) {
        total += nearestOnPolyline(poly, carX[i], carY[i], nullptr, nullptr);
    }
    alignment.residual = total / static_cast<double>(count);
    alignment.shiftX = totalShiftX;
    alignment.shiftY = totalShiftY;
    return alignment;
}

}  // namespace

// The signed turn of a polyline at one node: the sine of the turn angle.
// Its SIGN says which way the curve bends there, and it is what tells a
// curve from its mirror image.  It is normalised, so it does not depend on
// the units the positions are in.
static double turnSineAt(const std::vector<Point>& poly, size_t index) {
    const size_t n = poly.size();
    if (n < 3) {
        return 0.0;
    }
    const size_t prev = (index + n - 1) % n;
    const size_t next = (index + 1) % n;
    const double ax = poly[index].x - poly[prev].x;
    const double ay = poly[index].y - poly[prev].y;
    const double bx = poly[next].x - poly[index].x;
    const double by = poly[next].y - poly[index].y;
    const double la = std::sqrt(ax * ax + ay * ay);
    const double lb = std::sqrt(bx * bx + by * by);
    if (la <= 0.0 || lb <= 0.0) {
        return 0.0;
    }
    return (ax * by - ay * bx) / (la * lb);
}

// The same, over the CAR's own last three samples.
//
// The car's path IS part of the line, so its turn sign is the line's turn
// sign.  A mirrored variant bends the other way, and the residual cannot
// tell them apart over a short arc.
static double carTurnSine(const double* xs, const double* ys, size_t count) {
    if (count < 3) {
        return 0.0;
    }
    const double ax = xs[count - 2] - xs[count - 3];
    const double ay = ys[count - 2] - ys[count - 3];
    const double bx = xs[count - 1] - xs[count - 2];
    const double by = ys[count - 1] - ys[count - 2];
    const double la = std::sqrt(ax * ax + ay * ay);
    const double lb = std::sqrt(bx * bx + by * by);
    if (la <= 0.0 || lb <= 0.0) {
        return 0.0;
    }
    return (ax * by - ay * bx) / (la * lb);
}

bool raceLineFit(const SplineFile& spline, const char* chunkName,
                 const double* carX, const double* carY, size_t count,
                 RaceLine* out) {
    if (out == nullptr || carX == nullptr || carY == nullptr || count < 3) {
        return false;
    }
    const char* wanted = chunkName != nullptr ? chunkName : kCentreChunk;
    const SplineChunk* chunk = spline.chunk(wanted);
    if (chunk == nullptr || chunk->nodes.size() < 8) {
        return false;
    }

    // THE HANDEDNESS TEST.
    //
    // The residual cannot tell a curve from its mirror image over a short
    // arc: the car has driven at most 48 metres (one sample per metre), and
    // over 48 metres a curve and its reflection agree with the car's
    // positions to within centimetres.  So the fit can pick the MIRROR,
    // and then the look-ahead point is on the mirror image -- the wrong
    // side of the track.
    //
    // MEASURED 2026-11-03: the variant CHANGED MID-RUN in q121 (six
    // different values in one round), and every change moved the look-ahead
    // point to the mirror image -- a full-lock steering kick in the wrong
    // direction.  That is where "the car drives 7% of a lap and then stops"
    // came from.
    //
    // What CAN tell them apart is the direction of the bend: the car's own
    // path is part of the line, so the line bends the way the car bends,
    // and a mirror bends the other way.
    const double carTurn = carTurnSine(carX, carY, count);
    // A straight section bends by nothing and says nothing, so the test only
    // applies when the car is actually turning (about three degrees).
    const bool turnIsUsable = std::fabs(carTurn) > 0.05;

    double bestResidual = 1e30;
    double bestScale = 0.0;
    Variant bestVariant{false, false, false};
    // The unfiltered winner, used only if the test rejects EVERY variant.
    double fallbackResidual = 1e30;
    double fallbackScale = 0.0;
    Variant fallbackVariant{false, false, false};
    std::vector<Point> poly;
    for (const Variant& variant : kVariants) {
        for (double scale : kScales) {
            transform(*chunk, variant, scale, &poly);
            const Alignment alignment = alignOntoCar(poly, carX, carY, count);
            if (alignment.residual < fallbackResidual) {
                fallbackResidual = alignment.residual;
                fallbackScale = scale;
                fallbackVariant = variant;
            }
            if (turnIsUsable) {
                // THE NODE THE CAR IS ON, AND THE WAY THE LINE BENDS THERE.
                // The polyline is not shifted yet -- alignOntoCar() works on a
                // copy -- so the shift is applied to the node here.
                size_t nearest = 0;
                double nearestDistance = 1e30;
                for (size_t i = 0; i < poly.size(); ++i) {
                    const double dx = poly[i].x + alignment.shiftX - carX[count - 1];
                    const double dy = poly[i].y + alignment.shiftY - carY[count - 1];
                    const double d = dx * dx + dy * dy;
                    if (d < nearestDistance) {
                        nearestDistance = d;
                        nearest = i;
                    }
                }
                if (turnSineAt(poly, nearest) * carTurn < 0.0) {
                    // THE MIRROR.  Dropped.
                    continue;
                }
            }
            if (alignment.residual < bestResidual) {
                bestResidual = alignment.residual;
                bestScale = scale;
                bestVariant = variant;
            }
        }
    }
    if (bestScale <= 0.0) {
        // EVERY VARIANT WAS DROPPED, which means the test does not apply to
        // this track (or the car is where the line's bend flips).  The
        // unfiltered winner is used instead: the line is then exactly as
        // good as it was before, and no worse.
        bestResidual = fallbackResidual;
        bestScale = fallbackScale;
        bestVariant = fallbackVariant;
    }
    if (bestScale <= 0.0) {
        return false;
    }

    // REFINE the scale around the winner, MULTIPLICATIVELY.
    //
    // The powers of two are a grid, and the truth need not be on it: the
    // guest multiplies the stored coordinates by 16 when it uses them, but
    // the line the port can see is the CENTRE line, which is not the same
    // length as the track the guest drives.  A refinement of +/-0.5 around
    // the winning power of two cannot reach a scale that is a factor of 1.5
    // away from it, and the fit would then report a large residual and
    // REFUSE a line that is perfectly good.
    //
    // ⇒ The sweep covers 0.5x .. 2x of the winner, in 1/32 steps of the
    // factor, which is 49 fits.
    for (int step = 0; step <= 48; ++step) {
        const double factor = 0.5 + static_cast<double>(step) / 32.0;
        const double scale = bestScale * factor;
        if (scale <= 0.0) continue;
        transform(*chunk, bestVariant, scale, &poly);
        const Alignment alignment = alignOntoCar(poly, carX, carY, count);
        if (alignment.residual < bestResidual) {
            bestResidual = alignment.residual;
            bestScale = scale;
        }
    }

    // The final alignment, whose shift IS the offset.
    transform(*chunk, bestVariant, bestScale, &poly);
    const Alignment alignment = alignOntoCar(poly, carX, carY, count);

    RaceLine line;
    line.scale = bestScale;
    line.swapAxes = bestVariant.swap;
    line.flipX = bestVariant.flipX;
    line.flipY = bestVariant.flipY;
    line.residual = alignment.residual / 256.0;  // port units -> metres
    line.samples = count;
    std::snprintf(line.chunk, sizeof(line.chunk), "%s", wanted);
    line.offsetX = alignment.shiftX;
    line.offsetY = alignment.shiftY;
    line.valid = true;
    *out = line;
    return true;
}

bool raceLineTarget(const RaceLine& line, const SplineFile& spline,
                    double carX, double carY, double headingX, double headingY,
                    int* direction,
                    double lookAheadMetres, double* targetX, double* targetY) {
    if (!line.valid || targetX == nullptr || targetY == nullptr) {
        return false;
    }
    const SplineChunk* chunk = spline.chunk(line.chunk);
    if (chunk == nullptr || chunk->nodes.size() < 8) {
        return false;
    }
    // The nodes in the port's frame.
    std::vector<Point> poly;
    buildChunkPolyline(line, *chunk, &poly);

    // The nearest node.
    size_t nearest = 0;
    double nearestDistance = 1e30;
    for (size_t i = 0; i < poly.size(); ++i) {
        const double dx = poly[i].x - carX;
        const double dy = poly[i].y - carY;
        const double d = dx * dx + dy * dy;
        if (d < nearestDistance) {
            nearestDistance = d;
            nearest = i;
        }
    }
    // The car has to be ON the line: a car 200 m from it is not on this
    // track's line, and aiming at it would be aiming at noise.
    const double distanceMetres = std::sqrt(nearestDistance) / 256.0;
    if (distanceMetres > 25.0) {
        return false;
    }

    // THE DIRECTION ALONG THE LINE.
    //
    // The residual CANNOT tell the two directions apart: a closed loop
    // traversed backwards has the same shape, so the fit picks the same
    // scale, the same axis variant and the same offset either way.  The node
    // ORDER is the only thing that says which way round the track the car
    // goes, and the spline does not necessarily store it that way round.
    //
    // MEASURED 2026-11-03 over the archived rounds: the pursuit's error sat
    // at ~180 degrees in 210 of 210 samples in q119, 71 of 113 in q121 and
    // 21 of 133 in q116 -- the look-ahead point was BEHIND the car, so the
    // controller was steering the wrong way.  The guest only counts a lap
    // when the car crosses the line, and the car left the track at the
    // first corner, so the lap counter never moved: that is where "laps =
    // -1" came from.
    //
    // So the direction comes from the CAR: the step away from the nearest
    // node has to agree with the car's heading.
    //
    // AND IT IS DECIDED ONCE.  MEASURED 2026-11-04 (q142): re-deciding it
    // every frame made the look-ahead point jump to the other side of the car
    // whenever the car's direction of travel reversed, and the error the
    // controller saw was the SUM of two flips (~180 + ~180), so it slammed
    // full lock first one way and then the other.  The car drove backwards,
    // which reversed the heading again, and the loop never settled: the car
    // reached 5.08 % of a lap and stopped there.
    //
    // The track does not change direction while the car drives on it, so the
    // decision belongs to the race.  `direction` carries it in and out; the
    // caller owns it, which keeps this function free of hidden state.
    int step = 1;
    if (direction != nullptr && *direction != 0) {
        // LOCKED.  The race already decided which way round the track is.
        step = (*direction > 0) ? 1 : -1;
    } else if (headingX != 0.0 || headingY != 0.0) {
        const size_t next = (nearest + 1) % poly.size();
        const size_t prev = (nearest + poly.size() - 1) % poly.size();
        const double fx = poly[next].x - poly[nearest].x;
        const double fy = poly[next].y - poly[nearest].y;
        const double bx = poly[prev].x - poly[nearest].x;
        const double by = poly[prev].y - poly[nearest].y;
        const double forward = fx * headingX + fy * headingY;
        const double backward = bx * headingX + by * headingY;
        // The direction whose step agrees with the heading wins.  When the
        // heading is unknown, or the two agree to within a hair, the
        // spline's own order is kept.
        if (backward > forward) {
            step = -1;
        }
        if (direction != nullptr) {
            *direction = step;
        }
    }

    // The look-ahead point: L metres further along the loop, IN THE
    // DIRECTION THE CAR IS TRAVELLING.
    double travelled = 0.0;
    size_t index = nearest;
    for (size_t guard = 0; guard < poly.size(); ++guard) {
        const size_t next = (step > 0) ? ((index + 1) % poly.size())
                                        : ((index + poly.size() - 1) % poly.size());
        const double dx = poly[next].x - poly[index].x;
        const double dy = poly[next].y - poly[index].y;
        const double segment = std::sqrt(dx * dx + dy * dy) / 256.0;
        if (travelled + segment >= lookAheadMetres) {
            const double remaining = lookAheadMetres - travelled;
            const double t = segment > 0.0 ? remaining / segment : 0.0;
            *targetX = poly[index].x + (poly[next].x - poly[index].x) * t;
            *targetY = poly[index].y + (poly[next].y - poly[index].y) * t;
            return true;
        }
        travelled += segment;
        index = next;
    }
    return false;
}

// ---------------------------------------------------------------------------
// THE LOOK-AHEAD DISTANCE, AND WHY IT HAS TO FOLLOW THE CURVATURE.
//
// MEASURED 2026-11-04 (q147): the port was moved onto the track's own racing
// line (ai_spline) instead of the centre line, and the car's distance from
// the CENTRE line dropped from 2.92-8.25 m to 0.32 m -- the car was finally
// on the right line.  And the lap progress got WORSE: 10.35 % -> 5.14 %.
//
// The reason was the look-ahead.  It was a CONSTANT 20 m, and on the racing
// line's sharp corners 20 m ahead is a long way round a corner the car is
// still entering: the measured pursuit error on those corners was 0.7 rad
// (40 degrees).  The controller steers by AIMING AT A POINT, so a point
// 40 degrees off is a steering command 40 degrees wrong, and the car drove
// off the road at the first real corner.
//
// The fix is the ordinary one: a look-ahead that SHORTENS as the corner
// tightens.  A long look-ahead is right on a straight (it damps the
// steering and keeps the car on the line); a short one is right in a corner
// (it aims at the part of the line the car is actually about to drive).
//
// The measure is deliberately simple and is stated here so that it can be
// argued with: the line is walked forward from the car's nearest node, and
// the ARC LENGTH at which the accumulated heading change first exceeds the
// threshold is the corner's scale.  A straight never reaches the threshold,
// so the walk reaches the maximum look-ahead; a hairpin reaches it in a
// couple of metres.
//
// The curve is measured over the arc AHEAD, in the direction the car is
// travelling, so the answer is the same whichever way round the track goes.
// ---------------------------------------------------------------------------
constexpr double kCornerHeadingThresholdRad = 0.35;  // ~20 degrees
constexpr double kMinLookAheadMetres = 4.0;
constexpr double kMaxLookAheadMetres = 20.0;

// The heading of the segment from `from` to `to`, in radians.
double segmentHeading(const Point& from, const Point& to) {
    return std::atan2(to.y - from.y, to.x - from.x);
}

// The smallest non-zero angle between two headings, in radians.
//
// M_PI IS NOT USED: it is not in the C++ standard, and the host self-test
// compiles this file with -std=c++17 and no GNU extensions, where M_PI is
// only there if <cmath> happens to define it.
constexpr double kPi = 3.14159265358979323846;

double headingDifference(double a, double b) {
    double d = std::fabs(a - b);
    while (d > kPi) {
        d -= 2.0 * kPi;
    }
    while (d < 0.0) {
        d += 2.0 * kPi;
    }
    return d;
}

// The look-ahead distance for the car at `nearest` on `poly`, walking in
// `step`.  Returns kMaxLookAheadMetres when the road ahead is straight.
//
// THE ARC LENGTH, NOT THE ANGLE.
//
// The comment above says what this returns: "the ARC LENGTH at which the
// accumulated heading change first exceeds the threshold is the corner's
// scale".  The implementation used to walk the same path and return
// `accumulated * 8.0` -- the ANGLE, scaled by a magic number.  That is not
// the same thing, and it is wrong in the direction that matters:
//
//   * on a straight, the walk runs a long way before the turn reaches the
//     threshold, and the angle is still only just over 0.35 rad, so the
//     answer was ~2.8 m -- clamped to the 4 m minimum.  The straight got the
//     SHORTEST look-ahead of anywhere on the track, which is exactly where
//     the steering noise lives;
//   * in a tight corner the threshold is crossed inside one segment, so the
//     angle overshoots further and the answer got LONGER -- the opposite of
//     the intent, and the same failure q147 measured (a long look-ahead into
//     a corner is a steering command tens of degrees wrong).
//
// ⇒ The arc length walked is what bounds the pursuit error: aiming at a
// point L metres ahead along the road gives an error of about the road's
// turn over that arc, so the distance at which the turn reaches the
// threshold IS the look-ahead that keeps the error under the threshold.
// That is the law the comment states, and it is now the law the code obeys.
double curvatureLookAheadMetres(const std::vector<Point>& poly, size_t nearest,
                                int step) {
    if (poly.size() < 8) {
        return kMaxLookAheadMetres;
    }
    const size_t count = poly.size();
    const size_t first = (step > 0) ? ((nearest + 1) % count)
                                     : ((nearest + count - 1) % count);
    double previousHeading = segmentHeading(poly[nearest], poly[first]);
    double accumulatedTurn = 0.0;
    double walkedMetres = 0.0;
    size_t index = first;
    // The walk is bounded: it looks at most a third of the lap ahead, which
    // is far more than any corner needs and keeps the cost constant.
    const size_t maxSteps = count / 3 + 4;
    for (size_t step_i = 0; step_i < maxSteps; ++step_i) {
        const size_t next = (step > 0) ? ((index + 1) % count)
                                       : ((index + count - 1) % count);
        const double dx = poly[next].x - poly[index].x;
        const double dy = poly[next].y - poly[index].y;
        const double segment = std::sqrt(dx * dx + dy * dy) / 256.0;
        if (segment <= 0.0) {
            break;
        }
        walkedMetres += segment;
        const double heading = segmentHeading(poly[index], poly[next]);
        accumulatedTurn += headingDifference(heading, previousHeading);
        previousHeading = heading;
        if (accumulatedTurn >= kCornerHeadingThresholdRad) {
            // The corner's scale: the distance at which the road ahead has
            // turned by the threshold.  Aim there, so the target is still
            // within the threshold of the car's heading.
            if (walkedMetres < kMinLookAheadMetres) {
                return kMinLookAheadMetres;
            }
            if (walkedMetres > kMaxLookAheadMetres) {
                return kMaxLookAheadMetres;
            }
            return walkedMetres;
        }
        index = next;
        if (index == nearest) {
            break;  // wrapped the whole loop without a corner
        }
    }
    return kMaxLookAheadMetres;
}

// THE TARGET, WITH A CURVATURE-ADAPTIVE LOOK-AHEAD.
//
// This is raceLineTarget() with one difference: the look-ahead distance is
// not a constant but is computed from the curvature of the road AHEAD.  The
// direction lock, the nearest-node search and the 25 m "the car has to be
// ON the line" guard are all exactly as they were -- they are the parts
// that were MEASURED to be right, and they are not being touched.
//
// The curvature is computed on the SAME polyline and in the SAME direction
// as the target, so the two cannot disagree about which way the car is
// going.
bool raceLineTargetAdaptive(const RaceLine& line, const SplineFile& spline,
                            double carX, double carY, double headingX, double headingY,
                            int* direction,
                            double* targetX, double* targetY,
                            double* lookAheadMetres,
                            bool adaptiveLookAhead,
                            double fixedLookAheadMetres) {
    if (!line.valid || targetX == nullptr || targetY == nullptr) {
        return false;
    }
    const SplineChunk* chunk = spline.chunk(line.chunk);
    if (chunk == nullptr || chunk->nodes.size() < 8) {
        return false;
    }
    std::vector<Point> poly;
    buildChunkPolyline(line, *chunk, &poly);

    size_t nearest = 0;
    double nearestDistance = 1e30;
    for (size_t i = 0; i < poly.size(); ++i) {
        const double dx = poly[i].x - carX;
        const double dy = poly[i].y - carY;
        const double d = dx * dx + dy * dy;
        if (d < nearestDistance) {
            nearestDistance = d;
            nearest = i;
        }
    }
    const double distanceMetres = std::sqrt(nearestDistance) / 256.0;
    if (distanceMetres > 25.0) {
        return false;
    }

    // THE DIRECTION, LOCKED ONCE -- unchanged from raceLineTarget().
    int step = 1;
    if (direction != nullptr && *direction != 0) {
        step = (*direction > 0) ? 1 : -1;
    } else if (headingX != 0.0 || headingY != 0.0) {
        const size_t count = poly.size();
        const size_t next = (nearest + 1) % count;
        const size_t prev = (nearest + count - 1) % count;
        const double fx = poly[next].x - poly[nearest].x;
        const double fy = poly[next].y - poly[nearest].y;
        const double bx = poly[prev].x - poly[nearest].x;
        const double by = poly[prev].y - poly[nearest].y;
        const double forward = fx * headingX + fy * headingY;
        const double backward = bx * headingX + by * headingY;
        if (backward > forward) {
            step = -1;
        }
        if (direction != nullptr) {
            *direction = step;
        }
    }

    // THE LOOK-AHEAD: from the curvature ahead, or the constant.
    //
    // The fixed branch is EXACTLY what raceLineTarget() did, so the "fixed"
    // mode is the old behaviour and not an approximation of it.
    const double lookAhead = adaptiveLookAhead
        ? curvatureLookAheadMetres(poly, nearest, step)
        : fixedLookAheadMetres;
    if (lookAheadMetres != nullptr) {
        *lookAheadMetres = lookAhead;
    }

    double travelled = 0.0;
    size_t index = nearest;
    const size_t count = poly.size();
    for (size_t guard = 0; guard < count; ++guard) {
        const size_t next = (step > 0) ? ((index + 1) % count)
                                       : ((index + count - 1) % count);
        const double dx = poly[next].x - poly[index].x;
        const double dy = poly[next].y - poly[index].y;
        const double segment = std::sqrt(dx * dx + dy * dy) / 256.0;
        if (travelled + segment >= lookAhead) {
            const double remaining = lookAhead - travelled;
            const double t = segment > 0.0 ? remaining / segment : 0.0;
            *targetX = poly[index].x + (poly[next].x - poly[index].x) * t;
            *targetY = poly[index].y + (poly[next].y - poly[index].y) * t;
            return true;
        }
        travelled += segment;
        index = next;
    }
    return false;
}

// THE CAR'S DISTANCE FROM A CHUNK, IN METRES.
//
// WHY IT IS HERE: the port follows the CENTRE line, and the track's own
// racing line (ai_spline) is a different line -- 67 m shorter a lap on Spa,
// and up to 8.25 m away from the centre line.  A car that hits the barriers
// ten times a race while sitting within 2 m of the centre line is on the
// wrong line, and the number that says so is the car's distance from the
// racing line.
//
// The distance is to the nearest SEGMENT, not the nearest node: the nodes
// are ~8 m apart, so a node distance would overstate it by metres.
//
// The transform is the one the fit established, so the two lines are in the
// same frame and the comparison is meaningful.
bool raceLineDistanceToChunk(const RaceLine& line, const SplineFile& spline,
                             const char* chunkName,
                             double carX, double carY, double* metres) {
    if (!line.valid || metres == nullptr || chunkName == nullptr) {
        return false;
    }
    const SplineChunk* chunk = spline.chunk(chunkName);
    if (chunk == nullptr || chunk->nodes.size() < 8) {
        return false;
    }
    std::vector<Point> poly;
    buildChunkPolyline(line, *chunk, &poly);
    double best = 1e30;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % poly.size()];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length2 = dx * dx + dy * dy;
        if (length2 <= 0.0) {
            continue;
        }
        double t = ((carX - a.x) * dx + (carY - a.y) * dy) / length2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const double qx = a.x + dx * t;
        const double qy = a.y + dy * t;
        const double d = (carX - qx) * (carX - qx) + (carY - qy) * (carY - qy);
        if (d < best) {
            best = d;
        }
    }
    if (best >= 1e30) {
        return false;
    }
    *metres = std::sqrt(best) / 256.0;
    return true;
}

}  // namespace rr3
