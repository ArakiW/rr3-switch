# List the controls that sit in a corner of the screen, across every screen.
#
# WHY: the "race" button on the "select car" screen is NOT in that screen's
# own control tree (measured: CarSelectScreen has 44 controls and none of
# them is in the bottom-right corner).  It belongs to another screen, so the
# id has to be looked for across the whole live tree.
#
# Usage: python tools\corner_controls.py <guest-view.log> <xmin> <ymin>

import re
import sys

path = sys.argv[1]
xmin = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
ymin = int(sys.argv[3]) if len(sys.argv) > 3 else 600

ID = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\((-?\d+),(-?\d+),(-?\d+),(-?\d+)\)")
SC = re.compile(r"\bscreen=(\S+)")

best = {}
with open(path, "r", encoding="utf-8", errors="replace") as fh:
    for line in fh:
        if "phase=ui-ctl" not in line:
            continue
        m = SC.search(line)
        screen = m.group(1) if m else "?"
        for cid, xs, ys, ws, hs in ID.findall(line):
            x, y, w, h = int(xs), int(ys), int(ws), int(hs)
            if w <= 0 or h <= 0:
                continue
            cx = x + w // 2
            cy = y + h // 2
            if cx >= xmin and cy >= ymin:
                best[(screen, cid)] = (x, y, w, h, cx, cy)

print(f"controls with centre >= ({xmin},{ymin}): {len(best)}")
for (screen, cid), (x, y, w, h, cx, cy) in sorted(best.items(), key=lambda kv: -kv[1][4])[:30]:
    print(f"   {screen:28s} {cid:34s} rect={x},{y},{w},{h}  centre={cx},{cy}")
