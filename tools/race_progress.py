#!/usr/bin/env python3
"""Race progress from the guest-view log's race-state lines.

WHAT THIS IS FOR
----------------
The port's race-state probe writes one line per sample with the guest's own
RuleSetModifier_RaceMetrics.  Two questions have been asked about it, and
both need the SAME instrument:

  1. Does the car make progress around the track?  `total` is the distance
     covered in spline units; one lap is `lapd` (measured at 1,806,336 units).
     `laps` stays -1 until the guest itself counts a lap, so "laps == -1" is
     not a verdict -- "total / lapd" is the verdict.
  2. Is the pursuit line actually being followed?  `perr` is the angle
     between the car's heading and the look-ahead target.  Before the
     heading fix it sat at ~180 degrees (the target was BEHIND the car);
     after the fix it must sit near 0.

The point of this script is that a SINGLE sample proves nothing.  A car that
moves 5% of a lap in one sample and 90% in the next is a car that made
progress; a car that is frozen at the same `total` for 300 samples is
wedged.  So the script prints the whole curve, its slope, and the stall
runs -- not a single number.

USAGE
-----
    python tools/race_progress.py <guest-view.log> [--json]
"""

import argparse
import json
import re
import sys

# The fields we need out of a race-state line.  Every one of them is optional
# in the sense that a line without it is still a sample; the script reports
# what it found instead of assuming.
F_INT = ("f", "total", "lapd", "snode", "sdist", "speed", "time", "stalls",
         "flips", "line", "pur", "drive", "ctok", "tsok", "posok", "metrics")
F_FLOAT = ("lapF", "totalF", "speedF", "timeF", "rate", "prog", "lres", "perr",
           "steer", "mag", "best", "ctdist")

# `laps` is unsigned in the guest and -1 shows up as 4294967295.
LAPS_UNSET = 4294967295

SAMPLE_RE = re.compile(r"phase=race-state\b")
FIELD_RE = re.compile(r"(\w+)=(-?[0-9]+\.?[0-9]*)")


def parse_line(line):
    """Pull the fields out of one race-state line, or None if it is not one."""
    if not SAMPLE_RE.search(line):
        return None
    out = {}
    for key, value in FIELD_RE.findall(line):
        if key in F_INT:
            try:
                out[key] = int(value)
            except ValueError:
                pass
        elif key in F_FLOAT:
            try:
                out[key] = float(value)
            except ValueError:
                pass
        elif key == "laps":
            try:
                out["laps"] = int(value)
            except ValueError:
                pass
    if "f" not in out:
        return None
    return out


def read_samples(path):
    samples = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            sample = parse_line(line)
            if sample is not None:
                samples.append(sample)
    return samples


def lap_fraction(sample):
    """Fraction of a lap covered, using the guest's own lap length.

    `lapd` is the distance the guest says is left to the lap node, so the
    denominator has to be the FULL lap.  We take it from the largest lapd
    seen, because lapd shrinks towards zero as the lap is completed.
    """
    return None


