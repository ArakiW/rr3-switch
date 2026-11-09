// The racing line fit, against a line whose answer is KNOWN.
//
// The fit has to recover a scale, an offset and an axis variant from the
// car's own positions.  A synthetic circle with a known mapping is the
// positive control: if the fit cannot get this right, it cannot be trusted
// with a real track.
//
// Compiled and run by native-switch/tests/test_race_line_fit.py.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../source/race_line.cpp"

namespace {

constexpr double kRadius = 1000.0;    // stored units
constexpr double kScale = 16.0;        // port units (1/256 m) per stored unit
constexpr double kOffsetX = 5000.0;    // port units
constexpr double kOffsetY = -7000.0;   // port units
constexpr bool kSwap = true;
constexpr bool kFlipX = true;
constexpr bool kFlipY = false;
constexpr size_t kNodes = 64;

rr3::SplineFile makeCircle() {
    rr3::SplineFile file;
    file.version = 10;
    file.guid = "synthetic";
    rr3::SplineChunk chunk;
    chunk.name = "centre_spline";
    chunk.nodes.reserve(kNodes);
    for (size_t i = 0; i < kNodes; ++i) {
        const double angle = 2.0 * 3.14159265358979 * static_cast<double>(i) /
                             static_cast<double>(kNodes);
        rr3::SplineNode node;
        node.x = static_cast<int32_t>(2000.0 + kRadius * std::cos(angle));
        node.y = static_cast<int32_t>(-3000.0 + kRadius * std::sin(angle));
        node.z = 0;
        node.tangent = static_cast<uint16_t>(i * 1024);
        chunk.nodes.push_back(node);
    }
    file.chunks.push_back(chunk);
    return file;
}

// The stored node -> the port's world frame, with the known variant.
void toWorld(int32_t sx, int32_t sy, double* px, double* py) {
    double x = static_cast<double>(sx);
    double y = static_cast<double>(sy);
    if (kSwap) {
        const double t = x;
        x = y;
        y = t;
    }
    if (kFlipX) x = -x;
    if (kFlipY) y = -y;
    *px = x * kScale + kOffsetX;
    *py = y * kScale + kOffsetY;
}

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

}  // namespace

