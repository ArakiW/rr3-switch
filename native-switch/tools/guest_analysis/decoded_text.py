#!/usr/bin/env python3
"""Print a decoded, frame-stamped slice of the guest's rendered text.

The port's glyph-vector probe logs the text with non-ASCII bytes escaped, so
this decodes them back to UTF-8 for reading.

Usage: decoded_text.py <text-timeline.txt> <min_frame> [max_frame]
"""

import io
import re
import sys

ROW = re.compile(r"^f=(\d+)\s+line=(\d+)\s+(.*)$")


def main() -> int:
    path = sys.argv[1]
    lo = int(sys.argv[2])
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else lo + 100
    out = io.open(sys.argv[4], "w", encoding="utf-8") if len(sys.argv) > 4 else sys.stdout
    for line in io.open(path, encoding="utf-8", errors="replace"):
        m = ROW.match(line.rstrip("\n"))
        if not m:
            continue
        frame = int(m.group(1))
        if not (lo <= frame <= hi):
            continue
        raw = m.group(3)
        try:
            text = raw.encode("latin-1", "backslashreplace").decode("unicode_escape")
        except Exception:
            text = raw
        out.write(f"f={frame}\t{text}\n")
    out.flush()
    if out is not sys.stdout:
        out.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
