#!/usr/bin/env python3
"""Search the guest .so string pool for race-telemetry literals.

The guest's rendered HUD does not reach the framebuffer in this build, so the
lap telemetry has to come from somewhere else. The first place to look is the
guest's own string pool: if the guest has a lap/sector/finish message, it
exists as a literal here and can be matched against the guest's log.
"""

import re
import sys

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"

KEYWORDS = [
    "lap", "sector", "split time", "split_time", "best time", "besttime",
    "race finish", "racefinish", "finished the race", "completed a lap",
    "checkpoint", "check point", "timing", "position", "placement",
    "finished", "lap time", "laptime", "lapcount", "lap count",
]


def main() -> int:
    with open(SO, "rb") as fh:
        blob = fh.read()

    hits = []
    for m in re.finditer(rb"[\x20-\x7e]{3,160}", blob):
        s = m.group().decode("ascii")
        low = s.lower()
        for kw in KEYWORDS:
            if kw in low:
                hits.append((m.start(), s))
                break

    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    for off, s in hits[:limit]:
        print(f"{off:#010x} {s}")
    print(f"--- matches: {len(hits)} (printed {min(limit, len(hits))}) ---")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
