#!/usr/bin/env python3
"""The lap progress, in the units the project has always used.

THE FIELD.  The project's own record says "The car's best distance ever |
186,921 units = 10.35 % of a lap | q145", and q145's last race-state line
reads `total=186921 ... prog=9799`.  So the LAP PROGRESS IS `total` (the
guest's total spline distance), and `prog` is a different quantity that the
old report was printing under the name "progress".

⇒ The number this script prints is the MAXIMUM `total` over the round,
which is the furthest point the car reached, and it is directly comparable
with 186,921 / 10.35 %.

Usage: python .build/lap-progress.py <tag> [<tag> ...]
"""
import re
import sys
from pathlib import Path

RUNS = Path(r"E:\AI\RealRacing3-switch\.build\eden-runs")

# The reference the project measures against: q145, 186,921 units = 10.35 %.
REFERENCE_UNITS = 186921.0
REFERENCE_PERCENT = 10.35


def find(tag):
    hits = sorted(RUNS.glob(f"*{tag}*"), key=lambda p: p.stat().st_mtime)
    return hits[-1] if hits else None


def main():
    tags = sys.argv[1:] or ["pursuit-fixed-centre", "pursuit-adaptive-centre",
                            "pursuit-fixed-ai", "pursuit-adaptive-ai"]
    print("{0:26} {1:>9} {2:>8} {3:>7} {4:>7} {5:>7} {6:>7}".format(
        "round", "best", "%", "live@", "rres", "flips", "stalls"))
    print("{0:26} {1:>9} {2:>8} {3:>7} {4:>7} {5:>7} {6:>7}".format(
        "q145 (the reference)", "186921", "10.35", "540", "0.32", "7", "10"))
    for tag in tags:
        run = find(tag)
        if run is None:
            print("{0:26}  NOT FOUND".format(tag))
            continue
        log = run / "rr3-native-rehost.guest-view.log"
        best = -1
        best_f = 0
        live_at = None
        rres = []
        flips = 0
        stalls = 0
        with log.open("r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "phase=race-state" not in line:
                    continue
                m = re.search(r"\btotal=(\d+)", line)
                if m:
                    # `total` is SIGNED.  The guest publishes the car's
                    # position along the track and it goes NEGATIVE when the
                    # car drives backwards, so the raw uint32 wraps.  Taking
                    # the max of the UNSIGNED value makes a wrapped negative
                    # look like the best result of the round, which is the
                    # instrument lying for the third time.
                    v = int(m.group(1))
                    if v >= 0x80000000:
                        v -= 0x100000000
                    if v > best:
                        best = v
                        f = re.search(r"\bf=(\d+)", line)
                        best_f = int(f.group(1)) if f else 0
                if live_at is None and re.search(r"\bmetrics=1\b", line):
                    f = re.search(r"\bf=(\d+)", line)
                    live_at = int(f.group(1)) if f else 0
                m = re.search(r"\brres=(-?[\d.]+)", line)
                if m:
                    try:
                        rres.append(float(m.group(1)))
                    except ValueError:
                        pass
                m = re.search(r"\bflips=(\d+)", line)
                if m:
                    flips = max(flips, int(m.group(1)))
                m = re.search(r"\bstalls=(\d+)", line)
                if m:
                    stalls = max(stalls, int(m.group(1)))
        pct = (best / REFERENCE_UNITS) * REFERENCE_PERCENT if best > 0 else 0.0
        med = 0.0
        if rres:
            good = sorted(v for v in rres if v >= 0)
            if good:
                med = good[len(good) // 2]
        print("{0:26} {1:>9} {2:>8} {3:>7} {4:>7} {5:>7} {6:>7}".format(
            tag[-26:], best, "{0:.2f}".format(pct),
            live_at if live_at else "-", "{0:.2f}".format(med), flips, stalls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