def longest_stall(samples, key="total"):
    """The longest run of consecutive samples where `key` did not advance.

    A stall run is not proof of a wedge -- the guest can sit still for a few
    samples at a corner -- but a stall run that never ends IS the wedge.
    """
    best = (0, -1, -1)
    run_start = None
    last = None
    for index, sample in enumerate(samples):
        value = sample.get(key)
        if value is None:
            continue
        if last is not None and value > last + 1:
            run_start = index
        elif last is None:
            run_start = index
        length = index - (run_start if run_start is not None else index) + 1
        if length > best[0]:
            best = (length, run_start if run_start is not None else index, index)
        last = value
    return best


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--all", action="store_true",
                        help="print every sample, not every twentieth")
    args = parser.parse_args()

    samples = read_samples(args.log)
    if not samples:
        print("no race-state samples in %s" % args.log)
        return 1

    with_metrics = [s for s in samples if s.get("metrics") == 1]
    if not with_metrics:
        print("race-state lines: %d, but none had metrics=1" % len(samples))
        print("=> the guest had no live RaceMetrics object in this run")
        return 1

    # The full lap: the biggest lapd we ever saw.  lapd is "distance left to
    # the lap node", so it starts at one lap and falls to zero at the line.
    lapd_max = max((s.get("lapd", 0) for s in with_metrics), default=0)
    totals = [s.get("total", 0) for s in with_metrics]
    first_frame = with_metrics[0].get("f")
    last_frame = with_metrics[-1].get("f")
    first_total = totals[0]
    last_total = totals[-1]
    frames = last_frame - first_frame
    lap_pct = (100.0 * last_total / lapd_max) if lapd_max else 0.0

    # Pursuit error: the signed angle between heading and target.
    perrs = [s["perr"] for s in with_metrics if "perr" in s]
    lress = [s["lres"] for s in with_metrics if "lres" in s]
    # A pursuit error near pi means the target is BEHIND the car.
    behind = sum(1 for p in perrs if abs(p) > 1.0)

    report = {
        "samples": len(with_metrics),
        "first_frame": first_frame,
        "last_frame": last_frame,
        "frames_in_race": frames,
        "lap_units": lapd_max,
        "total_start": first_total,
        "total_end": last_total,
        "lap_percent": round(lap_pct, 4),
        "perr_min": min(perrs) if perrs else None,
        "perr_max": max(perrs) if perrs else None,
        "perr_behind_count": behind,
        "lres_max": max(lress) if lress else None,
        "line_set": sum(1 for s in with_metrics if s.get("line")),
        "drive_set": sum(1 for s in with_metrics if s.get("drive")),
        "stalls": with_metrics[-1].get("stalls"),
        "flips": with_metrics[-1].get("flips"),
        "speed_last": with_metrics[-1].get("speed"),
        "laps_last": with_metrics[-1].get("laps"),
    }

    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    print("race samples        %d (frames %s..%s, %d frames of race)"
          % (len(with_metrics), first_frame, last_frame, frames))
    print("lap length          %d spline units (guest's own lapd max)" % lapd_max)
    print("progress            total %d -> %d   = %.2f%% of a lap"
          % (first_total, last_total, lap_pct))
    print("pursuit error       perr min %.4f max %.4f rad  (%d/%d samples behind the car)"
          % (report["perr_min"], report["perr_max"], behind, len(perrs)))
    print("lateral residual    lres max %.2f" % report["lres_max"])
    print("line established    %d/%d samples" % (report["line_set"], len(with_metrics)))
    print("drive engaged       %d/%d samples" % (report["drive_set"], len(with_metrics)))
    print("stalls / flips      %s / %s" % (report["stalls"], report["flips"]))
    print("speed last          %s" % report["speed_last"])
    print("guest lap counter   %s" % report["laps_last"])

    stall_len, stall_from, stall_to = longest_stall(with_metrics)
    if stall_len > 0:
        print("longest no-progress run  %d samples (frames %s..%s)"
              % (stall_len, with_metrics[stall_from].get("f"),
                 with_metrics[stall_to].get("f")))

    # The curve, sampled: enough points to see the shape without a wall.
    print()
    if args.all:
        print("  frame      total   lap%   speed   perr     lres  steer    mag line flip stall snode  ctdist")
        step = 1
    else:
        print("  frame      total   lap%   speed   perr     lres  line drive")
        step = max(1, len(with_metrics) // 20)
    for index, sample in enumerate(with_metrics):
        if index % step and index != len(with_metrics) - 1:
            continue
        total = sample.get("total", 0)
        pct = (100.0 * total / lapd_max) if lapd_max else 0.0
        if args.all:
            print("  %6s %10d %6.2f %7s %7.4f %6.2f %7.4f %5.2f %3s %3s %5s %5s %8.2f"
                  % (sample.get("f"), total, pct, sample.get("speed"),
                     sample.get("perr", 0.0), sample.get("lres", 0.0),
                     sample.get("steer", 0.0), sample.get("mag", 0.0),
                     sample.get("line"), sample.get("flips"),
                     sample.get("stalls"), sample.get("snode"),
                     sample.get("ctdist", 0.0)))
        else:
            print("  %6s %10d %6.2f %7s %7.4f %6.2f %4s %5s"
                  % (sample.get("f"), total, pct, sample.get("speed"),
                     sample.get("perr", 0.0), sample.get("lres", 0.0),
                     sample.get("line"), sample.get("drive")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
