#!/usr/bin/env python3
"""What did the pursuit actually do?

The lap progress is the headline, but the numbers that explain it are the
car's distance from the racing line (rres), the look-ahead distance the
pursuit used (la) and the line it followed (lachunk).

THE REPORT HAS TO SAY WHAT IT MEASURED, NOT ONLY HOW FAR THE CAR GOT.

A round in which the guest's race-metrics object never came alive produces a
race-state line every time, and every one of those lines says "prog=0".  A
report that prints "progress: max=0.000" and stops there is a report that
looks like a measurement of "the pursuit is useless" and is actually a
measurement of "the pursuit never ran".  Those two are very different
findings, and only one of them is about the pursuit.

So this report answers three questions in this order:

  1. DID THE PURSUIT RUN AT ALL?  (the metrics object came alive)
  2. WHICH CONFIGURATION DID IT RUN UNDER?  (the keys the port read)
  3. AND THEN: how far did it get?

Usage: pursuit-report.py <tag>
"""
import re
import sys
from pathlib import Path

RUNS = Path(r"E:\AI\RealRacing3-switch\.build\eden-runs")


def newest(tag: str) -> Path:
    hits = sorted(RUNS.glob(f"*{tag}*"), key=lambda p: p.stat().st_mtime)
    if not hits:
        raise SystemExit(f"no run tagged {tag}")
    return hits[-1]


def main() -> int:
    tag = sys.argv[1] if len(sys.argv) > 1 else "q163-adaptive-centre"
    run = newest(tag)
    print(f"run: {run}\n")

    logs = sorted(run.glob("*guest-view.log"))
    if not logs:
        raise SystemExit("no guest-view.log")
    text = logs[0].read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # The pursuit's own line.
    states = [l for l in lines if "phase=race-state" in l]
    print(f"race-state lines: {len(states)}")
    if not states:
        print("no race-state line: the pursuit never ran")
        return 1

    def field(line: str, name: str):
        m = re.search(rf"\b{name}=(\S+)", line)
        return m.group(1) if m else None

    def frame_of(line: str):
        m = re.search(r"\bf=(\d+)", line)
        return int(m.group(1)) if m else None

    # --- 1. DID THE PURSUIT RUN AT ALL? -----------------------------------
    live = [l for l in states if re.search(r"\bmetrics=1\b", l)]
    rejections = {}
    for l in states:
        r = field(l, "rej")
        if r:
            rejections[r] = rejections.get(r, 0) + 1
    print(f"metrics live   : {len(live)} samples"
          + (f" (first at frame {frame_of(live[0])})" if live else "  <-- NEVER"))
    if not live:
        print(f"rejections     : {rejections}")
        print()
        print("!! THE PURSUIT NEVER RAN.  The guest's race-metrics object was")
        print("   never live, so the port's closed loop never took the wheel and")
        print("   no lap progress is measurable.  This round says NOTHING about")
        print("   the pursuit; it is a failure of the round, not a result.")
        return 2

    # --- 2. WHICH CONFIGURATION DID IT RUN UNDER? --------------------------
    chunks = {}
    modes = {}
    for l in states:
        c = field(l, "lachunk")
        if c:
            chunks[c] = chunks.get(c, 0) + 1
        m = field(l, "lam")
        if m:
            modes[m] = modes.get(m, 0) + 1
    print(f"raceline (lachunk): {chunks}")
    print(f"lookahead (lam)   : {modes}")
    if len(chunks) > 1 or len(modes) > 1:
        print("!! the configuration CHANGED DURING THE ROUND.  The port reads the")
        print("   card's rr3-screen.txt ONCE, at startup, so a round whose")
        print("   configuration moved is measuring two different things.")
    print()

    # --- 3. HOW FAR DID IT GET? --------------------------------------------
    laps = []
    progress = []
    rres = []
    las = []
    for l in states:
        try:
            progress.append(float(field(l, "prog") or 0.0))
        except ValueError:
            pass
        m = re.search(r"\blaps=(\d+)", l)
        if m:
            laps.append(int(m.group(1)))
        m = re.search(r"\brres=(-?[\d.]+)", l)
        if m:
            try:
                rres.append(float(m.group(1)))
            except ValueError:
                pass
        m = re.search(r"\bla=(-?[\d.]+)", l)
        if m:
            try:
                las.append(float(m.group(1)))
            except ValueError:
                pass

    # THE LAP PROGRESS IS `total`, NOT `prog`.
    #
    # The project's own record says "The car's best distance ever | 186,921
    # units = 10.35 % of a lap | q145", and q145's last race-state line
    # reads `total=186921 ... prog=9799`.  So the lap progress is the
    # guest's TOTAL SPLINE DISTANCE, and `prog` is a different quantity --
    # printing it under the name "progress" was an instrument that lied.
    #
    # The number that matters is the MAXIMUM over the round: it is the
    # furthest point the car reached, and it is directly comparable with
    # 186,921 / 10.35 %.
    best_total = 0
    best_total_frame = 0
    for l in states:
        m = re.search(r"\btotal=(\d+)", l)
        if m:
            v = int(m.group(1))
            if v > best_total:
                best_total = v
                f = re.search(r"\bf=(\d+)", l)
                best_total_frame = int(f.group(1)) if f else 0
    reference_units = 186921.0
    reference_percent = 10.35
    if best_total:
        print(f"lap      : best total={best_total} units = "
              f"{best_total / reference_units * reference_percent:.2f} % of a lap "
              f"(at frame {best_total_frame}; the reference is 186921 units = 10.35 %)")
    if progress:
        print(f"prog     : first={progress[0]:.3f} last={progress[-1]:.3f} "
              f"max={max(progress):.3f}   (the guest's own progress member, "
              f"NOT the lap distance)")
    if laps:
        print(f"laps   : {laps[-1]} (max {max(laps)})")
    if rres:
        good = [v for v in rres if v >= 0]
        if good:
            good_sorted = sorted(good)
            med = good_sorted[len(good_sorted) // 2]
            print(f"rres   : median={med:.2f} m  min={min(good):.2f}  "
                  f"max={max(good):.2f}  (n={len(good)})")
    if las:
        las_sorted = sorted(las)
        print(f"la     : min={min(las):.2f}  median={las_sorted[len(las)//2]:.2f}  "
              f"max={max(las):.2f}  (n={len(las)})")

    # The barrier hits and the flips are the other half of the story.
    for key in ("flips=", "stalls=", "sign="):
        vals = [re.search(rf"\b{re.escape(key)}(-?\d+)", l) for l in states]
        vals = [int(v.group(1)) for v in vals if v]
        if vals:
            print(f"{key:8} last={vals[-1]} max={max(vals)}")

    # The guest's own race markers, if the run got that far.
    for marker in ("StartLightsScreen", "InGameScreen", "RaceScreen"):
        n = sum(1 for l in lines if marker in l)
        if n:
            print(f"guest   {marker}: {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
