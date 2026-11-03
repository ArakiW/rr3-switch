# The full control list of one screen, from the live UI probe.
#
# WHY: the ui-ctl line is long (1193 controls) and printing it truncates,
# so the button that starts the race was missed by a truncated read.
#
# The probe prints every control with its rectangle: "ID(x,y,w,h)".  This
# prints them all, and the ones in a given corner first.
#
# Usage: python tools\screen_controls.py <guest-view.log> [SCREEN] [--corner xmin,ymin]

import re
import sys

path = sys.argv[1]
screen_want = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else None
xmin = 0
ymin = 0
if "--corner" in sys.argv:
    i = sys.argv.index("--corner")
    xmin, ymin = [int(v) for v in sys.argv[i + 1].split(",")]

ID = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")
SC = re.compile(r"\bscreen=(\S+)")
FR = re.compile(r"\bf=(\d+)")

best = None
with open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if "phase=ui-ctl" not in line:
            continue
        m = SC.search(line)
        screen = m.group(1) if m else "?"
        if screen_want and screen != screen_want:
            continue
        f = FR.search(line)
        frame = int(f.group(1)) if f else -1
        controls = [(cid, int(x), int(y), int(w), int(h))
                    for cid, x, y, w, h in ID.findall(line)]
        if not controls:
            continue
        if best is None or len(controls) > len(best[2]):
            best = (frame, screen, controls)

if best is None:
    print("no ui-ctl line for that screen")
    sys.exit(1)

frame, screen, controls = best
print(f"{screen} at frame {frame}: {len(controls)} controls parsed from the line")
print()
corner = [c for c in controls
          if c[1] + c[3] > xmin and c[2] + c[4] > ymin]
if xmin or ymin:
    print(f"controls reaching past ({xmin},{ymin}): {len(corner)}")
    for cid, x, y, w, h in sorted(corner, key=lambda c: -(c[1] + c[3])):
        print(f"   {cid:38s} rect={x},{y},{w},{h}  centre={x + w // 2},{y + h // 2}")
print()
print("every control whose id looks like an action button:")
for cid, x, y, w, h in controls:
    up = cid.upper()
    if any(k in up for k in ("BTN", "BUTTON", "GO", "START", "RACE", "PLAY", "OK", "NEXT")):
        print(f"   {cid:38s} rect={x},{y},{w},{h}  centre={x + w // 2},{y + h // 2}")
