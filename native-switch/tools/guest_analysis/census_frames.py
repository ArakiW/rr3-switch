#!/usr/bin/env python3
"""Segment the probe's per-frame draws by the census that frames them.

The census line for frame N is written when the FIRST draw of frame N+1
arrives, so the detail lines that FOLLOW a census line belong to the frame
the NEXT census line names. That is the only ordering the log gives, and it
is enough: the census line and the detail lines are written by the same
thread, in order, with no other writer in between.

Usage:
    python census_frames.py <archive-dir> [--from 1600] [--to 1700]
                            [--phase hud-frame-draw]
"""

import argparse
import os
import re
import sys

CENSUS = re.compile(r"^phase=hud-census ordinal=(\d+) target=([0-9a-fx]+) (.*)$")
DRAW = re.compile(r"^phase=(hud-frame-draw|hud-draw|text-draw) ordinal=(\d+) "
                  r"target=([0-9a-fx]+) (.*)$")
QUAD = re.compile(
    r"rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+) onScreen=(\d)")
READ = re.compile(
    r"readback=(\d+)x(\d+) at=(-?\d+),(-?\d+) whiteBefore=(\d+) whiteAfter=(\d+) "
    r"maxRBefore=(\d+) maxRAfter=(\d+) uniqAfter=(\d+)")
TEXWH = re.compile(r" texWH=(\d+x\d+)")
COLOUR = re.compile(r"u_CurrentColour\(type=0x[0-9a-f]+ \[([^\]]*)\]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=1 << 30)
    ap.add_argument("--phase", default="hud-frame-draw")
    ap.add_argument("--max-frames", type=int, default=3)
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log -> VOID RUN")
        return 1

    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    # Records: census markers and draw/quad pairs, in log order.
    records = []
    for index, line in enumerate(lines):
        m = CENSUS.match(line)
        if m:
            records.append(("census", int(m.group(1)), None, None))
            continue
        m = DRAW.match(line)
        if m and m.group(1) == args.phase:
            quad = None
            for j in range(index + 1, min(index + 4, len(lines))):
                if lines[j].startswith("phase=text-quad"):
                    quad = lines[j]
                    break
            records.append(("draw", m.group(2), m.group(4), quad))

    frames = {}
    census_text = {}
    pending = []
    for kind, a, b, c in records:
        if kind == "census":
            frame = a
            if pending:
                frames.setdefault(frame, []).extend(pending)
                census_text[frame] = b
            pending = []
        else:
            pending.append((a, b, c))
    if pending:
        frames.setdefault(0, []).extend(pending)

    print("census frames: %d   frames with %s draws: %d"
          % (sum(1 for r in records if r[0] == "census"),
             args.phase, len(frames)))

    shown = 0
    for frame in sorted(frames):
        if not (args.first <= frame <= args.last):
            continue
        draws = frames[frame]
        print("\n-- frame %d: %d %s draws" % (frame, len(draws), args.phase))
        print("   census: %s" % (census_text.get(frame, "?")))
        for tex, details, quad in draws:
            wh = TEXWH.search(details)
            col = COLOUR.search(details)
            glyph = "glyph" in details and " glyph=1" in details
            rect = QUAD.search(quad) if quad else None
            read = READ.search(quad) if quad else None
            if rect:
                rx, ry, rw, rh = (int(rect.group(i)) for i in range(1, 5))
                # The probe's rectangle is in GL coordinates (bottom-left
                # origin); the screen rows are counted from the top.
                rows = "%d..%d" % (720 - ry - rh, 720 - ry)
            else:
                rx = ry = rw = rh = 0
                rows = "?"
            if read:
                painted = (read.group(6) != read.group(5) or
                           read.group(8) != read.group(7))
                stat = ("readback %sx%s@%s,%s white %s->%s maxR %s->%s uniq=%s %s"
                        % (read.group(1), read.group(2), read.group(3),
                           read.group(4), read.group(5), read.group(6),
                           read.group(7), read.group(8), read.group(9),
                           "PAINTED" if painted else "no-change"))
            else:
                stat = "no readback"
            print("   tex=%-5s %-9s %-9s x=%-5d y=%-4d w=%-5d h=%-4d rows %-10s "
                  "colour=%-24s %s"
                  % (tex, wh.group(1) if wh else "?",
                     "GLYPH" if glyph else "sprite", rx, ry, rw, rh, rows,
                     col.group(1) if col else "?", stat))
        shown += 1
        if shown >= args.max_frames:
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
