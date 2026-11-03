# Measure where the splash title actually lands on the screen.
#
# WHY: the log says the guest asked for size=140 and the port measured the
# string at 2421 px wide on a 1280 px screen.  "Too wide" is not a diagnosis:
# the LEFT EDGE tells us whether the engine CENTRED the full string (which
# would start off-screen) or PLACED it at a margin (which means the engine
# laid out for a DIFFERENT width than the one it drew into).
#
# So: find the white ink of the title band and report its extent.
#
# Usage: python tools\splash_measure.py [png]

import os
import sys
import numpy as np
from PIL import Image

DEFAULT = os.path.join(
    r"E:\AI\RealRacing3-switch\.build\eden-runs",
    "20261103-202049-eden-rr3-q132-worker-gate", "frames", "frame-000120.png")

path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT

img = Image.open(path).convert("RGB")
a = np.asarray(img).astype(np.int16)
h, w, _ = a.shape
print(f"image   : {path}")
print(f"size    : {w}x{h}")

# White-ish ink on a dark blue background.
bright = (a[:, :, 0] > 170) & (a[:, :, 1] > 170) & (a[:, :, 2] > 170)
rows = np.where(bright.any(axis=1))[0]
if rows.size == 0:
    print("no bright pixels")
    sys.exit(0)

# Group the bright rows into bands (gaps of >= 4 empty rows start a new band).
bands = []
start = rows[0]
prev = rows[0]
for r in rows[1:]:
    if r - prev > 4:
        bands.append((start, prev))
        start = r
    prev = r
bands.append((start, prev))

print(f"bands   : {len(bands)}")
for (y0, y1) in bands:
    band = bright[y0:y1 + 1, :]
    cols = np.where(band.any(axis=0))[0]
    x0, x1 = int(cols[0]), int(cols[-1])
    ink = int(band.sum())
    print(f"  y {y0:4d}..{y1:4d} (h={y1 - y0 + 1:4d})  x {x0:5d}..{x1:5d} "
          f"(w={x1 - x0 + 1:5d})  ink={ink}")
    if x1 >= w - 1:
        print(f"    -> TOUCHES THE RIGHT EDGE (column {x1} of {w - 1}): clipped")
    if x0 == 0:
        print(f"    -> TOUCHES THE LEFT EDGE: clipped")
    colhits = band.sum(axis=0)
    gaps = []
    run = None
    for x in range(x0, x1 + 1):
        if colhits[x] == 0:
            if run is None:
                run = x
        else:
            if run is not None:
                gaps.append((run, x - 1))
                run = None
    if run is not None:
        gaps.append((run, x1))
    big = [g for g in gaps if (g[1] - g[0]) >= 4]
    if big:
        print(f"    -> {len(big)} gaps >= 4px, widths "
              f"{[g[1] - g[0] + 1 for g in big][:20]}")
