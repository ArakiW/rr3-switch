# Does the pure-pursuit steering converge, or diverge?
#
# WHY: the pursuit's AXIS SIGN has never been measured ("the pursuit's own
# handedness is still UNMEASURED" in race_state_probe.cpp).  The port aims at
# the fitted line's look-ahead point and computes the signed angle from the
# car's heading to that point (perr), then steers by
# steer = steerSign * perr / steerGain.  If the sign is right, the error
# SHRINKS; if it is inverted, the car steers away from the line and the
# error GROWS -- which is the deadlock that keeps the lap count at -1.
#
# So: take the samples of a race-state log line, compute the heading from
# two consecutive car positions, and compare the heading-to-target angle
# before and after each steering command.
#
# The verdict is a SIGN, not a number: "the error grew" or "the error
# shrank".
#
# Usage: python tools\pursuit_convergence.py <guest-view.log> [more logs...]

import math
import re
import sys

SAMPLE = re.compile(r"cpos=(-?\d+),(-?\d+),(-?\d+)\s+ctgt=(-?\d+),(-?\d+),(-?\d+)")
PERR = re.compile(r"perr=(-?[0-9.]+)")
PUR = re.compile(r"pur=(\d)")
STEER = re.compile(r"steer=(-?[0-9.]+)")
FRAME = re.compile(r"\bf=(\d+)")


def parse(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "cpos=" not in line or "perr=" not in line:
                continue
            m = SAMPLE.search(line)
            p = PERR.search(line)
            u = PUR.search(line)
            s = STEER.search(line)
            f = FRAME.search(line)
            if not (m and p and u and s):
                continue
            rows.append({
                "frame": int(f.group(1)) if f else -1,
                "px": int(m.group(1)) / 256.0,
                "py": int(m.group(2)) / 256.0,
                "tx": int(m.group(4)) / 256.0,
                "ty": int(m.group(5)) / 256.0,
                "perr": float(p.group(1)),
                "pur": int(u.group(1)),
                "steer": float(s.group(1)),
            })
    return rows


def analyse(path):
    rows = parse(path)
    if len(rows) < 4:
        print(f"{path}: only {len(rows)} samples")
        return
    # The angle between the car's heading (from two positions) and the
    # direction to the target.
    def heading_error(a, b):
        hx = b["px"] - a["px"]
        hy = b["py"] - a["py"]
        dx = b["tx"] - b["px"]
        dy = b["ty"] - b["py"]
        hlen = math.hypot(hx, hy)
        dlen = math.hypot(dx, dy)
        if hlen < 0.05 or dlen < 0.05:
            return None
        cross = (hx / hlen) * (dy / dlen) - (hy / hlen) * (dx / dlen)
        dot = (hx / hlen) * (dx / dlen) + (hy / hlen) * (dy / dlen)
        return math.atan2(cross, dot)

    grew = 0
    shrank = 0
    flat = 0
    checked = 0
    pairs = []
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        if a["pur"] != 1 or b["pur"] != 1:
            continue
        if abs(a["steer"]) < 0.05:
            continue
        e0 = heading_error(rows[max(0, i - 1)], a)
        e1 = heading_error(a, b)
        if e0 is None or e1 is None:
            continue
        checked += 1
        # Compare the MAGNITUDE: a correct sign reduces the angle.
        if abs(e1) < abs(e0) - 0.01:
            shrank += 1
            pairs.append((a["frame"], a["steer"], e0, e1, "shrink"))
        elif abs(e1) > abs(e0) + 0.01:
            grew += 1
            pairs.append((a["frame"], a["steer"], e0, e1, "GROW"))
        else:
            flat += 1
            pairs.append((a["frame"], a["steer"], e0, e1, "flat"))

    purs = sum(1 for r in rows if r["pur"] == 1)
    print(f"{path}")
    print(f"   samples {len(rows)}   pursuit ON {purs}   checked pairs {checked}")
    print(f"   error SHRANK {shrank}   GREW {grew}   flat {flat}")

    # THE DECISIVE STATISTIC: does the error's change depend on the SIGN of
    # the steering?  If a negative steer makes the error grow and a positive
    # one makes it shrink, then flipping the sign makes the controller
    # converge, and that is a MEASUREMENT rather than a guess.
    by_sign = {}
    for frame, steer, e0, e1, verdict in pairs:
        key = "+" if steer > 0.05 else ("-" if steer < -0.05 else "0")
        by_sign.setdefault(key, []).append(e1 - e0)
    for key in ("+", "-", "0"):
        if key not in by_sign:
            continue
        deltas = by_sign[key]
        mean = sum(deltas) / len(deltas)
        print(f"   steer {key}: n={len(deltas):3d}  mean d|error| = {mean:+.4f} rad "
              f"({'the error GREW' if mean > 0.002 else ('the error shrank' if mean < -0.002 else 'no change')})")
    for p in pairs[:12]:
        print(f"      f={p[0]:6d} steer={p[1]:+.3f}  err {p[2]:+.3f} -> {p[3]:+.3f}  {p[4]}")
    print()


for path in sys.argv[1:]:
    analyse(path)
