# Does the pure-pursuit steering converge, or diverge?  (wrap-aware)
#
# WHY: the pursuit's AXIS SIGN has never been measured ("the pursuit's own
# handedness is still UNMEASURED" in race_state_probe.cpp).  The controller
# aims at the fitted line's look-ahead point, computes the signed angle from
# the car's heading to it (perr), and steers by
#
#     steer = steerSign * perr / steerGain
#
# The car's heading turns the error down by some rate L, so
#
#     d(error) ~ -L * steer          (L > 0 when the sign is RIGHT)
#
# and the SIGN of the fitted slope is the answer: a POSITIVE slope means the
# steering makes the error worse and the sign has to be flipped.  The fit's
# R^2 says whether the model holds at all.
#
# The angle WRAPS at +-pi, so the change is unwrapped: a raw difference of
# -2.92 -> +3.09 is +0.02, not -6.01.  A first version of this analysis
# compared the magnitudes and got the answer backwards for exactly that
# reason.
#
# Usage: python tools\pursuit_slope.py <guest-view.log> [more logs...]

import math
import re
import sys

SAMPLE = re.compile(r"cpos=(-?\d+),(-?\d+),(-?\d+)\s+ctgt=(-?\d+),(-?\d+),(-?\d+)")
PERR = re.compile(r"perr=(-?[0-9.]+)")
PUR = re.compile(r"pur=(\d)")
STEER = re.compile(r"steer=(-?[0-9.]+)")
FRAME = re.compile(r"\bf=(\d+)")
RATE = re.compile(r"rate=(-?[0-9.]+)")


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
            r = RATE.search(line)
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
                "rate": float(r.group(1)) if r else 0.0,
            })
    return rows


def wrap(a):
    return math.atan2(math.sin(a), math.cos(a))


def heading_error(a, b):
    """The signed angle from the heading (a -> b) to the direction b -> target."""
    hx = b["px"] - a["px"]
    hy = b["py"] - a["py"]
    dx = b["tx"] - b["px"]
    dy = b["ty"] - b["py"]
    hlen = math.hypot(hx, hy)
    dlen = math.hypot(dx, dy)
    if hlen < 0.05 or dlen < 0.05:
        return None, hlen
    cross = (hx / hlen) * (dy / dlen) - (hy / hlen) * (dx / dlen)
    dot = (hx / hlen) * (dx / dlen) + (hy / hlen) * (dy / dlen)
    return math.atan2(cross, dot), hlen


def analyse(path):
    rows = parse(path)
    print(f"{path}")
    if len(rows) < 6:
        print(f"   only {len(rows)} samples")
        return
    xs = []
    ys = []
    for i in range(len(rows) - 1):
        a, b = rows[i], rows[i + 1]
        if a["pur"] != 1 or b["pur"] != 1:
            continue
        if abs(a["steer"]) < 0.05:
            continue
        e0, hlen = heading_error(rows[max(0, i - 1)], a)
        e1, _ = heading_error(a, b)
        if e0 is None or e1 is None:
            continue
        # The car has to be MOVING for its heading to mean anything, and the
        # error must not be sitting on the +-pi wrap.
        if hlen < 0.05:
            continue
        if abs(abs(e0) - math.pi) < 0.2:
            continue
        de = wrap(e1 - e0)
        xs.append(a["steer"])
        ys.append(de)

    n = len(xs)
    if n < 4:
        print(f"   pursuit ON {sum(1 for r in rows if r['pur'] == 1)}"
              f"   usable pairs {n}   (too few)")
        print()
        return
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    syy = sum((y - my) ** 2 for y in ys)
    slope = sxy / sxx if sxx > 0 else 0.0
    r2 = (sxy * sxy) / (sxx * syy) if sxx > 0 and syy > 0 else 0.0
    print(f"   pursuit ON {sum(1 for r in rows if r['pur'] == 1)} of {len(rows)}"
          f"   usable pairs {n}")
    print(f"   d(error) = {slope:+.4f} * steer   (R^2 = {r2:.3f})")
    if slope < -0.002:
        print("   => THE SIGN IS RIGHT: steering reduces the error."
              "  d(error)/d(steer) is negative, as a working controller must be.")
    elif slope > 0.002:
        print("   => THE SIGN IS WRONG: steering INCREASES the error."
              "  Flip steersign.")
    else:
        print("   => no usable slope; the steering does not move the error.")
    print()


for path in sys.argv[1:]:
    analyse(path)
