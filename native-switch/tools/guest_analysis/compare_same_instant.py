#!/usr/bin/env python3
"""Compare the probe's readback with the PNG taken at the SAME INSTANT.

The probe reads the quad's own rectangle out of the framebuffer right after
the draw, and then writes the whole back buffer out as a PNG. Both come from
the same framebuffer at the same moment, through different code paths, so
they are two independent instruments on the same pixels.

The readback rectangle is in GL coordinates (BOTTOM-left origin) and the PNG
rows are counted from the top, so the comparison has to flip Y.

Usage:
    python compare_same_instant.py <archive-dir> [--limit 40]
"""

import argparse
import os
import re
import sys

from PIL import Image
import numpy as np

DRAW = re.compile(r"^phase=hud-frame-draw ordinal=(\d+) target=([0-9a-fx]+) (.*)$")
QUAD = re.compile(
    r"rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) onScreen=(\d)")
READ = re.compile(
    r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) whiteAfter=(\d+) "
    r"maxRBefore=(\d+) maxRAfter=(\d+) uniqAfter=(\d+)")
TEXWH = re.compile(r" texWH=(\d+x\d+)")
COLOUR = re.compile(r"u_CurrentColour\(type=0x[0-9a-f]+ \[([^\]]*)\]")


def stats(arr):
    flat = arr.reshape(-1, arr.shape[-1])
    r, g, b = flat[:, 0], flat[:, 1], flat[:, 2]
    white = int(((r > 200) & (g > 200) & (b > 200)).sum())
    maxr = int(r.max()) if r.size else 0
    rows = flat[:, :3].tolist()
    unique = len({tuple(px) for px in rows}) if rows else 0
    return white, maxr, unique


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--frames", default=None)
    args = ap.parse_args()

    frames_dir = os.path.join(args.archive, "frames")
    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log -> VOID RUN")
        return 1

    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    pngs = {}
    for name in os.listdir(frames_dir):
        if name.startswith("frame-hud-") and name.endswith(".png"):
            # frame-hud-<frame>-<drawordinal>.png
            stem = name[len("frame-hud-"):-len(".png")]
            frame, _, ordinal = stem.partition("-")
            pngs[(int(frame), int(ordinal))] = os.path.join(frames_dir, name)
    print("same-instant PNGs in the archive: %d" % len(pngs))
    if not pngs:
        return 2

    agree = disagree = 0
    shown = 0
    for index, line in enumerate(lines):
        m = DRAW.match(line)
        if m is None:
            continue
        tex = int(m.group(1))
        quad = None
        for j in range(index + 1, min(index + 4, len(lines))):
            if lines[j].startswith("phase=text-quad"):
                quad = lines[j]
                break
        if quad is None:
            continue
        # The DRAW line's target is the vertex COUNT; the draw ORDINAL is in
        # the text-quad line's target, in hex.
        qm = re.match(r"^phase=text-quad ordinal=(\d+) target=([0-9a-fx]+)", quad)
        if qm is None:
            continue
        ordinal = int(qm.group(2), 16)
        rect = QUAD.search(quad)
        read = READ.search(quad)
        if rect is None or read is None:
            continue
        # The PNG is named by the FRAME and the draw ordinal. The frame is
        # not in the probe's own line, so find it from the file name.
        key = None
        for candidate in pngs:
            if candidate[1] == ordinal:
                key = candidate
                break
        if key is None:
            continue
        frame = key[0]
        if args.frames and frame not in {int(x) for x in args.frames.split(",")}:
            continue
        path = pngs[key]
        if not os.path.isfile(path):
            continue
        image = Image.open(path).convert("RGBA")
        width, height = image.size
        rx, ry, rw, rh = (int(rect.group(i)) for i in range(1, 5))
        # The probe's readback is CLAMPED to 256x64 at the rectangle's own
        # bottom-left corner, so the PNG has to be cropped to the SAME
        # rectangle: comparing the PNG's whole quad with the probe's clamped
        # patch compares two different sets of pixels.
        rw = int(read.group(1))
        rh = int(read.group(2))
        rx = int(read.group(3))
        ry = int(read.group(4))
        top = height - ry - rh
        left = rx
        if top < 0 or left < 0 or top + rh > height or left + rw > width:
            print("  tex=%-5d frame=%-6d rect OUTSIDE the PNG" % (tex, frame))
            continue
        crop = np.asarray(image.crop((left, top, left + rw, top + rh)), dtype=np.int32)
        pw, pmr, puq = stats(crop)
        rb_white = int(read.group(6))
        rb_maxr = int(read.group(8))
        rb_uniq = int(read.group(9))
        # The probe's unique-colour count stops at 64, so only the white
        # count and the maximum red are comparable numbers.
        ok = (pw == rb_white and pmr == rb_maxr)
        if ok:
            agree += 1
        else:
            disagree += 1
        if shown < args.limit:
            print("  tex=%-5d frame=%-6d rect=%d,%d,%d,%d  readback white=%-5d maxR=%-3d uniq=%-3d | "
                  "PNG white=%-5d maxR=%-3d uniq=%-3d  %s"
                  % (tex, frame, rx, ry, rw, rh, rb_white, rb_maxr, rb_uniq,
                     pw, pmr, puq, "AGREE" if ok else "DIFFER"))
        shown += 1
    print("\n== %d draws compared: %d AGREE, %d DIFFER ==" % (shown, agree, disagree))
    return 0


if __name__ == "__main__":
    sys.exit(main())
