# What exactly is the magenta on those cars?
#
# WHY: "magenta" has two very different causes and they need different fixes.
#   - classic missing-texture magenta, exactly (255, 0, 255): the texture never
#     reached the GPU.
#   - a livery COLOUR (the game draws distant rivals with a flat paint colour
#     taken from the livery table): the texture is fine and the colour path is
#     wrong.
# Counting "pink pixels" cannot tell them apart, and the pixel scanner already
# proved that (an advertising billboard is pink too). So sample the EXACT
# colours and crop the region so it can be looked at.
#
# Usage: python magenta-sample.py <frame.png> [out-prefix]
import sys
from collections import Counter
from pathlib import Path

from PIL import Image

frame = Path(sys.argv[1])
prefix = Path(sys.argv[2]) if len(sys.argv) > 2 else frame.with_suffix("")
img = Image.open(frame).convert("RGB")
w, h = img.size
px = img.load()

exact = Counter()
near = Counter()
xs, ys = [], []
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        if r > 180 and b > 180 and g < 90:
            near[(r, g, b)] += 1
            xs.append(x)
            ys.append(y)
            if (r, g, b) == (255, 0, 255):
                exact[(r, g, b)] += 1

print(f"frame      : {frame}")
print(f"size       : {w}x{h}")
print(f"near-pink  : {sum(near.values())} px  ({100.0 * sum(near.values()) / (w * h):.4f} % of frame)")
print(f"exact 255/0/255 : {sum(exact.values())} px")
print("top near-pink colours:")
for colour, count in near.most_common(12):
    print(f"   rgb{colour}  x{count}")

if xs:
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    print(f"bbox       : x {x0}..{x1}  y {y0}..{y1}")
    pad = 40
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(w - 1, x1 + pad)
    y1 = min(h - 1, y1 + pad)
    crop = img.crop((x0, y0, x1 + 1, y1 + 1))
    scale = max(1, min(6, 900 // max(1, crop.width)))
    crop = crop.resize((crop.width * scale, crop.height * scale), Image.NEAREST)
    out = Path(str(prefix) + ".magenta-crop.png")
    crop.save(out)
    print(f"crop saved : {out}  ({crop.width}x{crop.height})")
else:
    print("no pink pixels found")
