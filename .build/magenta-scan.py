"""Scan every frame of a round for the magenta signature, and report the colours.

WHY: the magenta on the AI cars had TWO possible causes (a texture that never
arrived, exactly (255,0,255); or a decode that zeroed the green channel, so
(255,4,255) and (192,4,192)), and a pink advertising billboard is NOT a fault
at all -- its green channel is 77, not 0.  So the scan has to look at the
GREEN CHANNEL, not at "pinkish".

Usage: python magenta-scan.py <frames-dir> [more-dirs...]
"""
import sys
from collections import Counter
from pathlib import Path

from PIL import Image

# The signature: red and blue high, GREEN ZERO.  The billboard's pink has
# green=77 and must not be counted.
GREEN_ZERO_LIMIT = 12


def scan(directory: Path) -> None:
    frames = sorted(directory.glob("*.png"))
    if not frames:
        print("%s: no frames" % directory)
        return
    worst = []
    zero_green_total = 0
    exact_magenta_total = 0
    pixels_total = 0
    low_green_colors = Counter()
    for frame in frames:
        img = Image.open(frame).convert("RGB")
        colors = Counter(img.getdata())
        pixels_total += sum(colors.values())
        exact = colors.get((255, 0, 255), 0)
        zero_green = 0
        for color, count in colors.items():
            r, g, b = color
            if g <= GREEN_ZERO_LIMIT and r >= 128 and b >= 128:
                zero_green += count
                low_green_colors[color] += count
        exact_magenta_total += exact
        zero_green_total += zero_green
        worst.append((zero_green, frame.name))
    worst.sort(reverse=True)
    print("=== %s ===" % directory)
    print("  frames              : %d" % len(frames))
    print("  pixels              : %d" % pixels_total)
    print("  EXACT (255,0,255)   : %d" % exact_magenta_total)
    print("  green<=%d, r,b>=128 : %d  (%.5f%% of all pixels)" %
          (GREEN_ZERO_LIMIT, zero_green_total,
           100.0 * zero_green_total / max(1, pixels_total)))
    print("  worst frames        : %s" % ", ".join("%s=%d" % (n, c) for c, n in worst[:5]))
    if low_green_colors:
        print("  the low-green colours: %s" %
              ", ".join("%s x%d" % (c, n) for c, n in low_green_colors.most_common(6)))
    else:
        print("  the low-green colours: none")
    print()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for arg in sys.argv[1:]:
        scan(Path(arg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
