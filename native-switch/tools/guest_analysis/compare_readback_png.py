#!/usr/bin/env python3
"""Compare the probe's framebuffer readback with the captured PNG.

TWO INDEPENDENT INSTRUMENTS
---------------------------
The probe reads the quad's own rectangle out of the framebuffer immediately
after the draw (GL coordinates, BOTTOM-left origin). The captured PNG is the
whole back buffer (image coordinates, TOP-left origin). If the two agree
about the pixels in the same rectangle, the draw landed and the picture is
not missing it.

The Y flip is the whole point of the comparison: a rectangle that is
(x, y, w, h) in GL space is (x, height - y - h, w, h) in image space, and
comparing them without the flip compares two different places.

Usage:
    python compare_readback_png.py <archive-dir> [--png frame-000300.png]
                                   [--phase hud-frame-draw] [--limit 40]
"""

import argparse
import os
import re
import sys

from PIL import Image

READBACK = re.compile(
    r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) whiteAfter=(\d+) "
    r"maxRBefore=(\d+) maxRAfter=(\d+) uniqAfter=(\d+)")
RECT = re.compile(r"rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) onScreen=(\d)")
HEAD = re.compile(r"^phase=(\S+) ordinal=(\d+) target=([0-9a-fx]+)")


def stats(pixels):
    """The same statistics the probe computes: white count, max R, uniques."""
    import numpy as np
    arr = np.asarray(pixels, dtype=np.int32)
    if arr.size == 0:
        return 0, 0, 0
    r, g, b = arr[:, 0], arr[:, 1], arr[:, 2]
    white = int(((r > 200) & (g > 200) & (b > 200)).sum())
    maxr = int(r.max()) if r.size else 0
    unique = len(set(map(tuple, arr[:, :3].tolist())))
    return white, maxr, unique


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--png", default=None, help="PNG file name inside frames/")
    ap.add_argument("--phase", default="hud-frame-draw")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    frames_dir = os.path.join(args.archive, "frames")
    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log")
        return 1

    pngs = sorted(os.listdir(frames_dir)) if os.path.isdir(frames_dir) else []
    print("PNGs in the archive: %s" % (", ".join(pngs) if pngs else "NONE"))

    image = None
    height = width = 0
    if args.png:
        path = os.path.join(frames_dir, args.png)
        if not os.path.isfile(path):
            print("no such PNG: %s" % path)
            return 1
        image = Image.open(path).convert("RGBA")
        width, height = image.size
        print("using %s (%dx%d)" % (args.png, width, height))

    # Pair every draw line with the text-quad line that follows it.
    pairs = []
    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()
    for index, line in enumerate(lines):
        head = HEAD.match(line)
        if head is None or head.group(1) != args.phase:
            continue
        quad = None
        for j in range(index + 1, min(index + 4, len(lines))):
            if lines[j].startswith("phase=text-quad"):
                quad = lines[j]
                break
        if quad is None:
            continue
        m = READBACK.search(quad)
        r = RECT.search(quad)
        if m is None or r is None:
            continue
        pairs.append({
            "line": index,
            "tex": int(head.group(2)),
            "draw": int(head.group(3), 16),
            "rw": int(m.group(1)), "rh": int(m.group(2)),
            "x": int(m.group(3)), "y": int(m.group(4)),
            "wb": int(m.group(5)), "wa": int(m.group(6)),
            "rb": int(m.group(7)), "ra": int(m.group(8)),
            "uq": int(m.group(9)),
            "rect": r.group(0),
            "on": int(r.group(5)),
        })

    print("\n== %d %s draws with a readback ==" % (len(pairs), args.phase))
    shown = 0
    for p in pairs:
        verdict = "PAINTED" if (p["wa"] != p["wb"] or p["ra"] != p["rb"]) else "no-change"
        line = ("tex=%-5d draw#=%-7d rect=%-18s readback=%dx%d@%d,%d "
                "white %d->%d maxR %d->%d uniq=%d  %s"
                % (p["tex"], p["draw"], p["rect"], p["rw"], p["rh"], p["x"], p["y"],
                   p["wb"], p["wa"], p["rb"], p["ra"], p["uq"], verdict))
        if image is not None:
            # GL (x, y) is the BOTTOM-left corner; the image is top-left.
            top = height - p["y"] - p["rh"]
            left = p["x"]
            if top < 0 or left < 0 or top + p["rh"] > height or left + p["rw"] > width:
                line += "  [PNG: RECT OUTSIDE THE IMAGE]"
            else:
                crop = image.crop((left, top, left + p["rw"], top + p["rh"]))
                pw, pmr, puq = stats(crop)
                agree = (pw == p["wa"] and pmr == p["ra"])
                line += "  | PNG white=%d maxR=%d uniq=%d  %s" % (
                    pw, pmr, puq, "AGREE" if agree else "DIFFER")
        print(line)
        shown += 1
        if shown >= args.limit:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
