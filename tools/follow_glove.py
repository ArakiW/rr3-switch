#!/usr/bin/env python3
"""Click where the guest's red glove cursor is.

The touch script (`rr3-touch.txt`) is a list of PIXEL coordinates measured on
one particular layout.  That makes it fragile in two ways, and both have
cost a round:

* the layout.  At 2048x1152 the guest lays the front end out differently,
  so the same pixels land on different widgets.
* the TIMING.  The hops are anchored to frame numbers, so if the guest
  loads slower than the round that measured them, every hop lands on the
  wrong widget and the guest never reaches the race.  Measured 2026-10-29:
  q12 spent its whole round in the front end because of this, while q10
  and q11 with the same script did reach the race.

THE GUEST SOLVES BOTH PROBLEMS ITSELF.  It draws a red glove cursor on the
control it wants the player to press, and it only draws it when it is
waiting for that press.  So the position of the glove IS the answer to
"where do I press", at any view size and at any moment.

This tool therefore does what a player does: it looks at the screen, finds
the glove, and presses there.  It needs no change to the port, because the
touch control file is APPEND-ONLY and the port injects every new line
exactly once (see native-switch/include/rr3/guest_touch.hpp).

HOW THE GLOVE IS FOUND

A colour mask alone cannot separate the glove from the red car behind it --
the tool's own earlier notes say so.  What separates them is that the glove
ANIMATES: it is the thing that MOVES between two frames of the same screen.
So the tool differences the two newest captures and looks at the changed
pixels.  When the change is small (the glove alone moved) the red cluster
inside it is the glove.  When the change is large (the whole car is
animating, or a screen transition) there is nothing to conclude and the
tool waits for the next pair.

The captures are the port's own framebuffer PNGs, written into the card's
app directory, and they are the PRESENTED screen (1280x720), which is the
same coordinate space the touch control file expects (view pixels).

THE FRAME NUMBER

`at <frame>` waits for the PORT's frame counter.  The port logs that same
counter in its `phase=race-state` lines (guest_input.cpp pumps the race
state probe with g_frameCounter), so the tool reads the last one out of the
guest log.

USAGE

    # follow the glove during a running round
    python tools/follow_glove.py --seconds 900

    # positive control: run the detector over archived frames
    python tools/follow_glove.py --selftest .build/eden-runs/<tag>/frames
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import time

# The glove's own colours: a saturated red with white stripes next to it.
GLOVE_RED_MIN = 140
GLOVE_RED_SPREAD = 70
# A change bigger than this is a screen transition or an animating car, not
# the glove.
MAX_CHANGED_PIXELS = 30000
# The red cluster has to be at least this big to be the glove and not noise.
MIN_RED_PIXELS = 150

TOUCH_PATH = r"E:\Eden\user\sdmc\switch\rr3-native-rehost\rr3-touch.txt"
GUEST_LOG = r"E:\Eden\user\sdmc\switch\rr3-native-rehost.guest-view.log"
APP_DIR = r"E:\Eden\user\sdmc\switch\rr3-native-rehost"


def find_glove(path_a: str, path_b: str):
    """Return (x, y) of the glove between two frames, or None."""
    try:
        import numpy as np
        from PIL import Image
    except ImportError:  # pragma: no cover - the tool needs both
        print("this tool needs numpy and PIL", file=sys.stderr)
        return None

    a = np.asarray(Image.open(path_a).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(path_b).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return None
    changed = np.abs(a - b).sum(axis=2) > 26
    count = int(changed.sum())
    if count < 200 or count > MAX_CHANGED_PIXELS:
        # nothing moved, or far too much moved to be the glove
        return None
    ys, xs = np.nonzero(changed)
    sub = a[changed]
    red = (
        (sub[:, 0] > GLOVE_RED_MIN)
        & (sub[:, 1] < 90)
        & (sub[:, 2] < 90)
        & ((sub[:, 0] - np.maximum(sub[:, 1], sub[:, 2])) > GLOVE_RED_SPREAD)
    )
    if int(red.sum()) < MIN_RED_PIXELS:
        return None
    rx, ry = xs[red], ys[red]
    return ((int(rx.min()) + int(rx.max())) // 2, (int(ry.min()) + int(ry.max())) // 2)


def port_frame_counter(log_path: str) -> int:
    """The port's frame counter, read out of the guest log's tail."""
    try:
        size = os.path.getsize(log_path)
    except OSError:
        return 0
    with open(log_path, "rb") as handle:
        handle.seek(max(0, size - 4 * 1024 * 1024))
        tail = handle.read().decode("utf-8", "replace")
    best = 0
    for match in re.finditer(r"phase=race-state \S+ target=\S+ f=(\d+)", tail):
        best = max(best, int(match.group(1)))
    return best


def append_touch(path: str, frame: int, x: int, y: int) -> None:
    """Append one press/release pair. The file is append-only by contract."""
    lines = []
    if os.path.exists(path) and os.path.getsize(path) > 0:
        with open(path, "rb") as handle:
            handle.seek(-1, os.SEEK_END)
            if handle.read(1) not in (b"\n", b"\r"):
                lines.append("")          # do not glue onto the last line
    lines.append("at %d %d %d down" % (frame + 20, x, y))
    lines.append("at %d %d %d up" % (frame + 60, x, y))
    with open(path, "a", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


def selftest(frames_dir: str) -> int:
    """The positive control: the detector must find the glove in real frames."""
    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")))
    if not frames:
        print("no frames in", frames_dir)
        return 1
    found = 0
    pairs = 0
    for previous, current in zip(frames, frames[1:]):
        pairs += 1
        spot = find_glove(previous, current)
        if spot is not None:
            found += 1
            print("  %-22s -> glove at (%d, %d)" % (os.path.basename(current), spot[0], spot[1]))
    print("selftest: %d of %d consecutive frame pairs carried a glove" % (found, pairs))
    return 0 if found else 1


def follow(seconds: int, app_dir: str, touch_path: str, log_path: str) -> int:
    print("watching %s for the glove, clicking into %s" % (app_dir, touch_path))
    seen: tuple[str, str] | None = None
    clicks = 0
    last_spot: tuple[int, int] | None = None
    last_click = 0.0
    deadline = time.time() + seconds
    while time.time() < deadline:
        frames = sorted(
            glob.glob(os.path.join(app_dir, "frame-*.png")), key=os.path.getmtime
        )
        spot = None
        if len(frames) >= 2:
            pair = (frames[-2], frames[-1])
            if pair != seen:
                seen = pair
                spot = find_glove(*pair)
                if spot is not None:
                    last_spot = spot
        # THE GLOVE DOES NOT ALWAYS MOVE.  The guest draws it on the control
        # it wants pressed and then WAITS, and on a screen that does not
        # change -- a car-select grid, a modal -- the difference between two
        # captures is empty, so the difference-based detector finds nothing
        # and the guest waits forever.  MEASURED 2026-10-29 (q17): the
        # watcher clicked the glove in the garage 80 times, the screen then
        # stopped changing, and the run ended with the guest still waiting
        # for a press it had already been asked for.
        #
        # So the LAST KNOWN glove position is kept and pressed again while
        # the screen stays still: the guest is still showing the glove, which
        # means it is still asking for that press.
        now = time.time()
        if spot is not None:
            frame = port_frame_counter(log_path)
            append_touch(touch_path, frame, spot[0], spot[1])
            clicks += 1
            last_click = now
            print("  glove at (%d, %d) at frame %d -> clicked" % (spot[0], spot[1], frame))
        elif last_spot is not None and (now - last_click) >= 3.0:
            frame = port_frame_counter(log_path)
            append_touch(touch_path, frame, last_spot[0], last_spot[1])
            clicks += 1
            last_click = now
            print("  screen still -> re-clicking the glove at (%d, %d) at frame %d"
                  % (last_spot[0], last_spot[1], frame))
        time.sleep(4.0)
    print("followed for %d s, %d click(s) appended" % (seconds, clicks))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--selftest", metavar="FRAMES_DIR",
                        help="run the detector over archived frames and exit")
    parser.add_argument("--seconds", type=int, default=900,
                        help="how long to follow the glove")
    parser.add_argument("--app-dir", default=APP_DIR)
    parser.add_argument("--touch", default=TOUCH_PATH)
    parser.add_argument("--guest-log", default=GUEST_LOG)
    # The Eden installation whose card the watcher reads and writes.  It
    # exists because the PRIVATE instance (tools\Setup-IsolatedEden.ps1)
    # has its own card, and the defaults above point at the shared one --
    # which is what the other project's stress runs use as well.  Pointing
    # the watcher at the shared card while the emulator runs on the private
    # one means the watcher follows frames that nobody is reading and
    # writes clicks into a file the port never opens.
    parser.add_argument("--emulator-root", default="",
                        help="Eden installation root; overrides the three paths above")
    args = parser.parse_args()
    if args.emulator_root:
        root = args.emulator_root.rstrip("\/")
        app = root + r"\user\sdmc\switch\rr3-native-rehost"
        args.app_dir = app
        args.touch = app + r"\rr3-touch.txt"
        args.guest_log = root + r"\user\sdmc\switch\rr3-native-rehost.guest-view.log"
    if args.selftest:
        return selftest(args.selftest)
    return follow(args.seconds, args.app_dir, args.touch, args.guest_log)


if __name__ == "__main__":
    raise SystemExit(main())