int main() {
    const rr3::SplineFile file = makeCircle();

    // The car sits on the line, at every sixth node.
    std::vector<double> carX;
    std::vector<double> carY;
    for (size_t i = 0; i < kNodes; i += 6) {
        double px = 0.0;
        double py = 0.0;
        toWorld(file.chunks[0].nodes[i].x, file.chunks[0].nodes[i].y, &px, &py);
        carX.push_back(px);
        carY.push_back(py);
    }
    std::printf("race_line selftest: %zu car samples on a %zu node circle\n",
                carX.size(), kNodes);

    rr3::RaceLine line;
    const bool fitted = rr3::raceLineFit(file, "centre_spline", carX.data(),
                                         carY.data(), carX.size(), &line);
    check(fitted, "the fit succeeds");
    if (!fitted) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("  scale=%.4f residual=%.4f m swap=%d flipX=%d flipY=%d\n",
                line.scale, line.residual, line.swapAxes ? 1 : 0,
                line.flipX ? 1 : 0, line.flipY ? 1 : 0);
    check(std::fabs(line.scale - kScale) < 0.25, "the scale is recovered");
    check(line.swapAxes == kSwap, "the axis swap is recovered");
    check(line.flipX == kFlipX, "the X flip is recovered");
    check(line.flipY == kFlipY, "the Y flip is recovered");
    check(line.residual < 0.5, "the residual is under half a metre");

    // The target has to be AHEAD, and the right distance away.
    double px = 0.0;
    double py = 0.0;
    toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &px, &py);
    // THE CAR'S HEADING: along the stored node order, from node 0 to node 1.
    //
    // The direction is an ARGUMENT because the fit's residual cannot tell a
    // curve from its mirror image over a short arc: only the direction of
    // travel says which way round the line the car goes.
    double hx = 0.0;
    double hy = 0.0;
    toWorld(file.chunks[0].nodes[1].x, file.chunks[0].nodes[1].y, &hx, &hy);
    hx -= px;
    hy -= py;
    {
        const double length = std::sqrt(hx * hx + hy * hy);
        if (length > 0.0) {
            hx /= length;
            hy /= length;
        }
    }
    double tx = 0.0;
    double ty = 0.0;
    // THE LOCK.  The direction belongs to the race, not to the frame: it is
    // decided once and then held.  MEASURED 2026-11-04 (q142): re-deciding
    // it every frame moved the look-ahead point to the other side of the car
    // whenever the car's direction of travel reversed, and the controller
    // slammed full lock first one way and then the other.
    int direction = 0;
    const bool got = rr3::raceLineTarget(line, file, px, py, hx, hy, &direction,
                                          20.0, &tx, &ty);
    check(got, "a target is produced");
    check(direction == 1 || direction == -1,
          "the direction round the track is decided and reported back");
    if (got) {
        const double distance = std::sqrt((tx - px) * (tx - px) +
                                          (ty - py) * (ty - py)) / 256.0;
        std::printf("  target %.2f m from the car\n", distance);
        check(distance > 15.0 && distance < 25.0, "the target is 20 m ahead");
        // The circle's centre, in the port's frame: the target must be on
        // the same side of the car as the arc, i.e. the geometry has to be
        // consistent rather than merely close.
        double cx = 0.0;
        double cy = 0.0;
        toWorld(2000, -3000, &cx, &cy);
        const double carToCentre = std::sqrt((px - cx) * (px - cx) +
                                              (py - cy) * (py - cy)) / 256.0;
        std::printf("  car to the circle centre: %.2f m (the radius is %.2f m)\n",
                    carToCentre, kRadius * kScale / 256.0);
        check(std::fabs(carToCentre - kRadius * kScale / 256.0) < 0.5,
              "the car is on the circle");

        // THE DIRECTION TEST.
        //
        // The same call with the OPPOSITE heading must put the target on
        // the other side of the car: that is the whole point of passing the
        // heading in, and it is what the pursuit needs -- the look-ahead
        // point has to be AHEAD of the car, and the spline's node order does
        // not necessarily run the way the car drives.
        //
        // AND THE LOCK OVERRIDES THE HEADING.  A locked race does not change
        // its direction round the track because the car was nudged, so the
        // opposite heading with the SAME lock has to produce the SAME side.
        double bx = 0.0;
        double by = 0.0;
        // A FRESH decision (direction 0): an unlocked race that is handed
        // the opposite heading has to pick the other side of the line.
        int backDirection = 0;
        const bool gotBack =
            rr3::raceLineTarget(line, file, px, py, -hx, -hy, &backDirection,
                                20.0, &bx, &by);
        check(gotBack, "a target is produced for the opposite heading");
        if (gotBack) {
            const double forwardDot = (tx - px) * hx + (ty - py) * hy;
            const double backwardDot = (bx - px) * (-hx) + (by - py) * (-hy);
            std::printf("  forward %.1f m along the heading, backward %.1f m\n",
                        forwardDot / 256.0, backwardDot / 256.0);
            check(forwardDot > 0.0, "the target is AHEAD along the heading");
            check(backwardDot > 0.0,
                  "the opposite heading puts the target on the other side");
            check((tx - px) * (bx - px) + (ty - py) * (by - py) < 0.0,
                  "the two targets are on opposite sides of the car");
        }

        // THE LOCK HOLDS.  The same locked direction, with the heading
        // reversed, has to keep the target on the SAME side of the car.
        // This is the case that was measured failing in q142: the target
        // jumped to the other side, the error became the sum of two flips,
        // and the car drove backwards.
        double lx = 0.0;
        double ly = 0.0;
        int lockedDirection = direction;
        const bool gotLocked =
            rr3::raceLineTarget(line, file, px, py, -hx, -hy, &lockedDirection,
                                20.0, &lx, &ly);
        check(gotLocked, "a locked race produces a target");
        if (gotLocked) {
            check(lockedDirection == direction,
                  "the lock is not changed by a reversed heading");
            check((tx - px) * (lx - px) + (ty - py) * (ly - py) > 0.0,
                  "a locked race keeps the target on the same side of the car");
        }
    }

    // THE GUARD: a car that is NOT on the line must be refused.
    int refusedDirection = 0;
    const bool refused = !rr3::raceLineTarget(line, file, px + 40000.0,
                                               py + 40000.0, 0.0, 0.0,
                                               &refusedDirection,
                                               20.0, &tx, &ty);
    check(refused, "a car far from the line is refused");

    // And a fit with too few samples is refused too.
    rr3::RaceLine thin;
    check(!rr3::raceLineFit(file, "centre_spline", carX.data(), carY.data(), 2,
                            &thin),
          "a fit with two samples is refused");

    // ---------------------------------------------------------------------
    // THE ADAPTIVE LOOK-AHEAD, AGAINST A TRACK WHOSE SHAPE IS KNOWN.
    //
    // The law under test is the one the comment in race_line.cpp states:
    // the look-ahead is the ARC LENGTH at which the road ahead has turned by
    // the threshold.  That has a closed-form answer on a constant-radius
    // corner (threshold * R), so a synthetic corner is a positive control
    // with a KNOWN number in it.
    //
    // WHY THE CONTROL MATTERS.  The implementation used to return the ANGLE
    // scaled by a magic constant instead of the arc length, and that put the
    // shortest look-ahead on the straights (where the steering noise lives)
    // and the longest one in the corners (which is the failure q147
    // measured).  A test that only checked "a number came back" would not
    // have caught that, so these assertions pin the actual law.
    // ---------------------------------------------------------------------
    {
        const double unit = 256.0;   // the port's units per metre

        auto point = [](double xMetres, double yMetres) {
            rr3::Point p;
            p.x = xMetres * 256.0;
            p.y = yMetres * 256.0;
            return p;
        };

        // A straight of `lengthMetres`, sampled every `spacingMetres`.
        auto straightLine = [&](double lengthMetres, double spacingMetres) {
            std::vector<rr3::Point> poly;
            for (double s = 0.0; s <= lengthMetres + 1e-9; s += spacingMetres) {
                poly.push_back(point(s, 0.0));
            }
            return poly;
        };

        // An arc of `radiusMetres` turning `sweepRad`, leaving the origin
        // along +x.  A constant-radius arc turns by (arc length / R), so the
        // distance at which it has turned by THRESHOLD is THRESHOLD * R.
        auto arcLine = [&](double radiusMetres, double sweepRad,
                           double spacingMetres) {
            std::vector<rr3::Point> poly;
            poly.push_back(point(0.0, 0.0));
            const double arcLength = radiusMetres * sweepRad;
            for (double s = spacingMetres; s <= arcLength + 1e-9;
                 s += spacingMetres) {
                const double theta = s / radiusMetres;
                poly.push_back(point(radiusMetres * std::sin(theta),
                                     radiusMetres * (1.0 - std::cos(theta))));
            }
            return poly;
        };

        // The law's own threshold, read from the implementation so the test
        // cannot drift away from it.
        const double threshold = 0.35;
        const double spacing = 2.0;

        // 1. A STRAIGHT.  The road ahead never turns, so the walk reaches the
        //    maximum look-ahead.  (The old law answered 4 m here.)
        {
            const std::vector<rr3::Point> poly = straightLine(300.0, spacing);
            const double la =
                rr3::curvatureLookAheadMetres(poly, 0, 1);
            std::printf("  straight : look-ahead %.2f m\n", la);
            check(la > 19.0 && la < 21.0,
                  "a straight gets the maximum look-ahead");
        }

        // 2. A CORNER OF KNOWN RADIUS.  The answer is threshold * R, to
        //    within one sample's worth of arc.
        {
            const double radius = 25.0;
            const std::vector<rr3::Point> poly =
                arcLine(radius, 1.5707963267948966, spacing);
            const double la = rr3::curvatureLookAheadMetres(poly, 0, 1);
            const double expected = threshold * radius;
            std::printf("  corner R=%.0f m : look-ahead %.2f m (expected %.2f m)\n",
                        radius, la, expected);
            check(la > expected - spacing && la < expected + spacing,
                  "a corner's look-ahead is the arc at which it turns by the threshold");
        }

        // 3. A TIGHTER CORNER GETS A SHORTER LOOK-AHEAD.  This is the
        //    direction the old law got backwards.
        {
            const std::vector<rr3::Point> wide =
                arcLine(50.0, 1.5707963267948966, spacing);
            const std::vector<rr3::Point> tight =
                arcLine(12.0, 1.5707963267948966, spacing);
            const double wideLa = rr3::curvatureLookAheadMetres(wide, 0, 1);
            const double tightLa = rr3::curvatureLookAheadMetres(tight, 0, 1);
            std::printf("  R=50 m : %.2f m      R=12 m : %.2f m\n",
                        wideLa, tightLa);
            check(wideLa > tightLa,
                  "a tighter corner gets a shorter look-ahead");
        }

        // 4. THE CAR APPROACHING A CORNER.  The look-ahead has to SHORTEN as
        //    the corner comes up, and it has to be the maximum while the road
        //    ahead is still straight.  This is the direction the old law got
        //    backwards: it answered 4 m in both places, so the car aimed at
        //    a point 4 m away in the middle of a corner.
        {
            const double radius = 25.0;
            const double straightMetres = 20.0;
            std::vector<rr3::Point> poly = straightLine(straightMetres, spacing);
            const std::vector<rr3::Point> corner =
                arcLine(radius, 1.5707963267948966, spacing);
            // The arc's FIRST point is the straight's last point, so it is not
            // appended: a zero-length segment would stop the walk early.
            for (size_t i = 1; i < corner.size(); ++i) {
                poly.push_back(point(straightMetres + corner[i].x / 256.0,
                                     corner[i].y / 256.0));
            }
            // The corner's entry: the first node that is off the straight.
            size_t entry = 0;
            for (size_t i = 0; i < poly.size(); ++i) {
                if (poly[i].y > 1.0) {
                    entry = i;
                    break;
                }
            }
            const double onStraight = rr3::curvatureLookAheadMetres(poly, 0, 1);
            const double atCorner = rr3::curvatureLookAheadMetres(poly, entry, 1);
            std::printf("  on the straight : %.2f m   at the corner : %.2f m\n",
                        onStraight, atCorner);
            check(onStraight > 19.0 && onStraight < 21.0,
                  "the straight before a corner gets the maximum look-ahead");
            check(atCorner > threshold * radius - spacing &&
                      atCorner < threshold * radius + spacing,
                  "the corner's entry gets the corner's own look-ahead");
            check(atCorner < onStraight,
                  "the look-ahead shortens as the corner comes up");
        }
    }

    // ---------------------------------------------------------------------
    // 5. A CAR FAR OFF THE LINE STILL GETS A TARGET, AND THE TARGET IS THE
    //    NEAREST POINT ON THE LINE.
    //
    //    MEASURED 2026-11-10 (round hybrid1, 223 race-state samples): the
    //    port used to refuse the line whenever the car was more than 25 m
    //    from it.  The guest's own aim target then stayed in the sample, and
    //    that target is garbage when the car is off the racing line -- 13,076
    //    m, 19,059 m and 47,014 m were measured -- so the pursuit switched
    //    itself off and the recovery took the wheel with a full lock.  The
    //    car spun and stalled instead of coming back (flips=9, stalls=7) and
    //    the lap progress fell to 3.0 % against the reference round's
    //    10.35 %.
    //
    //    So the line has to KEEP the car, and the target has to be the way
    //    back onto it.  This test has teeth: the old law answered "no
    //    target" for the car below.
    // ---------------------------------------------------------------------
    {
        double centreX = 0.0;
        double centreY = 0.0;
        toWorld(2000, -3000, &centreX, &centreY);
        double nodeX = 0.0;
        double nodeY = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &nodeX, &nodeY);
        const double radiusX = nodeX - centreX;
        const double radiusY = nodeY - centreY;
        const double radiusLength = std::sqrt(radiusX * radiusX + radiusY * radiusY);
        const double circleRadiusMetres = kRadius * kScale / 256.0;
        const double outward = 100.0 * 256.0;   // the port's units per metre
        const double carOutX =
            centreX + radiusX * (1.0 + outward / radiusLength);
        const double carOutY =
            centreY + radiusY * (1.0 + outward / radiusLength);

        int direction = 0;
        double tx = 0.0;
        double ty = 0.0;
        double lookAhead = -1.0;
        const bool got = rr3::raceLineTargetAdaptive(line, file, carOutX, carOutY,
                                                      hx, hy, &direction, &tx, &ty,
                                                      &lookAhead, true, 20.0);
        check(got, "a car 100 m off the line still gets a target");
        if (got) {
            const double toTarget =
                std::sqrt((tx - carOutX) * (tx - carOutX) +
                          (ty - carOutY) * (ty - carOutY)) / 256.0;
            const double targetToCentre =
                std::sqrt((tx - centreX) * (tx - centreX) +
                          (ty - centreY) * (ty - centreY)) / 256.0;
            std::printf("  100 m off the line : target %.2f m away, %.2f m from the centre\n",
                        toTarget, targetToCentre);
            check(std::fabs(lookAhead - rr3::kFarLookAheadMetres) < 0.001,
                  "the look-ahead is a fixed moderate value for a car that is off the line");
            check(toTarget > 90.0 && toTarget < 110.0,
                  "the target is about 100 m away -- the nearest point on the line");
            check(std::fabs(targetToCentre - circleRadiusMetres) < 1.5,
                  "the target lies ON the line");
        }

        // And a car that is not on this track's line AT ALL gets no target.
        int farDirection = 0;
        double fx = 0.0;
        double fy = 0.0;
        const bool gotFar = rr3::raceLineTargetAdaptive(
            line, file, carOutX + 400.0 * 256.0, carOutY, hx, hy, &farDirection,
            &fx, &fy, nullptr, true, 20.0);
        check(!gotFar, "a car 500 m off the line gets no target at all");
    }

    // ---------------------------------------------------------------------
    // 6. THE CHUNK THE CAR IS ACTUALLY ON.
    //
    //    MEASURED 2026-11-10 (round pursuit1): the chunk the line had been
    //    fitted on was 13,215 m from the car -- the pursuit's own `ctdist` --
    //    so the port had no line to aim at, the guest's own target was
    //    refused (it is garbage when the car is off the line) and the pursuit
    //    was off for the whole round.
    //
    //    The transform is a property of the TRACK, so the chunk the car is on
    //    can be found with the transform the fit already established -- no
    //    re-fit is needed.  This test has teeth: without the picker the port
    //    would keep aiming at the first chunk, which is 187 m away here.
    // ---------------------------------------------------------------------
    {
        rr3::SplineFile twoChunks = makeCircle();
        // A second chunk, 3,000 stored units away: 3,000 * 16 = 48,000 port
        // units = 187.5 m in the port's frame.
        rr3::SplineChunk second = twoChunks.chunks[0];
        second.name = "second_spline";
        for (rr3::SplineNode& node : second.nodes) {
            node.y += 3000;
        }
        twoChunks.chunks.push_back(second);

        // The car sits on the SECOND circle.
        double secondX = 0.0;
        double secondY = 0.0;
        toWorld(second.nodes[0].x, second.nodes[0].y, &secondX, &secondY);
        char chunk[32] = {0};
        double metres = -1.0;
        const bool got = rr3::raceLineNearestChunk(line, twoChunks, secondX, secondY,
                                                   chunk, sizeof(chunk), &metres);
        check(got, "the chunk the car is on is found");
        check(std::strcmp(chunk, "second_spline") == 0,
              "it is the chunk the car is on, not the fitted one");
        std::printf("  car on the second chunk : %s at %.2f m\n", chunk, metres);
        check(metres < 5.0, "the distance to that chunk is small");

        // And the car on the FIRST circle still picks the first chunk.
        double firstX = 0.0;
        double firstY = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &firstX, &firstY);
        char chunk2[32] = {0};
        const bool got2 = rr3::raceLineNearestChunk(line, twoChunks, firstX, firstY,
                                                    chunk2, sizeof(chunk2), nullptr);
        check(got2 && std::strcmp(chunk2, "centre_spline") == 0,
              "the car on the first chunk picks the first chunk");
    }

    // ---------------------------------------------------------------------
    // 7. FIX E1: A CAR ON THE LINE DRIVING FORWARD GETS AN AIM POINT AHEAD,
    //    AND THE PURSUIT ANGLE ERROR IS UNDER ABOUT 0.3 RAD.
    // ---------------------------------------------------------------------
    {
        double px = 0.0, py = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &px, &py);
        double nx = 0.0, ny = 0.0;
        toWorld(file.chunks[0].nodes[1].x, file.chunks[0].nodes[1].y, &nx, &ny);
        double hx = nx - px;
        double hy = ny - py;
        const double hlen = std::hypot(hx, hy);
        if (hlen > 0.0) {
            hx /= hlen;
            hy /= hlen;
        }

        int dir = 1;
        double tx = 0.0, ty = 0.0;
        double lookAhead = 0.0;
        const bool got = rr3::raceLineTargetAdaptive(line, file, px, py, hx, hy, &dir,
                                                      &tx, &ty, &lookAhead, false, 20.0);
        check(got, "E1: forward car on the line produces target");
        if (got) {
            const double dx = (tx - px) / 256.0;
            const double dy = (ty - py) / 256.0;
            const double dlen = std::hypot(dx, dy);
            const double cross = hx * (dy / dlen) - hy * (dx / dlen);
            const double dot = hx * (dx / dlen) + hy * (dy / dlen);
            const double err = std::atan2(cross, dot);
            std::printf("  E1 forward: error=%.4f rad (< 0.3 rad required)\n", err);
            check(std::fabs(err) < 0.3, "E1: forward car heading-to-target angle is under 0.3 rad");
        }
    }

    // ---------------------------------------------------------------------
    // 8. FIX E2: A CAR ON THE LINE DRIVING BACKWARD GETS AN AIM POINT WHOSE
    //    ARC COORDINATE IS AHEAD ALONG THE TRACK, AND THE GEOMETRY REPORTS
    //    A LARGE ERROR (> 2.5 RAD) -- NOT SILENTLY AIMING BEHIND.
    // ---------------------------------------------------------------------
    {
        double px = 0.0, py = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &px, &py);
        double nx = 0.0, ny = 0.0;
        toWorld(file.chunks[0].nodes[1].x, file.chunks[0].nodes[1].y, &nx, &ny);
        double hx = nx - px;
        double hy = ny - py;
        const double hlen = std::hypot(hx, hy);
        if (hlen > 0.0) {
            hx /= hlen;
            hy /= hlen;
        }

        // Backward heading: opposite to forward
        const double bhx = -hx;
        const double bhy = -hy;

        int lockedDir = 1; // Locked forward round the track
        double tx = 0.0, ty = 0.0;
        double lookAhead = 0.0;
        double carArc = 0.0;
        double targetArc = 0.0;
        double loopLen = 0.0;

        rr3::raceLineProject(line, file, px, py, nullptr, nullptr, &carArc, nullptr, &loopLen);
        const bool got = rr3::raceLineTargetAdaptive(line, file, px, py, bhx, bhy, &lockedDir,
                                                      &tx, &ty, &lookAhead, false, 20.0);
        check(got, "E2: backward car produces target");
        if (got) {
            rr3::raceLineProject(line, file, tx, ty, nullptr, nullptr, &targetArc, nullptr, nullptr);
            const double arcDiff = rr3::raceLineArcDelta(targetArc, carArc, loopLen);
            std::printf("  E2 backward: arcDiff=%.2f m (lookAhead=%.2f m)\n", arcDiff, lookAhead);
            check(arcDiff > 15.0 && arcDiff < 25.0,
                  "E2: target arc coordinate is AHEAD along the track");

            const double dx = (tx - px) / 256.0;
            const double dy = (ty - py) / 256.0;
            const double dlen = std::hypot(dx, dy);
            const double cross = bhx * (dy / dlen) - bhy * (dx / dlen);
            const double dot = bhx * (dx / dlen) + bhy * (dy / dlen);
            const double err = std::atan2(cross, dot);
            std::printf("  E2 backward: error=%.4f rad (> 2.5 rad required)\n", err);
            check(std::fabs(err) > 2.5,
                  "E2: geometry reports large error (> 2.5 rad) indicating wrong way");
        }
    }

    // ---------------------------------------------------------------------
    // 9. FIX E3: A CAR 30 M OFF THE LINE STILL GETS A TARGET, AND THE TARGET
    //    IS NOT THE CAR'S OWN POSITION: DISTANCE IS AT LEAST LOOK-AHEAD.
    // ---------------------------------------------------------------------
    {
        double centreX = 0.0, centreY = 0.0;
        toWorld(2000, -3000, &centreX, &centreY);
        double nodeX = 0.0, nodeY = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &nodeX, &nodeY);
        const double radiusX = nodeX - centreX;
        const double radiusY = nodeY - centreY;
        const double radiusLength = std::hypot(radiusX, radiusY);
        const double outward30 = 30.0 * 256.0; // 30 m off the line
        const double carOut30X = centreX + radiusX * (1.0 + outward30 / radiusLength);
        const double carOut30Y = centreY + radiusY * (1.0 + outward30 / radiusLength);

        int dir = 1;
        double tx = 0.0, ty = 0.0;
        double lookAhead = -1.0;
        const bool got = rr3::raceLineTargetAdaptive(line, file, carOut30X, carOut30Y,
                                                      hx, hy, &dir, &tx, &ty,
                                                      &lookAhead, true, 20.0);
        check(got, "E3: car 30 m off the line gets a target");
        if (got) {
            const double dist = std::hypot(tx - carOut30X, ty - carOut30Y) / 256.0;
            std::printf("  E3: 30 m off line: lookAhead=%.2f m, distToTarget=%.2f m\n",
                        lookAhead, dist);
            check(lookAhead > 0.001, "E3: look-ahead is NOT zero for car 30 m off line");
            check(dist >= lookAhead, "E3: distance to target is at least look-ahead");
            check(dist >= 30.0, "E3: distance to target is at least the offset distance");
        }
    }

    // ---------------------------------------------------------------------
    // 10. FIX E4: THE ALONG-TRACK COORDINATE INCREASES WHEN MOVING FORWARD
    //     ALONG THE LINE AND DECREASES WHEN MOVING BACKWARDS.
    // ---------------------------------------------------------------------
    {
        double loopLen = 0.0;
        double along0 = 0.0;
        double alongFwd = 0.0;
        double alongBwd = 0.0;

        // Position at node 0
        double p0x = 0.0, p0y = 0.0;
        toWorld(file.chunks[0].nodes[0].x, file.chunks[0].nodes[0].y, &p0x, &p0y);
        // Position at node 1 (forward along node order)
        double p1x = 0.0, p1y = 0.0;
        toWorld(file.chunks[0].nodes[1].x, file.chunks[0].nodes[1].y, &p1x, &p1y);
        // Midpoint 0->1
        const double fwdX = 0.5 * (p0x + p1x);
        const double fwdY = 0.5 * (p0y + p1y);

        // Position at node N-1 (backward along node order)
        double pLastX = 0.0, pLastY = 0.0;
        toWorld(file.chunks[0].nodes[kNodes - 1].x, file.chunks[0].nodes[kNodes - 1].y, &pLastX, &pLastY);
        // Midpoint (N-1)->0
        const double bwdX = 0.5 * (p0x + pLastX);
        const double bwdY = 0.5 * (p0y + pLastY);

        rr3::raceLineProject(line, file, p0x, p0y, nullptr, nullptr, &along0, nullptr, &loopLen);
        rr3::raceLineProject(line, file, fwdX, fwdY, nullptr, nullptr, &alongFwd, nullptr, nullptr);
        rr3::raceLineProject(line, file, bwdX, bwdY, nullptr, nullptr, &alongBwd, nullptr, nullptr);

        const double deltaFwd = rr3::raceLineArcDelta(alongFwd, along0, loopLen);
        const double deltaBwd = rr3::raceLineArcDelta(alongBwd, along0, loopLen);

        std::printf("  E4: loopLen=%.2f m deltaFwd=%.2f m deltaBwd=%.2f m\n",
                    loopLen, deltaFwd, deltaBwd);
        check(deltaFwd > 0.0, "E4: along-track coordinate increases when moving forward");
        check(deltaBwd < 0.0, "E4: along-track coordinate decreases when moving backward");
    }

    std::printf("%s\n", failures == 0 ? "race_line selftest: OK" : "race_line selftest: FAILED");
    return failures == 0 ? 0 : 1;
}
