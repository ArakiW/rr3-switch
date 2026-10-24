#!/usr/bin/env python3
"""List the text the guest renders in a window of the guest-view log.

The port's glyph facade logs every string it rasterises (phase=glyph-vector
with text="..."), so the strings the guest asked for are in the log, in
order, with the line number that says WHEN.

Usage:
    python race_text.py <archive-dir> [--lo 197000] [--hi 209600]
"""

import argparse
import os
import re
import sys

VECTOR = re.compile(r'phase=glyph-vector.*?text="([^"]*)"')
UTF = re.compile(r'phase=jni-utf163.*?text=(.*)$')
ANY = re.compile(r'phase=(glyph-vector|jni-utf163|glyph-glyph)\b')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--lo", type=int, default=0)
    ap.add_argument("--hi", type=int, default=1 << 30)
    ap.add_argument("--unique", action="store_true")
    args = ap.parse_args()

    log = os.path.join(args.archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log")
        return 1
    with open(log, "r", encoding="utf-8", errors="replace") as handle:
        lines = handle.readlines()

    out = []
    for index, line in enumerate(lines):
        if not (args.lo <= index <= args.hi):
            continue
        m = VECTOR.search(line)
        if m:
            out.append((index, "vector", m.group(1)))
            continue
        m = UTF.search(line)
        if m:
            out.append((index, "utf", m.group(1).strip()))
    print("text lines in the window: %d" % len(out))
    if args.unique:
        seen = []
        for index, kind, text in out:
            if text not in [s[2] for s in seen]:
                seen.append((index, kind, text))
        out = seen
    for index, kind, text in out:
        print("  line %-8d %-6s %r" % (index, kind, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
