#!/usr/bin/env python3
"""The evidence for ONE input-candidate run, read out of its archive.

WHAT THIS READS, AND WHY EACH THING IS A SEPARATE INSTRUMENT.

  injected   the port's own record of what it pushed into the guest
             ("inj_ax=" / "inj_btn="). It says what the port DID.
  guest      the guest's OWN storage, read back out of the guest image
             ("ax=" / "btn="). It says what the guest HOLDS. A candidate
             that is injected but absent from the guest is a port bug; a
             candidate that is in the guest and does nothing is a game
             binding question, and those are different faults.
  brake      the guest's OWN on-screen message, "hold the brake to
             reverse" (U+6309 U+4F4F U+5239 U+8F66 U+6765 U+5012 U+8F66
             U+3002), drawn only when the car is NOT moving. Its
             appearance is the discriminator the guest
             itself supplies.
  markers    the harness's race markers, so a run that never reached the
             race can be told apart from a run in which the candidate did
             not work.

Usage:  throttle_evidence.py <archive-dir> [<archive-dir> ...]
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

FRAME_RE = re.compile(r"phase=input-door .*?\bf=(\d+)\b")
GUEST_AX_RE = re.compile(r"\bax=\[([^\]]*)\]")
GUEST_BTN_RE = re.compile(r"\bbtn=\[([^\]]*)\]")
PUSH_AX_RE = re.compile(r"inj_ax=\[([^\]]*)\]")
PUSH_BTN_RE = re.compile(r"inj_btn=(\S+)")
TEXT_RE = re.compile(r'phase=glyph-vector .*?\btext="((?:[^"\\]|\\.)*)"')
TRIGGER_RE = re.compile(r"phase=pad-trigger .*?detail=(.*)$")

# The guest's own message: "hold the brake to reverse".
BRAKE_TEXT = "\u6309\u4f4f\u5239\u8f66\u6765\u5012\u8f66\u3002"


def read_archive(directory: Path) -> dict:
    result = {
        "name": directory.name,
        "guest_log": False,
        "frames": [],
        "brake": [],
        "trigger": [],
        "markers": {},
        "summary": {},
    }
    log = directory / "rr3-native-rehost.guest-view.log"
    if not log.exists():
        return result
    result["guest_log"] = True

    frame = 0
    with log.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            match = FRAME_RE.search(line)
            if match:
                frame = int(match.group(1))
                continue
            match = TRIGGER_RE.search(line)
            if match:
                result["trigger"].append((frame, match.group(1).strip()))
                continue
            match = TEXT_RE.search(line)
            if match and BRAKE_TEXT in match.group(1):
                result["brake"].append(frame)
                continue

    # The injected/guest axis and button timeline, compressed to the
    # frames where something CHANGED.
    last = None
    with log.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "phase=input-door" not in line:
                continue
            match = FRAME_RE.search(line)
            if not match:
                continue
            current = int(match.group(1))
            pushed_ax = PUSH_AX_RE.search(line)
            pushed_btn = PUSH_BTN_RE.search(line)
            guest_ax = GUEST_AX_RE.search(line)
            guest_btn = GUEST_BTN_RE.search(line)
            state = (
                pushed_ax.group(1) if pushed_ax else "",
                pushed_btn.group(1) if pushed_btn else "",
                guest_ax.group(1) if guest_ax else "",
                guest_btn.group(1) if guest_btn else "",
            )
            if state != last:
                result["frames"].append((current,) + state)
                last = state

    summary = directory / "run-summary.txt"
    if summary.exists():
        for line in summary.read_text(encoding="utf-8", errors="replace").splitlines():
            stripped = line.strip()
            if stripped.startswith("frames "):
                result["markers"]["frames"] = stripped
            for key in ("start lights", "race screen", "race HUD built",
                        "race objective text", "wall clock", "guest log"):
                if stripped.startswith(key):
                    result["markers"][key] = stripped
    return result


def report(result: dict) -> None:
    print(f"=== {result['name']}")
    if not result["guest_log"]:
        print("    NO GUEST LOG -- the archive is VOID")
        return
    for key, value in result["markers"].items():
        print(f"    {value}")
    print("    injected timeline (frame, pushed axes, pushed buttons, "
          "guest axes, guest buttons):")
    for row in result["frames"][:14]:
        print(f"      f={row[0]:<7} push=[{row[1]}] btn={row[2]:<8} "
              f"guest=[{row[3]}] gbtn=[{row[4]}]")
    if result["brake"]:
        print(f"    BRAKE MESSAGE at frames {result['brake']} "
              f"-> the guest says the car is NOT moving")
    else:
        print("    no brake message -> the car kept moving")
    if result["trigger"]:
        print("    pad-trigger diagnostic (the HID service's own report):")
        for frame, detail in result["trigger"][:4]:
            print(f"      f={frame:<7} {detail}")
    print()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for name in sys.argv[1:]:
        report(read_archive(Path(name)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
