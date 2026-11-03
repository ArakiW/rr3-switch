# Group the splash title's GlyphVector calls by JAVA OBJECT.
#
# WHY: the guest asks for the SAME 40-character string at two sizes --
# 140 (which the port measures at 2421 px) and 42 (719 px).  The engine put
# the line's left edge where a ~766 px wide line would be centred, and the
# pixels show 2421 px of ink drawn at 1:1.  So the layout width and the drawn
# texture disagree by 3.16x.
#
# The question that settles it: are those two sizes the SAME Java object?
#
#   * same object, re-init'd  -> the port's published metrics (719) and its
#     uploaded texture (2421 px of ink) describe DIFFERENT strings/sizes, and
#     the engine drew the old texture into the new box.  That is a port bug:
#     the texture and the metrics have to describe the same raster.
#   * different objects      -> the engine mixed them up itself, which is not
#     something the port can fix.
#
# The object handle is in the cc-dispatch line that precedes each call.
#
# Usage: python tools\group_glyph_objects.py <guest-view.log>

import re
import sys

path = sys.argv[1]
OBJ = re.compile(r"method=(\w+).*?obj=(0x[0-9a-f]+)")
TEXT = re.compile(r"text=([^\"]{0,60})")

with open(path, "r", encoding="utf-8", errors="replace") as fh:
    lines = fh.read().splitlines()

# The object handle of the call that is currently being dispatched.
current_obj = None
# obj -> list of (method, size, boundsW, texId)
calls = {}

for line in lines:
    if "phase=cc-dispatch" in line:
        m = OBJ.search(line)
        if m:
            current_obj = m.group(2)
        continue
    if "phase=glyph-vector" not in line:
        continue
    method = re.search(r"method=(\w+)", line)
    size = re.search(r"size=([0-9.]+)", line)
    bounds = re.search(r"boundsW=([0-9.]+)", line)
    tex = re.search(r"texId=(-?\d+)", line)
    if not method or current_obj is None:
        continue
    entry = calls.setdefault(current_obj, [])
    entry.append((method.group(1),
                  size.group(1) if size else "-",
                  bounds.group(1) if bounds else "-",
                  tex.group(1) if tex else "-"))

print(f"{len(calls)} glyph objects saw a dispatch in this log\n")

# The objects that were measured at MORE THAN ONE size are the interesting
# ones: those are the ones where the layout width and the drawn texture can
# come from different measurements.
mixed = 0
for obj, entry in sorted(calls.items(), key=lambda kv: str(kv[0])):
    sizes = sorted({e[1] for e in entry if e[1] != "-" and float(e[1]) > 0})
    if len(sizes) < 2:
        continue
    mixed += 1
    print(f"object {obj}   sizes={sizes}")
    for method, size, bounds, tex in entry:
        print(f"    {method:18s} size={size:>7s} boundsW={bounds:>8s} texId={tex}")
    print()

print(f"{mixed} object(s) were measured at more than one size")
