#!/usr/bin/env python3
"""Set the pursuit's configuration on the card.

The port reads two keys out of
  sdmc:/switch/rr3-native-rehost/rr3-screen.txt

    raceline=centre_spline | ai_spline
    lookahead=fixed | adaptive

The port reads that file ONCE, at startup (raceLineConf() caches on first
use), so this has to run BEFORE the round starts.  A round that changes the
file while the guest is running measures the OLD configuration and reports
the NEW one, which is worse than not changing it at all.

Usage:  set-pursuit-conf.py <raceline> <lookahead>
"""
import sys
from pathlib import Path

CARD = Path(r"E:\Eden-rr3\user\sdmc\switch\rr3-native-rehost\rr3-screen.txt")

RACELINES = ("centre_spline", "ai_spline")
LOOKAHEADS = ("fixed", "adaptive")

KEYS = ("raceline", "lookahead")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: set-pursuit-conf.py <raceline> <lookahead>")
        print("  raceline : {0}".format(" | ".join(RACELINES)))
        print("  lookahead: {0}".format(" | ".join(LOOKAHEADS)))
        return 2

    raceline = sys.argv[1].strip()
    lookahead = sys.argv[2].strip()
    if raceline not in RACELINES:
        print("unknown raceline {0!r}; expected one of {1}".format(raceline, RACELINES))
        return 2
    if lookahead not in LOOKAHEADS:
        print("unknown lookahead {0!r}; expected one of {1}".format(lookahead, LOOKAHEADS))
        return 2

    if not CARD.is_file():
        print("no {0}".format(CARD))
        return 1

    # Read as bytes and split on newlines, so the file's line endings and its
    # encoding survive the edit untouched.  The port reads it as bytes.
    raw = CARD.read_bytes()
    text = raw.decode("utf-8", errors="replace")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()

    # Drop the old keys, wherever they are, and remember the comments that
    # explain them so the file keeps documenting itself.
    kept = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("raceline=") or stripped.startswith("lookahead="):
            continue
        kept.append(line)

    # Drop a trailing run of blank lines so the new keys land at the end
    # where a reader expects them.
    while kept and not kept[-1].strip():
        kept.pop()

    kept.append("raceline={0}".format(raceline))
    kept.append("lookahead={0}".format(lookahead))

    out = newline.join(kept) + newline
    CARD.write_bytes(out.encode("utf-8"))

    # Read it back and PROVE it: a configuration that was written and not
    # read back is a configuration nobody has checked.
    check = CARD.read_bytes().decode("utf-8", errors="replace")
    found = {}
    for line in check.splitlines():
        for key in KEYS:
            if line.strip().startswith(key + "="):
                found[key] = line.strip().split("=", 1)[1].strip()
    if found.get("raceline") != raceline or found.get("lookahead") != lookahead:
        print("the file does not read back as asked: {0}".format(found))
        return 1

    print("rr3-screen.txt -> raceline={0} lookahead={1} ({2} bytes)".format(
        raceline, lookahead, CARD.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
