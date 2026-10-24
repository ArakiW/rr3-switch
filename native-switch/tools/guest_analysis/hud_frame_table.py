#!/usr/bin/env python3
"""The HUD's 2D-program draws in one race frame, in full.

Every field the question asks for: the texture, its size, the screen
rectangle, the UV rectangle, the colour, and the before/after readback.

The probe's rectangle is in GL coordinates (BOTTOM-left origin), so the
screen rows are given as well, counted from the top.

Usage:
    python hud_frame_table.py <archive-dir> [--frame 1623] [--phase hud-frame-draw]
"""

import argparse
import os
import re
import sys

DRAW = re.compile(r"^phase=(hud-frame-draw|hud-draw|text-draw) ordinal=(\d+) "
                  r"target=([0-9a-fx]+) (.*)$")
QUAD = re.compile(r"rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) onScreen=(\d)")
READ = re.compile(
    r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) whiteAfter=(\d+) "
    r"maxRBefore=(\d+) maxRAfter=(\d+) uniqAfter=(\d+)")
TEXWH = re.compile(r" texWH=(\d+x\d+)")
COLOUR = re.compile(r"u_CurrentColour\(type=0x[0-9a-f]+ \[([^\]]*)\]")
UV = re.compile(r"a_TexCoord0: (.*?) ndc=")
FBO = re.compile(r" fbo=(\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--phase", default="hud-frame-draw")
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    # The census line for frame N is written at the first draw of frame N+1,
    # so the draws that FOLLOW a census line belong to the frame the NEXT
    # census line names.
    frames = {}
    pending = []
    for index, line in enumerate(lines):
        m = DRAW.match(line)
        if m and m.group(1) == args.phase:
            quad = None
            for j in range(index + 1, min(index + 4, len(lines))):
                if lines[j].startswith("phase=text-quad"):
                    quad = lines[j]
                    break
            pending.append((m.group(2), m.group(4), quad))
            continue
        if line.startswith("phase=hud-census"):
            frame = int(re.match(r"phase=hud-census ordinal=(\d+)", line).group(1))
            if pending:
                frames.setdefault(frame, []).extend(pending)
            pending = []
    if pending:
        frames.setdefault(0, []).extend(pending)

    if args.frame:
        frames = {args.frame: frames.get(args.frame, [])}

    for frame in sorted(frames):
        print("\n== frame %d: %d draws ==" % (frame, len(frames[frame])))
        print("%-6s %-10s %-5s %-19s %-9s %-22s %s"
              % ("tex", "size", "fbo", "rect(x,y,w,h)", "rows", "colour", "readback"))
        for tex, details, quad in frames[frame]:
            wh = TEXWH.search(details)
            col = COLOUR.search(details)
            fbo = FBO.search(details)
            rect = QUAD.search(quad) if quad else None
            read = READ.search(quad) if quad else None
            uv = UV.search(quad) if quad else None
            if rect:
                x, y, w, h = (int(rect.group(i)) for i in range(1, 5))
                rows = "%d..%d" % (720 - y - h, 720 - y)
            else:
                x = y = w = h = 0
                rows = "?"
            if read:
                painted = (read.group(6) != read.group(5) or
                           read.group(8) != read.group(7))
                stat = ("w %s->%s maxR %s->%s %s"
                        % (read.group(5), read.group(6), read.group(7),
                           read.group(8), "PAINTED" if painted else "no-change"))
            else:
                stat = "no readback (off-screen or zero area)"
            uvs = ""
            if uv:
                vals = re.findall(r"v\d=\(([-\d.]+),([-\d.]+)\)", uv.group(1))
                if vals:
                    u0 = min(float(v[0]) for v in vals)
                    u1 = max(float(v[0]) for v in vals)
                    v0 = min(float(v[1]) for v in vals)
                    v1 = max(float(v[1]) for v in vals)
                    uvs = "UV %g..%g,%g..%g" % (u0, u1, v0, v1)
            print("%-6s %-10s %-5s %-19s %-9s %-22s %s  %s"
                  % (tex, wh.group(1) if wh else "?", fbo.group(1) if fbo else "?",
                     "%d,%d,%d,%d" % (x, y, w, h), rows,
                     col.group(1) if col else "?", stat, uvs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
