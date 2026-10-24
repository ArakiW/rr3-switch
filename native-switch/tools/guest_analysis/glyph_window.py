#!/usr/bin/env python3
"""Dump every distinct text the guest rendered, with the log line it appeared on.

Usage: glyph_window.py <guest-view.log> [min_line]

The port's glyph-vector probe records the exact string of every glyph run the
guest draws. That is the guest's OWN on-screen telemetry, so it is the
instrument for questions a framebuffer cannot answer ("did the lap counter
change", "what did the HUD say the speed was").
"""

import re
import sys

TEXT_RE = re.compile(r'phase=glyph-vector .*?\btext="((?:[^"\\]|\\.)*)"')


def main() -> int:
    path = sys.argv[1]
    min_line = int(sys.argv[2]) if len(sys.argv) > 2 else 0

    seen = {}
    order = []
    total = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            total = lineno
            m = TEXT_RE.search(line)
            if not m:
                continue
            text = m.group(1)
            if text in seen:
                continue
            seen[text] = lineno
            order.append((lineno, text))

    window = [(l, t) for l, t in order if l >= min_line]
    print(f"total_log_lines={total} distinct_texts={len(order)} "
          f"window>={min_line} -> {len(window)}")
    for lineno, text in window:
        # The guest renders localised (Chinese) text here, and the Windows
        # console codec is GBK, so the text is escaped rather than printed.
        safe = text[:160].encode("unicode_escape").decode("ascii")
        print(f"{lineno}\t{safe}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
