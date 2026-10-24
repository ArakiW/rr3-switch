#!/usr/bin/env python3
"""Mine the guest's own rendered text out of an archived guest-view.log.

The port's glyph-vector probe logs every string the guest draws, with its
exact text. That is the guest's OWN telemetry, read out of the framebuffer
path the guest itself used, and it is the only instrument that can say
"the lap was completed" -- a PNG cannot.

Usage:  glyph_text.py <guest-view.log> [keyword ...]
"""

import re
import sys
from collections import OrderedDict

TEXT_RE = re.compile(r'phase=glyph-vector .*?\btext="((?:[^"\\]|\\.)*)"')


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    keywords = [k.lower() for k in sys.argv[2:]]

    seen = OrderedDict()   # text -> [count, first_line, last_line]
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            m = TEXT_RE.search(line)
            if not m:
                continue
            text = m.group(1)
            rec = seen.get(text)
            if rec is None:
                seen[text] = [1, lineno, lineno]
            else:
                rec[0] += 1
                rec[2] = lineno

    rows = []
    for text, (count, first, last) in seen.items():
        if keywords and not any(k in text.lower() for k in keywords):
            continue
        rows.append((first, last, count, text))

    rows.sort()
    print(f"# distinct texts: {len(seen)}   matching: {len(rows)}")
    for first, last, count, text in rows:
        print(f"first={first:<9} last={last:<9} n={count:<6} {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
