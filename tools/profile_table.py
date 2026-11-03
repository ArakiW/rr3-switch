# Offline table of the guest's device profiles.
#
# WHY: the guest's own settings dump (from a guest-view.log) says the
# AndroidHigh profile carries
#
#     EXPECTED_SCREEN_WIDTH  = 480
#     EXPECTED_SCREEN_HEIGHT = 320
#     GUI_LAYOUT_EXTENSION   = "_480x320"
#
# while the port runs a 1280x720 screen.  The cipher is the guest's own RC4
# variant and the key is compiled into the guest binary, so all six profiles
# can be read offline -- no emulator round needed to find out what the OTHER
# tiers carry.
#
# Usage: python tools\profile_table.py [dir-with-plists]

import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "rr3-plist.py")

spec = importlib.util.spec_from_file_location("rr3plist", TOOL)
rr3plist = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rr3plist)

WANTED = [
    "PLIST_IDENTIFIER", "DEVICE_DESCRIPTION",
    "EXPECTED_SCREEN_WIDTH", "EXPECTED_SCREEN_HEIGHT",
    "GUI_LAYOUT_EXTENSION", "GUI_ATLAS_OVERRIDE", "GUI_IMAGE_AUTO_SCALE",
    "HIGH_RES_MENU", "AA_LEVEL", "FONT_PAGE_SIZE", "ENGINE_FONT_CACHE_SIZE",
    "3D_SCENE_SCALE", "ALLOW_RETINA",
    "SAFE_AREA_LEFT", "SAFE_AREA_RIGHT", "SAFE_AREA_TOP", "SAFE_AREA_BOTTOM",
    "EXTERNAL_DISPLAY_DEFAULT_RES", "CREATE_FULL_SCREEN_BUFFERS",
]

ROOT = os.path.dirname(HERE)
dirs = sys.argv[1:] or [
    os.path.join(ROOT, ".build", "eds"),
    os.path.join(ROOT, ".build", "f1n-extract"),
]

profiles = {}
for d in dirs:
    if not os.path.isdir(d):
        continue
    for name in sorted(os.listdir(d)):
        if not name.endswith(".plist"):
            continue
        path = os.path.join(d, name)
        blob = open(path, "rb").read()
        try:
            plain = rr3plist.decrypt(blob)
        except Exception as exc:  # noqa: BLE001
            print(f"{name:28s} DECRYPT FAILED: {exc}")
            continue
        if not plain.lstrip().startswith(b"<?xml") and b"<plist" not in plain[:200]:
            print(f"{name:28s} not a plist after decrypt (head={plain[:24]!r})")
            continue
        pairs = rr3plist.read_key_values(plain)
        profiles[name] = (pairs, len(blob))

if not profiles:
    print("no profiles found")
    sys.exit(1)

print(f"{'profile':28s} {'bytes':>7s}  keys")
for name in sorted(profiles):
    pairs, size = profiles[name]
    print(f"\n===== {name}  ({size} bytes, {len(pairs)} keys) =====")
    table = {k: (v[1] if isinstance(v, tuple) else v) for k, v in pairs.items()}
    for key in WANTED:
        if key in table:
            print(f"   {key:32s} = {table[key]}")
    extra = sorted(k for k in table if k not in WANTED)
    if extra:
        print(f"   ... {len(extra)} other keys")
