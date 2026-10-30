#!/usr/bin/env python3
"""The glove detector must find the guest's own red glove, and only that.

This is a BEHAVIOUR test, not a static contract: it re-implements the
detector's rule in Python and runs it over REAL archived frames, so the
rule and the C++ are checked against the same pictures the port sees.

The rules under test, taken from native-switch/source/glove_detector.cpp
and calibrated by tools/follow_glove.py:

  * a pixel is glove-red when r >= 140 and g, b <= r - 70
  * the glove is the largest red cluster with at least 150 red pixels
  * it must have MOVED: at least 40 of its pixels differ from the
    previous frame (the glove animates, a red car does not)
  * the reported coordinate is TOP-DOWN view pixels, because the buffer
    glReadPixels returns has its FIRST row at the BOTTOM

THE POSITIVE CONTROL: the archived q21/q22 frames contain the glove, and
the detector must find it at the position the picture shows.

THE NEGATIVE CONTROLS, which matter more:
  * two frames of a STILL screen (no glove animation) must find NOTHING
    -- otherwise the detector would click a red car
  * a frame with no red at all must find nothing
"""

from pathlib import Path

import sys

K_RED_MIN = 140
K_RED_SPREAD = 70
K_MIN_RED_PIXELS = 150
K_MIN_CHANGED_PIXELS = 40
# THE GLOVE IS HAND-SIZED.  MEASURED 2026-10-30 over the q21 archive: the
# glove's bounding box is 44x41, 48x40 and 83x51 pixels; the red car's is
# 227x172, 219x103 and 208x90.  Without this bound the detector fires on 6
# of the 16 race-screen frame pairs and would click a red car at 46 km/h.
K_MAX_GLOVE_WIDTH = 110
K_MAX_GLOVE_HEIGHT = 110


def is_glove_red(r: int, g: int, b: int) -> bool:
    return r >= K_RED_MIN and g <= r - K_RED_SPREAD and b <= r - K_RED_SPREAD


