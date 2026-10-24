#!/usr/bin/env python3
"""Search the guest binary for the strings that name the HUD's text.

The guest binary is the oracle. This prints the printable ASCII runs that
match a pattern, with their file offset, so a candidate can be looked up in
the decompiled code.

Usage:
    python so_strings.py <so> <regex> [--min 4] [--ctx 0]
"""

import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("pattern")
    ap.add_argument("--min", type=int, default=4)
    ap.add_argument("--limit", type=int, default=400)
    args = ap.parse_args()

    data = open(args.so, "rb").read()
    rx = re.compile(rb"[\x20-\x7e]{%d,}" % args.min)
    hits = 0
    for m in rx.finditer(data):
        s = m.group().decode("ascii", "replace")
        if re.search(args.pattern, s, re.I):
            print("0x%08x  %s" % (m.start(), s))
            hits += 1
            if hits >= args.limit:
                print("... truncated at %d hits" % args.limit)
                break
    print("== %d hits in %d bytes" % (hits, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
