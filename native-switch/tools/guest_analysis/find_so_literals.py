#!/usr/bin/env python3
"""Find exact literal strings in the guest .so and report their file offsets.

Used to locate the native joystick-config matcher: the log line the guest
prints ("Cannot find description for device: ... using default: [...]") and the
config keys it parses ("accel", "steering", ...) are all in the guest's own
string pool, so their offsets say where the matcher lives.
"""

import re
import sys

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"

EXACT = [
    b"Cannot find description for device",
    b"using default",
    b"Android Gamepad Extended",
    b"Android Gamepad Basic",
    b"joystick_config.txt",
    b"accel",
    b"brake",
    b"steering",
    b"axis",
    b"button",
    b"hat",
    b"invert",
    b"default",
    b"wheel",
    b"vendor:",
    b"product:",
    b"successfully loaded joystick settings file",
]


def main() -> int:
    with open(SO, "rb") as fh:
        blob = fh.read()

    for needle in EXACT:
        hits = []
        start = 0
        while True:
            i = blob.find(needle, start)
            if i < 0:
                break
            # Show a little context so a substring hit is distinguishable from
            # the real literal (e.g. "accel" inside "Accelerometer").
            ctx = blob[max(0, i - 24): i + len(needle) + 24]
            printable = re.sub(rb"[^\x20-\x7e]", b".", ctx).decode("ascii")
            hits.append((i, printable))
            start = i + 1
            if len(hits) > 24:
                break
        print(f"=== {needle!r}: {len(hits)} hit(s)")
        for off, ctx in hits:
            print(f"    {off:#010x}  ...{ctx}...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
