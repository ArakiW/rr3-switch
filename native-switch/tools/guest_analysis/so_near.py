#!/usr/bin/env python3
"""Print the printable strings around a byte offset in the guest binary.

The .rodata section of a C++ binary lays the string literals of one function
out next to each other, so the neighbourhood of a string is usually the
vocabulary of the code that uses it.

Usage:
    python so_near.py <so> <hex-offset> [--before 40] [--after 40]
"""

import argparse
import re
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("offset")
    ap.add_argument("--before", type=int, default=40)
    ap.add_argument("--after", type=int, default=40)
    args = ap.parse_args()

    data = open(args.so, "rb").read()
    centre = int(args.offset, 0)
    lo = max(0, centre - args.before * 200)
    hi = min(len(data), centre + args.after * 200)
    rx = re.compile(rb"[\x20-\x7e]{4,}")
    for m in rx.finditer(data[lo:hi]):
        start = lo + m.start()
        mark = ">>" if start <= centre < start + len(m.group()) else "  "
        print("%s 0x%08x  %s" % (mark, start, m.group().decode("ascii", "replace")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
