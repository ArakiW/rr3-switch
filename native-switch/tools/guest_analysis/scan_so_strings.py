#!/usr/bin/env python3
"""Scan the guest .so string pool for joystick-config related literals.

Read-only helper for the throttle/lap investigation. It prints every ASCII
string in the guest binary that matches a keyword list, with its file offset,
so the surrounding cluster can be dumped and read by hand.
"""

import re
import sys

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"

KEYWORDS = [
    "joystick",
    "accel",
    "steering",
    "brake",
    "vendor:",
    "product:",
    "wheel",
    "invert",
    "ControllerProfile",
    "ControlScheme",
    "profile",
]


def main() -> int:
    with open(SO, "rb") as fh:
        blob = fh.read()

    out = []
    for m in re.finditer(rb"[\x20-\x7e]{4,120}", blob):
        s = m.group().decode("ascii")
        low = s.lower()
        for kw in KEYWORDS:
            if kw.lower() in low:
                out.append((m.start(), s))
                break

    limit = int(sys.argv[1]) if len(sys.argv) > 1 else 400
    for off, s in out[:limit]:
        print(f"{off:#010x} {s}")
    print(f"--- total matches: {len(out)} (printed {min(limit, len(out))}) ---")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
