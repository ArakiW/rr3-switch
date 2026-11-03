# Has ANY round ever completed a lap?
#
# WHY: "laps = -1" is the port's headline symptom, and the goal is to get
# the game properly running.  The race-state line carries the guest's OWN lap
# counter (laps=), the lap distance (lapd=), the total distance (total=),
# the best progress rate (best=) and the steering state.  So the question
# "did any round ever get round the track" is answerable from the archives
# without a new round -- and it says which of the archived rounds is the one
# worth studying.
#
# Usage: python tools\lap_census.py [eden-runs-dir]

import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else r"E:\AI\RealRacing3-switch\.build\eden-runs"

LAPS = re.compile(r"\blaps=(\d+)")
LAPD = re.compile(r"\blapd=(\d+)")
TOTAL = re.compile(r"\btotal=(\d+)")
BEST = re.compile(r"\bbest=(-?[0-9.]+)")
RATE = re.compile(r"\brate=(-?[0-9.]+)")
PUR = re.compile(r"\bpur=(\d)")
DRIVE = re.compile(r"\bdrive=(\d)")
STEER = re.compile(r"\bsteer=(-?[0-9.]+)")
FLIPS = re.compile(r"\bflips=(\d+)")
STALLS = re.compile(r"\bstalls=(\d+)")
BRK = re.compile(r"\bbrk=(\d)")
OK = re.compile(r"\bok=(\d)")

rows = []
for name in sorted(os.listdir(ROOT)):
    log = os.path.join(ROOT, name, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        continue
    best_laps = -1
    best_lapd = 0
    best_total = 0
    best_rate = 0.0
    samples = 0
    drive_samples = 0
    pur_on = 0
    flips = 0
    stalls = 0
    brk = 0
    ok = 0
    with open(log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "laps=" not in line:
                continue
            samples += 1
            m = LAPS.search(line)
            if m:
                best_laps = max(best_laps, int(m.group(1)))
            m = LAPD.search(line)
            if m:
                best_lapd = max(best_lapd, int(m.group(1)))
            m = TOTAL.search(line)
            if m:
                best_total = max(best_total, int(m.group(1)))
            m = BEST.search(line)
            if m:
                best_rate = max(best_rate, float(m.group(1)))
            m = PUR.search(line)
            if m and m.group(1) == "1":
                pur_on += 1
            m = DRIVE.search(line)
            if m and m.group(1) == "1":
                drive_samples += 1
            m = OK.search(line)
            if m:
                ok += max(ok, int(m.group(1)))
            for rx, key in ((FLIPS, "flips"), (STALLS, "stalls"), (BRK, "brk")):
                m = rx.search(line)
                if m:
                    if key == "flips":
                        flips = max(flips, int(m.group(1)))
                    elif key == "stalls":
                        stalls = max(stalls, int(m.group(1)))
                    else:
                        brk = max(brk, int(m.group(1)))
    if samples:
        rows.append((best_laps, best_total, best_lapd, samples, drive_samples, pur_on,
                     flips, stalls, brk, name))

rows.sort(key=lambda r: (-r[0], -r[1]))
print(f"{'laps':>5s} {'total':>9s} {'lapd':>8s} {'smp':>5s} {'drv':>5s} "
      f"{'pur':>5s} {'flip':>5s} {'stall':>5s} {'brk':>4s}  round")
for r in rows[:22]:
    print(f"{r[0]:5d} {r[1]:9d} {r[2]:8d} {r[3]:5d} {r[4]:5d} {r[5]:5d} "
          f"{r[6]:5d} {r[7]:5d} {r[8]:4d}  {r[9]}")

completed = [r for r in rows if r[0] > 0]
print()
print(f"rounds with a race-state line : {len(rows)}")
print(f"rounds whose guest reported laps > 0 : {len(completed)}")
if completed:
    best = max(completed, key=lambda r: r[0])
    print(f"best : {best[9]}  laps={best[0]} total={best[1]} lapd={best[2]}")
else:
    print("NO round has ever reported a completed lap.")
    far = max(rows, key=lambda r: r[1]) if rows else None
    if far:
        print(f"furthest : {far[9]}  total={far[1]} spline units "
              f"({far[1] / 7056.0 * 100.0:.1f}% of a 7056-unit lap)")
