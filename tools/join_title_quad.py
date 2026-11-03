# Join the splash title's TEXTURE to the QUAD the engine drew with it.
#
# WHY: the splash title's texture is 4096x128 with 2421 px of ink, and the
# screen shows the ink at 1:1 (121 px tall, clipped at the right edge).  The
# engine's own sprite geometry for that texture is what says whether the
# engine asked for a 2421 px wide sprite (the guest is wrong) or for a
# ~766 px wide sprite (then the PORT put the wrong texture, or the wrong
# size, into it).
#
# The text-quad probe logs the BOUND TEXTURE for every quad it watches, so
# the join is exact: texture id -> the quad that used it -> its vertices,
# its NDC and its screen rectangle.
#
# Usage: python tools\join_title_quad.py <guest-view.log>

import re
import sys

path = sys.argv[1]

with open(path, "r", encoding="utf-8", errors="replace") as fh:
    lines = fh.read().splitlines()

# 1. the title's texture: the createTexture whose boundsW is the width the
#    port measured for the 40-character player-ID line.
title_tex = None
title_bounds = None
for line in lines:
    if "phase=glyph-vector" in line and "method=createTexture" in line:
        m = re.search(r"boundsW=([0-9.]+)", line)
        t = re.search(r"texId=(-?\d+)", line)
        if m and t and float(m.group(1)) > 2000:
            title_tex = int(t.group(1))
            title_bounds = float(m.group(1))
            break

print(f"title texture : texId={title_tex} boundsW={title_bounds}")
if title_tex is None:
    print("the title's createTexture is not in this log")
    sys.exit(0)

# 2. every quad that used it.
hexid = f"{title_tex:016x}"
found = 0
for line in lines:
    if "phase=text-quad" not in line:
        continue
    if ("target=" + hexid) not in line:
        continue
    found += 1
    for field in ("a_Position", "a_TexCoord0", "ndc", "rect"):
        m = re.search(field + r"[:=]\s*(.*?)(?=\s+[a-zA-Z_]+[:=]|$)", line)
        if m:
            print(f"  {field:12s} {m.group(1).strip()}")
    if found >= 3:
        break
if found == 0:
    print(f"no text-quad used texture {title_tex} "
          f"(the probe only watches a sample of the draws)")
else:
    print(f"{found} quad(s) used it")
