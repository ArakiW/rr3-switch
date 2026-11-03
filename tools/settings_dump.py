# Dump the guest's own settings, key AND value, from a guest-view.log.
#
# WHY: the guest prints "Current settings:" followed by one line per key
# ("NAME" - ) and one line with the value.  The values are the numbers the
# engine actually ran with, so they are the ground truth for every question
# about font size, UI scale and screen size.
#
# MEASURED 2026-11-03: the AndroidHigh profile carries
#   EXPECTED_SCREEN_WIDTH  = 480   EXPECTED_SCREEN_HEIGHT = 320
#   GUI_LAYOUT_EXTENSION   = _480x320
# while the screen is 1280x720 -- and q61 already measured that changing
# EXPECTED_SCREEN_* does NOT move the font sizes.
#
# Usage: python tools\settings_dump.py <guest-view.log>

import re
import sys

path = sys.argv[1]
KEY = re.compile(r'"([A-Z0-9_]+)" - $')

WANTED = [
    "FONT_PAGE_SIZE", "ENGINE_FONT_CACHE_SIZE",
    "GUI_ATLAS_OVERRIDE", "GUI_IMAGE_AUTO_SCALE", "GUI_LAYOUT_EXTENSION",
    "HIGH_RES_MENU", "EXPECTED_SCREEN_WIDTH", "EXPECTED_SCREEN_HEIGHT",
    "SAFE_AREA_LEFT", "SAFE_AREA_RIGHT", "SAFE_AREA_TOP", "SAFE_AREA_BOTTOM",
    "3D_SCENE_SCALE", "DEVICE_DESCRIPTION", "PLIST_IDENTIFIER",
    "EXTERNAL_DISPLAY_DEFAULT_RES", "ALLOW_RETINA", "AA_LEVEL",
    "CREATE_FULL_SCREEN_BUFFERS", "USE_FULL_SCREEN_EFFECTS",
]

with open(path, "r", encoding="utf-8", errors="replace") as fh:
    lines = fh.read().splitlines()

start = None
for i, line in enumerate(lines):
    if "Current settings:" in line:
        start = i
        break
if start is None:
    print("no settings dump in this log")
    sys.exit(1)

pairs = []
key = None
for line in lines[start + 1:start + 1400]:
    m = KEY.search(line)
    if m:
        key = m.group(1)
        continue
    if key is not None:
        if "guest-log-any" in line:
            value = line.split("guest-log-any", 1)[1]
            value = re.sub(r"^\s*ordinal=\d+\s+target=[0-9a-f]+\s*", "", value)
            pairs.append((key, value.strip()))
            key = None
        elif "Res order" in line or "Current res" in line:
            break

print(f"settings dump: {len(pairs)} entries from line {start + 1}")
print()
print("--- the ones that matter for text size and UI layout ---")
for k, v in pairs:
    if k in WANTED:
        print(f"  {k:32s} = {v}")

print()
print("--- every entry whose value is a number, for reference ---")
for k, v in pairs:
    if re.match(r"^-?\d+(\.\d+)?$", v):
        print(f"  {k:32s} = {v}")
