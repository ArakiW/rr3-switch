# Which of the game's faces measures the splash title at ~748 px?
#
# WHY: the engine placed the title's left edge at x=266 on a 1280 px screen,
# which is where a 748 px wide string would be centred.  The PORT drew the
# string with ConduitITCPro-Bold and measured 2421 px (ground truth 2433.6 px).
# So the engine measured the same string with something ELSE -- and the only
# way to know what is to measure the string in every face the game ships.
#
# MEASURED 2026-11-03: the narrowest face is 2367 px, none is ~748.  So the
# engine did not measure with another face either.

import glob
import os
from fontTools.ttLib import TTFont

TEXT = "Player ID: CCDI123456789 / CCRR123456789"
SIZE = 140.0

ROOT = os.path.dirname(os.path.abspath(__file__))
roots = [
    os.path.join(ROOT, "..", "native-switch", "romfs", "ui"),
    os.path.join(ROOT, "..", "analysis", "rr3-ui-source", "files", ".depot"),
]

seen = {}
for root in roots:
    for path in glob.glob(os.path.join(root, "*")):
        if not os.path.isfile(path):
            continue
        if os.path.splitext(path)[1].lower() not in (".otf", ".ttf"):
            continue
        name = os.path.basename(path)
        if name in seen:
            continue
        try:
            f = TTFont(path, fontNumber=0)
            upem = f["head"].unitsPerEm
            cmap = f.getBestCmap()
            hmtx = f["hmtx"]
            total = 0
            missing = []
            for ch in TEXT:
                gname = cmap.get(ord(ch))
                if gname is None:
                    missing.append(ch)
                    continue
                total += hmtx[gname][0]
            width = total * SIZE / upem
            seen[name] = (width, upem, missing, os.path.getsize(path))
            f.close()
        except Exception as exc:  # noqa: BLE001
            seen[name] = (None, None, [f"ERR {exc}"], os.path.getsize(path))

print(f"{'face':44s} {'width@140':>10s} {'upem':>6s} {'missing':>8s}  bytes")
for name in sorted(seen, key=lambda n: (seen[n][0] is None, seen[n][0] or 0)):
    width, upem, missing, size = seen[name]
    if width is None:
        print(f"{name:44s} {'ERROR':>10s} {'':>6s} {str(missing):>8s}  {size}")
        continue
    flag = ""
    if abs(width - 748.0) < 60.0:
        flag = "   <== ~748 (the engine's implied width)"
    if abs(width - 2421.0) < 60.0:
        flag = "   <== ~2421 (what the port drew)"
    print(f"{name:44s} {width:10.1f} {upem:6d} {len(missing):8d}  {size}{flag}")
