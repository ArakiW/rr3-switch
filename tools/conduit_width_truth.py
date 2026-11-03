# Ground truth for the splash title width.
#
# WHY: the port measured "Player ID: CCDI123456789 / CCRR123456789" at 2421 px
# in ConduitITCPro-Bold at size 140, and the engine put the string's left edge
# at x=266, which is where a 748 px wide string would be centred on a 1280 px
# screen.  Those two numbers disagree by 3.2x.  One of them is wrong.
#
# The FONT FILE is the arbiter: compute the real advance width of that string
# at 140 px and compare.  If the real width is 2421, the port measures
# correctly and the ENGINE is mis-sizing; if it is ~750, the PORT measures
# wrong and that is our bug.
#
# MEASURED 2026-11-03: the real width is 2433.6 px, so the PORT IS RIGHT.

import os
from fontTools.ttLib import TTFont

TEXT = "Player ID: CCDI123456789 / CCRR123456789"

UI = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                  "native-switch", "romfs", "ui")

paths = {
    "Bold": os.path.join(UI, "ConduitITCPro-Bold.otf"),
    "Medium": os.path.join(UI, "ConduitITCPro-Medium.otf"),
    "Regular": os.path.join(UI, "ConduitITCPro-Regular.otf"),
}

for label, path in paths.items():
    f = TTFont(path)
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
    em_px = 140.0
    width = total * em_px / upem
    print(f"{label:8s} upem={upem:5d}  advances={total:6d} units  "
          f"-> {width:8.1f} px at size 140")
    print(f"         per char = {total / len(TEXT) / upem:.4f} em, "
          f"missing={missing}")
    print(f"         size for 748 px  = {748 * upem / total:.1f}")
    print(f"         size for 1280 px = {1280 * upem / total:.1f}")
    print(f"         size for  766 px = {766 * upem / total:.1f}")
    f.close()
