# Find the control that starts the race, in the guest's OWN layout files.
#
# WHY: the port can press a control by the id the guest gives it (rr3-ui.txt),
# and such a request needs NO frame budget -- the touch script's frame index
# is what broke when the menu phase ran at 3 fps.  So the race button's id is
# worth having.
#
# The guest's screens are XML (.gui) with every widget's id_string and its
# <GuiTransform> box; tools/gui_index.py decrypts and indexes all of them.
# This script finds the control that sits where the race button is.
#
# The screen space is 960x640 (the atlas the guest picks at 1280x720), so the
# button that is at (1114, 698) ON SCREEN is at about (835, 620) in the
# layout.
#
# Usage: python tools\find_gui_button.py [index.json] [--near x,y] [--screen NAME]

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INDEX = os.path.join(HERE, "..", ".build", "guiindex", "gui-index.json")

path = DEFAULT_INDEX
args = sys.argv[1:]
if args and not args[0].startswith("--"):
    path = args.pop(0)

near = None
screen_want = None
if "--near" in args:
    i = args.index("--near")
    near = tuple(float(v) for v in args[i + 1].split(","))
if "--screen" in args:
    i = args.index("--screen")
    screen_want = args[i + 1]

with open(path, "r", encoding="utf-8") as fh:
    index = json.load(fh)

screens = index["screens"]


def walk(node, out):
    if isinstance(node, dict):
        if node.get("id_string"):
            out.append(node)
        for value in node.values():
            walk(value, out)
    elif isinstance(node, list):
        for value in node:
            walk(value, out)


hits = []
for key, screen in screens.items():
    name = screen.get("screen_name") or screen.get("name") or key
    if screen_want and screen_want.lower() not in str(name).lower() \
            and screen_want.lower() not in str(key).lower():
        continue
    controls = []
    walk(screen, controls)
    for c in controls:
        box = c.get("transform") or c.get("box") or c.get("rect") or {}
        try:
            x = float(box.get("x", 0))
            y = float(box.get("y", 0))
            w = float(box.get("width", box.get("w", 0)))
            h = float(box.get("height", box.get("h", 0)))
        except (TypeError, ValueError, AttributeError):
            continue
        cx = x + w / 2.0
        cy = y + h / 2.0
        if near is not None:
            d = (cx - near[0]) ** 2 + (cy - near[1]) ** 2
            hits.append((d, str(name), c.get("id_string"), c.get("tag"), x, y, w, h))
        else:
            if cy > 480 and cx > 640:
                hits.append((0.0, str(name), c.get("id_string"), c.get("tag"), x, y, w, h))

hits.sort(key=lambda h: h[0])
print(f"{len(hits)} candidate control(s) in {path}")
for d, name, cid, tag, x, y, w, h in hits[:40]:
    extra = ""
    if near is not None:
        extra = f" d2={d:10.1f}"
    print(f"   {str(cid):38s} {str(tag):22s} screen={name:34s} rect={x:.0f},{y:.0f},{w:.0f},{h:.0f}{extra}")
