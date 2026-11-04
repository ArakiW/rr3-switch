#!/usr/bin/env python3
"""Is the car stuck on the CENTRE line, or on the racing line?

WHY THIS EXISTS
---------------
The port's pure-pursuit controller follows the spline chunk named
"centre_spline".  MEASURED 2026-11-04 (Spa, tools/spline_chunks.py):

    centre_spline  441 nodes  7016.9 m a lap
    ai_spline      882 nodes  6949.8 m a lap   (67.0 m shorter = the racing line)
    deviation      median 2.92 m, p90 4.62 m, max 8.25 m

So the racing line cuts the corners and the centre line does not, and a car
that follows the centre line drives on the OUTSIDE of every corner.  That is
a plausible reason for the ten barrier hits a race.

BUT THAT IS A HYPOTHESIS, and it is only worth a build if the car is
actually stuck ON THE CENTRE LINE.  If it is already on the racing line, then
switching the chunk would change nothing and the barrier hits have some
other cause.

THIS ANSWERS IT FROM DATA THAT ALREADY EXISTS.  The guest-view log has the
car's world position (cpos) every sample.  The port's fit maps the stored
centre spline onto the car's path, so re-deriving that transform here gives
the racing line in the SAME world frame, and the car's distance to each line
follows.

THE TRANSFORM
-------------
The port searches eight axis variants (swap / flipX / flipY) and a scale
grid of powers of two, and aligns the winner onto the car's positions with a
few ICP iterations (source/race_line.cpp).  This script does the same
search, because the transform has to be the SAME one or the comparison means
nothing.

USAGE
-----
    python tools/racing_line_check.py <track.spline> <guest-view.log>
"""

import argparse
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spline_chunks import parse  # noqa: E402

# The port's own scales: the guest multiplies the stored coordinates by 16
# (ELF 0x1425d54), and the fit searches the powers of two around it.
SCALES = [1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0]

RACE_RE = re.compile(r"phase=race-state\b")
METRICS_RE = re.compile(r"metrics=1")
CPOS_RE = re.compile(r"cpos=(-?\d+),(-?\d+),(-?\d+)")
FRAME_RE = re.compile(r"\bf=(\d+)")
PERR_RE = re.compile(r"perr=(-?[0-9.]+)")
LRES_RE = re.compile(r"lres=(-?[0-9.]+)")
BRK_RE = re.compile(r"brk=(\d+)")
SNODE_RE = re.compile(r"snode=(\d+)")
CTDIST_RE = re.compile(r"ctdist=(-?[0-9.]+)")


def read_car_samples(path):
    """The car's world position and the pursuit's own numbers, per sample."""
    samples = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not RACE_RE.search(line) or not METRICS_RE.search(line):
                continue
            pos = CPOS_RE.search(line)
            if pos is None:
                continue
            sample = {
                "frame": int(FRAME_RE.search(line).group(1)) if FRAME_RE.search(line) else 0,
                "x": float(pos.group(1)),
                "y": float(pos.group(2)),
                "z": float(pos.group(3)),
            }
            for name, regex in (("perr", PERR_RE), ("lres", LRES_RE),
                                ("brk", BRK_RE), ("snode", SNODE_RE),
                                ("ctdist", CTDIST_RE)):
                found = regex.search(line)
                if found:
                    sample[name] = float(found.group(1))
            samples.append(sample)
    return samples


def variants():
    """The eight axis variants the fit tries: (swap, flipX, flipY)."""
    out = []
    for swap in (False, True):
        for flip_x in (False, True):
            for flip_y in (False, True):
                out.append((swap, flip_x, flip_y))
    return out


def transform_nodes(nodes, scale, swap, flip_x, flip_y):
    out = []
    for x, y in nodes:
        if swap:
            x, y = y, x
        if flip_x:
            x = -x
        if flip_y:
            y = -y
        out.append((x * scale, y * scale))
    return out


def distance_to_polyline(poly, px, py):
    """The distance from a point to a CLOSED polyline, in segments."""
    best = None
    count = len(poly)
    for i in range(count):
        x0, y0 = poly[i]
        x1, y1 = poly[(i + 1) % count]
        dx = x1 - x0
        dy = y1 - y0
        length2 = dx * dx + dy * dy
        if length2 <= 0.0:
            continue
        t = ((px - x0) * dx + (py - y0) * dy) / length2
        if t < 0.0:
            t = 0.0
        elif t > 1.0:
            t = 1.0
        qx = x0 + dx * t
        qy = y0 + dy * t
        d = (px - qx) ** 2 + (py - qy) ** 2
        if best is None or d < best:
            best = d
    return math.sqrt(best) if best is not None else float("inf")


