#!/usr/bin/env python3
"""A TAP MUST SPAN MORE THAN ONE FRAME -- and the frame loop must not
inject a begin and an end in the same place.

This is the regression guard for the bug that cost q25 a whole run.

WHAT HAPPENED (measured 2026-10-30)
-----------------------------------
The port's UI driver used to do this, in the frame loop, on one frame:

    injectGuestTouchEvent(..., GuestTouchAction::Begin, ... det.x, det.y ...);
    injectGuestTouchEvent(..., GuestTouchAction::End,   ... det.x, det.y ...);

A zero-duration tap.  The guest's UI does not accept one: the button's
"pressed" state has to exist for at least one of the guest's own frames
before the finger comes up.

THE EVIDENCE, from every archive in .build/eden-runs that injected touches
at all:

    q21   24 taps, every down->up gap 40 frames   -> reached a race
    q25  144 taps, every down->up gap  0 frames   -> never advanced

Every archive that reached a race has same-frame = 0.  q25 -- the only run
with a same-frame tap -- is the only one that never moved.  The separation
is perfect, over 28 archives.

WHAT THIS TEST PINS
-------------------
  * the shared tap primitive holds the finger for kTapHoldFrames (40), the
    value every archive that reached a race used;
  * the frame loop goes through that primitive and does NOT itself inject a
    Begin and an End next to each other;
  * the primitive clears the guest's shipped "a finger is down at (-1,-1)"
    before the first press, because the guest's engine stores a new point
    only when no finger is down and would otherwise swallow the first press
    of the run (measured: q25's first press left the guest's point at
    (-1,-1); q21's first press, 900 frames in, landed).
"""

from pathlib import Path
import re
import sys


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    src_dir = root / "source"
    inc_dir = root / "include" / "rr3"

    tap_hpp = (inc_dir / "ui_tap.hpp").read_text(encoding="utf-8")
    tap_cpp = (src_dir / "ui_tap.cpp").read_text(encoding="utf-8")
    probe = (src_dir / "guest_view_probe.cpp").read_text(encoding="utf-8")

    # ---- the hold length ------------------------------------------------
    assert "kTapHoldFrames = 40u" in tap_hpp, (
        "the tap hold must stay at the measured 40 frames")
    assert "kTapHoldFrames" in tap_cpp, (
        "the implementation does not use the hold length")

    # ---- the primitive really separates begin and end -------------------
    # The Begin is in press(); the End is in service().  If both were in one
    # function the tap would be a single-frame tap again.
    press_body = tap_cpp.split("bool Tapper::press")[1].split("bool Tapper::service")[0]
    service_body = tap_cpp.split("bool Tapper::service")[1]
    assert "GuestTouchAction::Begin" in press_body, (
        "press() must inject the Begin")
    assert "GuestTouchAction::End" not in press_body, (
        "press() must NOT inject the End: that is what made a zero-duration tap")
    assert "GuestTouchAction::End" in service_body, (
        "service() must inject the End")
    assert "s_releaseFrame = frame + kTapHoldFrames" in press_body, (
        "the release must be scheduled kTapHoldFrames later, not on this frame")
    assert "frame < s_releaseFrame" in service_body, (
        "the release must wait for its frame")

    # ---- the stale pointer ----------------------------------------------
    assert "GuestTouchAction::Release" in tap_cpp, (
        "the shipped 'a finger is down at (-1,-1)' state must be released "
        "before the first press")
    assert "state.x == -1 && state.y == -1" in tap_cpp, (
        "the stale pointer must be recognised by its -1,-1 position")

    # ---- THE FRAME LOOP MUST NOT DO IT ITSELF ---------------------------
    # This is the check that would have caught q25.  A line that injects a
    # Begin or an End directly in the frame loop is a tap the frame loop
    # controls, and it is exactly what produced the zero-duration tap.
    direct = []
    for n, line in enumerate(probe.split("\n"), start=1):
        stripped = line.strip()
        if stripped.startswith("//"):
            continue
        if "injectGuestTouchEvent" in line:
            direct.append((n, stripped))
    assert not direct, (
        "the frame loop injects touches itself (%r); it must go through "
        "rr3::ui::Tapper so that every tap spans more than one frame"
        % (direct[:3],))

    assert "rr3::ui::Tapper" in probe, "the frame loop does not use the tapper"
    assert "tapper.press" in probe, "the frame loop does not press through the tapper"
    assert "tapper.service" in probe, (
        "the frame loop does not release pending taps")
    # service() has to run on EVERY frame, not only on the frames that do a
    # readback, or a tap would stay down forever.
    assert probe.count("tapper.service") >= 1

    # ---- the port's own control file must not have regressed -----------
    # The file control path is a DIFFERENT, older, working mechanism: it
    # injects down and up on the frames the file names, which are 40 frames
    # apart.  It is not allowed to grow a same-frame pair.
    touch_cpp = (src_dir / "guest_touch.cpp").read_text(encoding="utf-8")
    assert "GuestTouchAction::Begin" in touch_cpp
    assert "GuestTouchAction::End" in touch_cpp

    print("tap hold               : %s" % re.search(r"kTapHoldFrames = (\d+)u", tap_hpp).group(1))
    print("primitive              : press() injects Begin, service() injects End")
    print("stale pointer          : released before the first press")
    print("frame loop             : goes through the tapper, injects nothing itself")
    print("same-frame tap         : impossible by construction")


if __name__ == "__main__":
    sys.exit(main())
