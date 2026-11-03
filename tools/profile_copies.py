# Every AndroidHigh.plist on this machine, decrypted, side by side.
#
# WHY: the guest's own settings dump says it sees EXPECTED_SCREEN_WIDTH 480
# and EXPECTED_SCREEN_HEIGHT 320.  The port builds the profile it serves from
# the game's own copy (settings-profile ... base=rr3core:/files/.depot/eds/
# AndroidHigh.plist ... changed=1), so the file the guest actually reads is
# the one this script has to find.
#
# So: find every copy of the profile, decrypt each, and print the keys that
# decide the UI scale.
#
# Usage: python tools\profile_copies.py

import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "rr3-plist.py")

spec = importlib.util.spec_from_file_location("rr3plist", TOOL)
rr3plist = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rr3plist)

WANTED = ["PLIST_IDENTIFIER", "EXPECTED_SCREEN_WIDTH", "EXPECTED_SCREEN_HEIGHT",
          "GUI_LAYOUT_EXTENSION", "AA_LEVEL", "FONT_PAGE_SIZE",
          "HIGH_RES_MENU", "DEVICE_DESCRIPTION"]

ROOT = os.path.dirname(HERE)
ROOTS = [
    os.path.join(ROOT, "dist"),
    os.path.join(ROOT, ".build"),
    r"E:\Eden-rr3\user\sdmc",
    os.path.join(ROOT, "analysis"),
]

rows = []
for root in ROOTS:
    if not os.path.isdir(root):
        continue
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "node_modules")]
        for name in filenames:
            if not name.startswith("Android") or not name.endswith(".plist"):
                continue
            path = os.path.join(dirpath, name)
            try:
                blob = open(path, "rb").read()
            except OSError:
                continue
            if len(blob) > 200000:
                continue
            try:
                plain = rr3plist.decrypt(blob)
            except Exception:  # noqa: BLE001
                plain = b""
            pairs = {}
            if plain and (b"<plist" in plain[:400] or plain.lstrip().startswith(b"<?xml")):
                pairs = rr3plist.read_key_values(plain)
            rows.append((path, len(blob), pairs))

print(f"{len(rows)} profile copies found\n")
for path, size, pairs in sorted(rows):
    print(f"--- {path}")
    print(f"    {size} bytes" + ("" if pairs else "   (NOT a plain plist after decrypt)"))
    if pairs:
        for k in WANTED:
            if k in pairs:
                kind, value = pairs[k]
                print(f"      {k:28s} = {value!r}  ({kind})")
    print()
