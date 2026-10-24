#!/usr/bin/env python3
"""Frame-stamped timeline of the text the guest draws on screen.

The port's glyph-vector probe records the exact string of every glyph run the
guest draws. That is the guest's OWN on-screen telemetry: it is the only
instrument that can say "the guest thinks the car has stopped" or "the guest
says the lap is done", because the race HUD's numbers are not drawn in this
build and a PNG cannot be read for meaning.

The guest-view.log is written by several threads at once, so a line's position
in the file is not its time. The port's input-door line carries the frame
counter, so the frame is the last input-door frame at or before the line.

Usage: text_timeline.py <guest-view.log> [keyword ...]
        text_timeline.py <guest-view.log> --all
"""

import re
import sys

FRAME_RE = re.compile(r'phase=input-door .*?\bf=(\d+)\b')
TEXT_RE = re.compile(r'phase=glyph-vector .*?\btext="((?:[^"\\]|\\.)*)"')


def main() -> int:
    path = sys.argv[1]
    args = sys.argv[2:]
    show_all = "--all" in args
    keywords = [k.lower() for k in args if not k.startswith("--")]

    frame = 0
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            m = FRAME_RE.search(line)
            if m:
                frame = int(m.group(1))
                continue
            m = TEXT_RE.search(line)
            if not m:
                continue
            text = m.group(1)
            if not show_all and keywords:
                if not any(k in text.lower() for k in keywords):
                    continue
            rows.append((frame, lineno, text))

    print(f"# text draws matched: {len(rows)}")
    for frame, lineno, text in rows:
        safe = text.encode("unicode_escape").decode("ascii")
        print(f"f={frame:<7} line={lineno:<8} {safe}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