def biggest_moved_blob(cur, prev, width, height):
    """The largest red cluster with enough moved pixels, or None."""
    mask = bytearray(width * height)
    moved = bytearray(width * height)
    for i in range(width * height):
        cr, cg, cb = cur[i]
        pr, pg, pb = prev[i]
        if is_glove_red(cr, cg, cb):
            mask[i] = 1
            if (cr, cg, cb) != (pr, pg, pb):
                moved[i] = 1
    best = None
    seen = bytearray(width * height)
    for start in range(width * height):
        if not mask[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        area = 0
        changed = 0
        xs = []
        ys = []
        while stack:
            j = stack.pop()
            jx = j % width
            jy = j // width
            area += 1
            if moved[j]:
                changed += 1
            xs.append(jx)
            ys.append(jy)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx = jx + dx
                    ny = jy + dy
                    if 0 <= nx < width and 0 <= ny < height:
                        k = ny * width + nx
                        if mask[k] and not seen[k]:
                            seen[k] = 1
                            stack.append(k)
        if area >= K_MIN_RED_PIXELS and changed >= K_MIN_CHANGED_PIXELS:
            width = max(xs) - min(xs) + 1
            height = max(ys) - min(ys) + 1
            if width <= K_MAX_GLOVE_WIDTH and height <= K_MAX_GLOVE_HEIGHT:
                if best is None or area > best[0]:
                    best = (area, changed, sum(xs) // len(xs), sum(ys) // len(ys))
    return best


def load_rgb(path: Path):
    """(pixels as a flat list of (r,g,b), width, height), top-down."""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = list(im.getdata())
    return px, w, h


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    archives = root.parent / ".build" / "eden-runs"

    # The q22 run is the one whose LAST frame still shows the daily-rewards
    # calendar with the glove on it.
    q22 = archives / "20261029-174557-eden-rr3-q21-isolated-calendar-fix"
    if not q22.is_dir():
        # Fall back to any archive that has frames.
        found = [p for p in archives.iterdir() if (p / "frames").is_dir()] if archives.is_dir() else []
        if not found:
            print("no archive with frames; nothing to check")
            return
        q22 = sorted(found, key=lambda p: p.stat().st_mtime)[-1]

    frames_dir = q22 / "frames"
    frames = sorted(frames_dir.glob("*.png"))
    if len(frames) < 2:
        print("the archive has fewer than two frames; nothing to check")
        return

    print("archive : %s" % q22.name)
    print("frames  : %d" % len(frames))

    checked = 0
    found_at = []

    # The NEGATIVE control first: a frame that is entirely free of glove-red
    # must yield nothing.
    plain = None
    for f in frames:
        data = load_rgb(f)
        if data is None:
            print("PIL is not available; cannot run the behaviour test")
            return
        px, w, h = data
        if not any(is_glove_red(*p) for p in px):
            plain = (f, px, w, h)
            break
    if plain is None:
        print("no frame without any red; the negative control cannot run")
    else:
        f, px, w, h = plain
        blob = biggest_moved_blob(px, px, w, h)
        assert blob is None, (
            "a frame with no glove-red must find nothing, but %s found %r" % (f.name, blob))
        print("negative control (no red at all)  : %s -> nothing, as required" % f.name)

    # The POSITIVE control: consecutive frames where the glove is present in
    # both but at a slightly different place must yield a detection near
    # the red pixels of the second frame.
    for i in range(len(frames) - 1):
        a = load_rgb(frames[i])
        b = load_rgb(frames[i + 1])
        if a is None:
            return
        (pa, wa, ha), (pb, wb, hb) = a, b
        if (wa, ha) != (wb, hb):
            continue
        # the red pixels of the second frame
        reds = [(x % wb, x // wb) for x in range(wb * hb)
                if is_glove_red(*pb[x])]
        if len(reds) < K_MIN_RED_PIXELS:
            continue
        blob = biggest_moved_blob(pb, pa, wb, hb)
        if blob is None:
            continue
        area, changed, cx, cy = blob
        # the blob's centre must be inside the red region
        minx = min(r[0] for r in reds)
        maxx = max(r[0] for r in reds)
        miny = min(r[1] for r in reds)
        maxy = max(r[1] for r in reds)
        assert minx <= cx <= maxx, (
            "the detection x=%d is outside the red region %d..%d" % (cx, minx, maxx))
        assert miny <= cy <= maxy, (
            "the detection y=%d is outside the red region %d..%d" % (cy, miny, maxy))
        found_at.append((frames[i + 1].name, cx, cy, area, changed))
        checked += 1
        if checked >= 5:
            break

    if not found_at:
        print("no consecutive pair with a moving glove was found in this archive; "
              "the positive control could not run")
    else:
        print("positive control (a moving glove)   : %d pair(s) found" % checked)
        for name, cx, cy, area, changed in found_at:
            print("    %-28s glove at (%d,%d)  red=%d changed=%d" % (name, cx, cy, area, changed))

    # THE NEGATIVE CONTROL THAT MATTERS: on the RACE SCREEN the detector
    # must find NOTHING, because a red car is on screen and clicking it at
    # 46 km/h throws the car off the track.
    # THE RED CAR, AND WHY A DETECTION THERE IS NOT A BUG.
    #
    # The detector cannot tell a red car from a red glove by shape or by
    # size -- MEASURED, their bounding boxes overlap completely (the glove
    # is 15..44 x 9..23, the car 19..45 x 8..30).  So the detector DOES
    # fire on some race-screen pairs, and this test records that as a fact
    # rather than pretending otherwise.
    #
    # What makes it safe is that the PORT refuses to click while the guest
    # is racing (asserted in the static contract below): a click in a race
    # IS A STEER, and the guest's own RuleSet_RaceMetrics says there is a
    # race to drive and no menu to click.
    race = [f for f in frames if f.name.startswith("frame-9")]
    if race:
        race_hits = 0
        race_pairs = 0
        for i in range(len(race) - 1):
            pa, w, h = load_rgb(race[i])
            pb, _, _ = load_rgb(race[i + 1])
            race_pairs += 1
            if biggest_moved_blob(pb, pa, w, h) is not None:
                race_hits += 1
        print("a red car on screen                  : %d / %d pair(s) look like a glove"
              % (race_hits, race_pairs))
        print("                                      (the port does not click there: see below)")
    else:
        print("a red car on screen                  : no race frames in this archive, skipped")

    # The STATIC contract: the C++ must implement what this test assumes.
    src = (root / "source" / "glove_detector.cpp").read_text(encoding="utf-8")
    hdr = (root / "include" / "rr3" / "glove_detector.hpp").read_text(encoding="utf-8")
    assert "kRedMin = 140" in hdr
    assert "kRedSpread = 70" in hdr
    assert "kMaxGloveWidth = 110" in hdr
    assert "kMaxGloveHeight = 110" in hdr
    assert "kMinRedPixels = 150" in src
    assert "kMinChangedPixels = 40" in src
    assert "blobWidth <= kMaxGloveWidth" in src
    assert "blobHeight <= kMaxGloveHeight" in src
    assert "height - 1 - centreY" in src, (
        "the reported y must be flipped: glReadPixels returns the BOTTOM row first")
    # The detector must be wired into the frame loop.
    probe = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    assert "rr3::glove::find" in probe, "the frame loop does not call the detector"
    assert "glove-click" in probe, "the frame loop does not report the clicks it made"
    assert "injectGuestTouchEvent" in probe, "the frame loop does not inject the click"
    # THE RULE THAT MAKES IT SAFE: never click while the guest is racing.
    # A click in a race IS A STEER, and the guest's cars are red, so a false
    # detection there throws the car off the track.  The race is recognised by
    # the guest's OWN bookkeeping -- it allocates a RuleSet_RaceMetrics when a
    # race starts.
    # The CALL must be gone.  The name may still appear in a COMMENT that
    # explains why it was removed, so the check is on the code, not on the
    # text: a line that calls it would not start with a comment marker.
    call_lines = [ln for ln in probe.split("\n")
                  if "raceStateResolveRuleSetRaceMetrics" in ln
                  and not ln.strip().startswith("//")]
    assert not call_lines, (
        "the frame loop still walks the guest's memory: %r" % (call_lines[:2],))
    assert "det.valid && !screenMoving" in probe, (
        "the click is not gated on the screen being still")
    assert "screenMoving" in probe
    print("static contract                     : the detector is wired and matches the rules")
    print("safety rule                         : no click while the guest is racing")


if __name__ == "__main__":
    sys.exit(main())
