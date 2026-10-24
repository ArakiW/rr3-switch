#!/usr/bin/env python3
"""The exhaust flame: a THIRD, independent instrument for "the car is driving".

The guest's own message ("hold the brake to reverse") says the car is not
moving, and the guest's own axis array says what the guest was given. Both
are text in a log. This one is the PIXELS: when the car accelerates, the
guest draws the exhaust flame at the back of the car, and the flame is a
small, very bright, warm blob in the bottom centre of the frame.

The measurement is deliberately narrow -- the box under the car's tail -- so
that the sky, the grandstands and the HUD cannot contribute. The number
reported is the brightest pixel in that box and the mean of the pixels that
are bright AND warm (R clearly above B), which is what a flame is and what
a white number or a red light is not.

Usage:  exhaust_flame.py <frame.png> [<frame.png> ...]
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

# The box under the car's tail, in the chase camera's view. x 540..720,
# y 600..700 of the 1280x720 back buffer.
BOX = (540, 600, 720, 700)


def measure(path: Path) -> tuple[int, int, float]:
    with Image.open(path) as im:
        rgb = np.asarray(im.convert("RGB"), dtype=np.int16)
    x0, y0, x1, y1 = BOX
    patch = rgb[y0:y1, x0:x1]
    brightness = patch.max(axis=2)
    warmest = int(brightness.max())
    red = patch[:, :, 0]
    blue = patch[:, :, 2]
    warm = (brightness > 140) & (red > blue + 40)
    mean_warm = float(patch[:, :, 0][warm].mean()) if warm.any() else 0.0
    return warmest, int(warm.sum()), mean_warm


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    print("file                          maxR  warmPx  warmMean")
    for name in sys.argv[1:]:
        path = Path(name)
        brightest, warm_pixels, mean_warm = measure(path)
        print(f"{path.name:<28} {brightest:<5} {warm_pixels:<7} {mean_warm:<8.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
