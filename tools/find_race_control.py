# Find the control the port can press to START a race.
#
# WHY: the touch script is FRAME indexed, and the game's "race" tap sits at
# script frame 5500 while the game only reached port frame 2400 in 611 s
# (about 4 fps).  So the tap that starts the race never fires.
#
# The port can press a control BY ITS ID (rr3-ui.txt), and a request without
# "at=<frame>" fires as soon as the control is on screen.  So the race can be
# started without a frame budget -- once the control's id is known.
#
# The UI probe logs the live screen tree with every control's id and
# rectangle (phase=ui-ctl ... screen=<name> ... ID(x,y,w,h) ...), so the id
# of the button at the bottom right of the "select car" screen is a lookup,
# not a guess.
#
# Usage: python tools\find_race_control.py <guest-view.log> [--at x,y]

import re
import sys

path = sys.argv[1]
want_x = None
want_y = None
if "--at" in sys.argv:
    i = sys.argv.index("--at")
    want_x, want_y = [float(v) for v in sys.argv[i + 1].split(",")]

SCREEN = re.compile(r"\bscreen=(\S+)")
ID_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")

best = []
with open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if "phase=ui-ctl" not in line:
            continue
        m = SCREEN.search(line)
        screen = m.group(1) if m else "?"
        for cid, x, y, w, h in ID_RE.findall(line):
            x = int(x)
            y = int(y)
            w = int(w)
            h = int(h)
            if w <= 0 or h <= 0:
                continue
            if want_x is None:
                best.append((screen, cid, x, y, w, h))
            else:
                # The centre of the control, nearest to the wanted point.
                cx = x + w / 2.0
                cy = y + h / 2.0
                d = (cx - want_x) ** 2 + (cy - want_y) ** 2
                best.append((d, screen, cid, x, y, w, h))

if want_x is None:
    print(f"{len(best)} control entries")
    seen = {}
    for screen, cid, x, y, w, h in best:
        seen.setdefault(cid, set()).add(screen)
    print(f"{len(seen)} distinct control ids")
    print()
    print("ids that look like a race/action button:")
    for cid in sorted(seen):
        if any(k in cid.upper() for k in ("RACE", "GO", "START", "BEGIN", "DRIVE", "PLAY")):
            print(f"   {cid:34s} {sorted(seen[cid])[:3]}")
else:
    best.sort(key=lambda r: r[0])
    print(f"controls nearest ({want_x},{want_y}):")
    for entry in best[:12]:
        d, screen, cid, x, y, w, h = entry
        print(f"   d={d:9.1f}  {cid:30s} screen={screen:28s} rect={x},{y},{w},{h}"
              f"  centre={x + w / 2:.0f},{y + h / 2:.0f}")

