#!/usr/bin/env python3
"""List the text the port's glyph facade rasterised, with its size.

The port logs one line per call (the first few of each (method, size) key).
`textLen=0` is a LOGGER ARTEFACT -- the port's own comment says the first
call of a new size key legitimately has no text yet -- so a textLen=0 line
says nothing about the text.

Usage:
    python glyph_calls.py <archive-dir> [--regex RE] [--last 30]
"""

import argparse
import os
import re
import sys

V = re.compile(
    r'phase=glyph-vector.*?method=(\S+) textLen=(\d+) size=([\d.]+).*?text="([^"]*)"')
TEX = re.compile(r'phase=glyph-tex.*?texture=(\S+) inked=(\d+) of (\d+).*?texId=(\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--regex", default=None)
    ap.add_argument("--last", type=int, default=30)
    ap.add_argument("--from-line", type=int, default=0)
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    rows = []
    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        for index, line in enumerate(handle):
            if index < args.from_line:
                continue
            m = V.search(line)
            if m:
                rows.append((index, m.group(1), int(m.group(2)),
                             m.group(3), m.group(4)))
    print("glyph-vector lines: %d" % len(rows))
    sel = rows
    if args.regex:
        pattern = re.compile(args.regex)
        sel = [r for r in rows if pattern.search(r[4])]
        print("matching: %d" % len(sel))
    for row in sel[-args.last:]:
        print("  line %-8d %-14s textLen=%-4d size=%-7s %r" % row)
    return 0


if __name__ == "__main__":
    sys.exit(main())
