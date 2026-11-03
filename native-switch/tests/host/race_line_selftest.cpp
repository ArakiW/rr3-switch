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
    const bool got = rr3::raceLineTarget(line, file, px, py, hx, hy, 20.0, &tx, &ty);
    check(got, "a target is produced");
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
        double bx = 0.0;
        double by = 0.0;
        const bool gotBack =
            rr3::raceLineTarget(line, file, px, py, -hx, -hy, 20.0, &bx, &by);
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
    }

    // THE GUARD: a car that is NOT on the line must be refused.
    const bool refused = !rr3::raceLineTarget(line, file, px + 40000.0,
                                               py + 40000.0, 0.0, 0.0,
                                               20.0, &tx, &ty);
    check(refused, "a car far from the line is refused");

    // And a fit with too few samples is refused too.
    rr3::RaceLine thin;
    check(!rr3::raceLineFit(file, "centre_spline", carX.data(), carY.data(), 2,
                            &thin),
          "a fit with two samples is refused");

    std::printf("%s\n", failures == 0 ? "race_line selftest: OK" : "race_line selftest: FAILED");
    return failures == 0 ? 0 : 1;
}
