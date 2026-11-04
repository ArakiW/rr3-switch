#!/usr/bin/env python3
"""Where does a frame's time go?

WHY
---
The frame rate is a PHASE measurement (MEASURED 2026-11-04: the menu phase
runs at 4.8 fps and the race phase at 15..17 fps on Eden) while the CPU is
flat at 1.30..1.56 cores in every healthy round.  A flat CPU with a 3x
frame-rate spread means the extra time per frame is WAITING, and until now
nothing in the log said what it waits on.

The port's per-frame census line carries the answer (added 2026-11-04):

    tt   the whole frame iteration, in ms
    tr   the guest's own render call
    tp   the port's own per-frame work and the input/Cloudcell pumps
    ts   eglSwapBuffers -- the PRESENT, the host's display path

This script reads those and splits them BY PHASE, because a mean over the
whole run mixes the two and says nothing (that is the mistake the October
"23 fps" and the November "3.9 fps" were).

THE PHASE BOUNDARY
------------------
The race starts when the guest's own RaceMetrics object appears, which the
race-state log records with its frame number.  Everything before that frame
is the menu; everything from it on is the race.

USAGE
-----
    python tools/frame_costs.py <guest-view.log>
"""

import argparse
import re
import sys

CENSUS_RE = re.compile(r"phase=guest-frame-census\b")
FRAME_RE = re.compile(r"\bframe=(\d+)")
# The race-state line numbers its sample with "f=", NOT "frame=" -- the census
# line is the one that says "frame=".  Using the wrong one made the phase
# split silently fall back to "the whole log is the menu", which is how the
# first run of this script reported a race it had plainly seen.
RACE_FRAME_RE = re.compile(r"\bf=(\d+)\b")
TIMING_RE = re.compile(r"\btt=(\d+)\s+tr=(\d+)\s+ts=(\d+)\s+tp=(\d+)")
RACE_RE = re.compile(r"phase=race-state\b")
METRICS_RE = re.compile(r"metrics=1")


def percentiles(values, points=(50, 90, 99)):
    if not values:
        return []
    ordered = sorted(values)
    out = []
    for point in points:
        index = min(len(ordered) - 1, int(len(ordered) * point / 100.0))
        out.append((point, ordered[index]))
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log")
    args = parser.parse_args()

    samples = []
    race_first_frame = None
    with open(args.log, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if RACE_RE.search(line) and METRICS_RE.search(line):
                if race_first_frame is None:
                    found = RACE_FRAME_RE.search(line)
                    if found:
                        race_first_frame = int(found.group(1))
                continue
            if not CENSUS_RE.search(line):
                continue
            timing = TIMING_RE.search(line)
            if timing is None:
                continue
            frame_found = FRAME_RE.search(line)
            samples.append({
                "frame": int(frame_found.group(1)) if frame_found else 0,
                "tt": int(timing.group(1)),
                "tr": int(timing.group(2)),
                "ts": int(timing.group(3)),
                "tp": int(timing.group(4)),
            })

    if not samples:
        print("no timed census lines in this log")
        print("(the tt/tr/ts/tp fields were added on 2026-11-04)")
        return 1

    if race_first_frame is None:
        print("no race in this run: the whole log is the menu phase")
        phases = [("menu", samples)]
    else:
        menu = [s for s in samples if s["frame"] < race_first_frame]
        race = [s for s in samples if s["frame"] >= race_first_frame]
        phases = []
        if menu:
            phases.append(("menu", menu))
        if race:
            phases.append(("race", race))

    print("race starts at frame %s" % (race_first_frame if race_first_frame else "-"))
    for name, group in phases:
        count = len(group)
        print("")
        print("=== %s phase: %d timed frames ===" % (name, count))
        for key, label in (("tt", "the whole frame"),
                           ("tr", "the guest's render"),
                           ("tp", "the port's own work"),
                           ("ts", "the present (swap)")):
            values = [s[key] for s in group]
            mean = sum(values) / float(count)
            print("  %-22s mean %7.1f ms   median %7.1f   p90 %7.1f   max %7.1f"
                  % (label, mean,
                     percentiles(values, (50,))[0][1],
                     percentiles(values, (90,))[0][1],
                     max(values)))
            if key == "tt" and mean > 0.0:
                print("      of which: guest %.1f%%, port %.1f%%, present %.1f%%"
                      % (100.0 * sum(s["tr"] for s in group) / sum(s["tt"] for s in group),
                         100.0 * sum(s["tp"] for s in group) / sum(s["tt"] for s in group),
                         100.0 * sum(s["ts"] for s in group) / sum(s["tt"] for s in group)))
        fps = 1000.0 / (sum(s["tt"] for s in group) / float(count))
        print("  %-22s %.2f fps" % ("implied frame rate", fps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
