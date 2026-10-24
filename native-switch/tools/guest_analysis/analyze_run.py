#!/usr/bin/env python3
"""Analyse one archived run: profile, race start, throttle response, motion.

Everything here comes out of the guest's OWN outputs:

  * the profile line is the guest's own log ("Cannot find description for
    device: ... using default: [...]").
  * the race start is the frame at which the guest opens StartLightsScreen.gui.
  * the throttle response is the frame at which the guest draws its own
    on-screen toast "hold the brake to reverse", which it only draws when the
    player's car has come to a stop. A car that never accelerates stops at
    once, so the toast arrives at the first frame of the race; a car that
    accelerates keeps going and the toast arrives much later, or never.
  * the motion numbers come from the framebuffer (the ground region either
    side of the car, which the camera slides past as the car travels).

Usage: analyze_run.py <archive-dir>
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOAST = "\u6309\u4f4f\u5239\u8f66\u6765\u5012\u8f66"  # hold the brake to reverse


def main() -> int:
    folder = sys.argv[1]
    log = os.path.join(folder, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest log in", folder)
        return 1

    frame_re = re.compile(r"phase=input-door .*?\bf=(\d+)\b")
    text_re = re.compile(r'phase=glyph-vector .*?\btext="((?:[^"\\]|\\.)*)"')

    frame = 0
    lights = None
    ingame = None
    toasts = []
    profile = None
    last_door = None
    with open(log, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            m = frame_re.search(line)
            if m:
                frame = int(m.group(1))
                last_door = line.strip()
                continue
            if "Cannot find description" in line and profile is None:
                i = line.find("Cannot find description")
                profile = line[i:].strip()
            if "StartLightsScreen.gui" in line and lights is None:
                lights = frame
            if "InGameScreen.gui" in line and ingame is None:
                ingame = frame
            if TOAST in line:
                toasts.append((frame, lineno))

    print("=== guest-side ===")
    print("profile        :", profile)
    print("start lights   : frame", lights)
    print("in-game screen : frame", ingame)
    print("'hold brake to reverse' toast at frames:",
          [t[0] for t in toasts] or "NONE")
    if last_door:
        i = last_door.find(" ax=[")
        print("last input-door:", last_door[i:i + 150] if i >= 0 else last_door[:200])

    frames_dir = os.path.join(folder, "frames")
    if os.path.isdir(frames_dir):
        print("\n=== framebuffer motion ===")
        out = subprocess.run(
            [sys.executable, os.path.join(HERE, "motion.py"), frames_dir],
            capture_output=True, text=True)
        print(out.stdout.strip() or out.stderr.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