def align(poly, car_x, car_y, iterations=12):
    """ICP: move the polyline onto the car's path.  Returns (dx, dy, residual)."""
    dx = 0.0
    dy = 0.0
    residual = float("inf")
    for _ in range(iterations):
        moved = [(x + dx, y + dy) for x, y in poly]
        total = 0.0
        for px, py in zip(car_x, car_y):
            total += distance_to_polyline(moved, px, py)
        residual = total / len(car_x)
        if residual < 1e-6:
            break
        # Move by the mean of the differences to the nearest points.
        sum_x = 0.0
        sum_y = 0.0
        for px, py in zip(car_x, car_y):
            best = None
            best_d = None
            count = len(moved)
            for i in range(count):
                x0, y0 = moved[i]
                x1, y1 = moved[(i + 1) % count]
                ex = x1 - x0
                ey = y1 - y0
                length2 = ex * ex + ey * ey
                if length2 <= 0.0:
                    continue
                t = ((px - x0) * ex + (py - y0) * ey) / length2
                if t < 0.0:
                    t = 0.0
                elif t > 1.0:
                    t = 1.0
                qx = x0 + ex * t
                qy = y0 + ey * t
                d = (px - qx) ** 2 + (py - qy) ** 2
                if best_d is None or d < best_d:
                    best_d = d
                    best = (qx, qy)
            if best is not None:
                sum_x += px - best[0]
                sum_y += py - best[1]
        dx += sum_x / len(car_x)
        dy += sum_y / len(car_x)
    return dx, dy, residual


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("spline")
    parser.add_argument("log")
    parser.add_argument("--reference", default="centre_spline")
    parser.add_argument("--racing", default="ai_spline")
    args = parser.parse_args()

    with open(args.spline, "rb") as handle:
        _, _, chunks = parse(handle.read())
    by_name = {c.name: c for c in chunks}
    if args.reference not in by_name or args.racing not in by_name:
        print("the file does not have both %s and %s" % (args.reference, args.racing))
        return 1

    samples = read_car_samples(args.log)
    if len(samples) < 8:
        print("only %d race samples with a position in the log" % len(samples))
        return 1
    car_x = [s["x"] for s in samples]
    car_y = [s["y"] for s in samples]

    # The same search the port does.
    best = None
    for scale in SCALES:
        for swap, flip_x, flip_y in variants():
            poly = transform_nodes(by_name[args.reference].nodes, scale, swap, flip_x, flip_y)
            dx, dy, residual = align(poly, car_x, car_y)
            if best is None or residual < best["residual"]:
                best = {"scale": scale, "swap": swap, "flipX": flip_x,
                        "flipY": flip_y, "dx": dx, "dy": dy,
                        "residual": residual, "poly": poly}
    print("TRANSFORM (fitted to the car's own path, like the port's fit):")
    print("  scale %.1f  swap %d  flipX %d  flipY %d" %
          (best["scale"], best["swap"], best["flipX"], best["flipY"]))
    print("  offset %.0f, %.0f port units" % (best["dx"], best["dy"]))
    print("  residual %.2f port units = %.2f m" % (best["residual"], best["residual"] / 256.0))

    # THE SELF-CHECK.
    #
    # The port fits the SAME chunk onto the SAME car's path and reports its
    # own residual as `lres` in the log.  If this script's fit does not agree
    # with that number, this script's transform is not the port's transform,
    # and every distance derived from it is void.  A script that prints
    # numbers anyway is worse than no script: the first version of this one
    # printed a residual of 19.22 m where the port says 2.01 m, and the
    # distances it derived from that were nonsense.
    port_lres = [s["lres"] for s in samples if "lres" in s and s["lres"] > 0.0]
    if not port_lres:
        print("")
        print("  NOTE: the log has no positive lres to check the fit against, so")
        print("        this comparison cannot be trusted.  Treat the numbers")
        print("        below as unverified.")
    else:
        port_median = sorted(port_lres)[len(port_lres) // 2]
        fit_metres = best["residual"] / 256.0
        print("")
        print("  THE PORT'S OWN lres for the same chunk: median %.2f m" % port_median)
        if fit_metres > max(2.0 * port_median, port_median + 1.0):
            print("")
            print("  THIS FIT DOES NOT AGREE WITH THE PORT'S lres (%.2f m against %.2f m)."
                  % (fit_metres, port_median))
            print("  So this transform is not the port's transform, and every")
            print("  distance below is VOID.  The port measures the same thing")
            print("  directly now (the `rres` field), so use that instead.")
            return 1
        print("  the fit agrees with it, so the transform is the port's.")
    print("")

    racing_poly = [(x + best["dx"], y + best["dy"]) for x, y in
                   transform_nodes(by_name[args.racing].nodes, best["scale"],
                                   best["swap"], best["flipX"], best["flipY"])]

    rows = []
    for sample in samples:
        d_ref = distance_to_polyline(best["poly"], sample["x"], sample["y"]) / 256.0
        d_race = distance_to_polyline(racing_poly, sample["x"], sample["y"]) / 256.0
        rows.append((sample, d_ref, d_race))

    braking = [r for r in rows if r[0].get("brk") == 1]
    print("SAMPLES: %d total, %d with the brake held (the recovery)" %
          (len(rows), len(braking)))
    if braking:
        d_ref = [r[1] for r in braking]
        d_race = [r[2] for r in braking]
        print("  while braking: distance to the CENTRE line  median %.2f m  max %.2f m" %
              (sorted(d_ref)[len(d_ref) // 2], max(d_ref)))
        print("  while braking: distance to the RACING line  median %.2f m  max %.2f m" %
              (sorted(d_race)[len(d_race) // 2], max(d_race)))
    d_race_all = sorted(r[2] for r in rows)
    print("  over the whole race: distance to the RACING line  median %.2f m  p90 %.2f m  max %.2f m"
          % (d_race_all[len(d_race_all) // 2],
             d_race_all[int(len(d_race_all) * 0.90)], d_race_all[-1]))

    print("")
    print("  frame   brk  toCentre  toRacing   perr    lres   ctdist")
    step = max(1, len(rows) // 25)
    for index, (sample, d_ref, d_race) in enumerate(rows):
        if index % step and index != len(rows) - 1:
            continue
        print("  %6d %4d %9.2f %9.2f %7.3f %6.2f %8.2f" %
              (sample["frame"], int(sample.get("brk", 0)), d_ref, d_race,
               sample.get("perr", 0.0), sample.get("lres", 0.0),
               sample.get("ctdist", 0.0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
