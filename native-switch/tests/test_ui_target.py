#!/usr/bin/env python3
"""The port must find the CONTROL the guest wants pressed, not only the
guest's red glove.

This is a BEHAVIOUR test over REAL archived frames, plus a static contract
for the rules the C++ implements.

WHY THE GLOVE ALONE IS NOT ENOUGH (measured 2026-10-30)
---------------------------------------------------------
On the q25 run the guest sat on the main menu for the whole 1500 seconds.
The port found the guest's red glove every time and pressed it -- 144 times
-- and the guest never moved, for two separate reasons:

  1. the tap was zero-duration (down and up in the same frame), which the
     guest's UI does not accept at all; that is pinned by test_ui_tap.py;
  2. the glove was on the season-promo CAR IMAGE, which is a decoration.
     The actual way into a race was the ORANGE "ENTER THE RACE" button at
     x 1090..1223, y 611..641 -- and the guest does NOT draw a glove on it.

So the port needs the guest's accent-coloured controls as well.

THE RULES UNDER TEST, from native-switch/source/ui_target.cpp:

  * orange band  : r >= 230, 150 <= g <= 215, b <= 60
  * yellow band  : r >= 225, g >= 215,  b <= 90
  * runs of that colour smaller than 16 pixels are dust
  * runs whose boxes are within 8 pixels of each other are ONE control:
    a button's label is a row of glyphs with a few pixels of background
    between them, and the calendar's "COLLECT" word is nothing but glyphs
  * a control has at least 200 accent pixels and a box 60..600 wide and
    16..160 tall
  * the press point is the CENTROID of the accent pixels, not the middle of
    the box: a label's glyphs leave the box's middle empty between the
    characters, and a skewed button's box middle can be off the button.

THE POSITIVE CONTROL: the q25 archive's last frame shows the main menu with
the orange "ENTER THE RACE" button, and the detector must find it at the
position the picture shows.

THE NEGATIVE CONTROLS:
  * a picture with no accent colour at all must yield no control;
  * the press point must land INSIDE the accent pixels, not merely near
    them -- a centroid that falls in the gap between two glyphs is a miss.
"""

from pathlib import Path
import sys

# ---- the rules, copied from native-switch/include/rr3/ui_target.hpp ----
K_ORANGE_RED_MIN = 230
K_ORANGE_GREEN_MIN = 150
K_ORANGE_GREEN_MAX = 215
K_ORANGE_BLUE_MAX = 60
K_YELLOW_RED_MIN = 225
K_YELLOW_GREEN_MIN = 215
K_YELLOW_BLUE_MAX = 90
K_MIN_BUTTON_PIXELS = 200
K_MIN_BUTTON_WIDTH = 60
K_MAX_BUTTON_WIDTH = 600
K_MIN_BUTTON_HEIGHT = 16
K_MAX_BUTTON_HEIGHT = 160
K_MERGE_GAP = 8
K_MIN_RUN_PIXELS = 16


def in_band(r, g, b, band):
    if band == "orange":
        return (r >= K_ORANGE_RED_MIN and K_ORANGE_GREEN_MIN <= g <=
                K_ORANGE_GREEN_MAX and b <= K_ORANGE_BLUE_MAX)
    return (r >= K_YELLOW_RED_MIN and g >= K_YELLOW_GREEN_MIN and
            b <= K_YELLOW_BLUE_MAX)


def collect_runs(px, w, h, band):
    """Connected runs of the accent colour, 8-connected."""
    mask = bytearray(w * h)
    for i in range(w * h):
        r, g, b = px[i]
        if in_band(r, g, b, band):
            mask[i] = 1
    runs = []
    for start in range(w * h):
        if not mask[start]:
            continue
        stack = [start]
        mask[start] = 0
        xs = []
        ys = []
        while stack:
            j = stack.pop()
            jx = j % w
            jy = j // w
            xs.append(jx)
            ys.append(jy)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx = jx + dx
                    ny = jy + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        k = ny * w + nx
                        if mask[k]:
                            mask[k] = 0
                            stack.append(k)
        if len(xs) >= K_MIN_RUN_PIXELS:
            runs.append({
                "x0": min(xs), "x1": max(xs), "y0": min(ys), "y1": max(ys),
                "area": len(xs), "sx": sum(xs), "sy": sum(ys),
            })
    return runs


def merge_runs(runs):
    groups = list(runs)
    merged = True
    while merged:
        merged = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                a = groups[i]
                b = groups[j]
                gap_x = (a["x0"] - b["x1"] - 1) if a["x0"] > b["x1"] else \
                        ((b["x0"] - a["x1"] - 1) if b["x0"] > a["x1"] else 0)
                gap_y = (a["y0"] - b["y1"] - 1) if a["y0"] > b["y1"] else \
                        ((b["y0"] - a["y1"] - 1) if b["y0"] > a["y1"] else 0)
                if gap_x > K_MERGE_GAP or gap_y > K_MERGE_GAP:
                    continue
                groups[i] = {
                    "x0": min(a["x0"], b["x0"]), "x1": max(a["x1"], b["x1"]),
                    "y0": min(a["y0"], b["y0"]), "y1": max(a["y1"], b["y1"]),
                    "area": a["area"] + b["area"],
                    "sx": a["sx"] + b["sx"], "sy": a["sy"] + b["sy"],
                }
                del groups[j]
                merged = True
                break
            if merged:
                break
    return groups


def largest_control(px, w, h, band):
    """The biggest button-sized accent group, or None."""
    groups = merge_runs(collect_runs(px, w, h, band))
    best = None
    for g in groups:
        bw = g["x1"] - g["x0"] + 1
        bh = g["y1"] - g["y0"] + 1
        if g["area"] < K_MIN_BUTTON_PIXELS:
            continue
        if bw < K_MIN_BUTTON_WIDTH or bw > K_MAX_BUTTON_WIDTH:
            continue
        if bh < K_MIN_BUTTON_HEIGHT or bh > K_MAX_BUTTON_HEIGHT:
            continue
        if best is None or g["area"] > best["area"]:
            best = g
    if best is None:
        return None
    return {
        "x": best["sx"] // best["area"],
        "y": best["sy"] // best["area"],
        "area": best["area"],
        "w": best["x1"] - best["x0"] + 1,
        "h": best["y1"] - best["y0"] + 1,
        "bbox": (best["x0"], best["y0"], best["x1"], best["y1"]),
    }


def load_rgb(path):
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return list(im.getdata()), w, h


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    archives = root.parent / ".build" / "eden-runs"

    # ---- the NEGATIVE control: a picture with no accent colour ---------
    blank = [(30, 30, 30)] * (64 * 64)
    assert largest_control(blank, 64, 64, "orange") is None, (
        "a grey picture must yield no control")
    assert largest_control(blank, 64, 64, "yellow") is None, (
        "a grey picture must yield no control")
    print("negative control (no accent colour) : nothing, as required")

    # ---- the NEGATIVE control that matters: the accent pixels are dust --
    dust = [(0, 0, 0)] * (64 * 64)
    for i in range(0, 64 * 64, 37):
        dust[i] = (240, 180, 20)
    assert largest_control(dust, 64, 64, "orange") is None, (
        "scattered dust must not become a control")
    print("negative control (scattered dust)   : nothing, as required")

    # ---- the POSITIVE control: the main menu's ENTER THE RACE button --
    q25 = archives / "20261030-215625-eden-rr3-q25-glove-frame30" / "frames"
    frame = None
    if q25.is_dir():
        # The ordinary capture frames, not the HUD grabs.
        frames = sorted(q25.glob("frame-0*.png"))
        if not frames:
            frames = sorted(q25.glob("frame-*.png"))
        if frames:
            frame = frames[-1]
    if frame is None:
        print("the q25 archive is not here; the positive control cannot run")
    else:
        data = load_rgb(frame)
        assert data is not None, "PIL is not available; cannot run the behaviour test"
        px, w, h = data
        print("positive control frame : %s (%dx%d)" % (frame.name, w, h))
        orange = largest_control(px, w, h, "orange")
        assert orange is not None, (
            "the main menu's orange ENTER THE RACE button was not found")
        print("    orange control      : x=%d y=%d area=%d box=%dx%d bbox=%s"
              % (orange["x"], orange["y"], orange["area"], orange["w"],
                 orange["h"], orange["bbox"]))

        # MEASURED on this very frame (2026-10-30, orange mask
        # r>230, 150<g<215, b<60): the button is x 1090..1223, y 611..641.
        x0, y0, x1, y1 = orange["bbox"]
        assert 1080 <= x0 <= 1100 and 1215 <= x1 <= 1230, (
            "the control's box does not match the measured ENTER THE RACE "
            "button (x 1090..1223): got %s" % (orange["bbox"],))
        assert 605 <= y0 <= 615 and 635 <= y1 <= 648, (
            "the control's box does not match the measured ENTER THE RACE "
            "button (y 611..641): got %s" % (orange["bbox"],))

        # THE PRESS POINT MUST LAND ON THE CONTROL, not in the gap between
        # two glyphs.  This is the check that the centroid rule buys.
        r, g, b = px[orange["y"] * w + orange["x"]]
        assert in_band(r, g, b, "orange"), (
            "the press point (%d,%d) is not on an accent pixel (rgb=%r)"
            % (orange["x"], orange["y"], (r, g, b)))
        print("    press point         : on an accent pixel (rgb=%r)" % ((r, g, b),))

    # ---- the STATIC contract: the C++ implements what this test assumes.
    hdr = (root / "include" / "rr3" / "ui_target.hpp").read_text(encoding="utf-8")
    src = (root / "source" / "ui_target.cpp").read_text(encoding="utf-8")
    probe = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    assert "kOrangeRedMin = 230" in hdr
    assert "kOrangeGreenMin = 150" in hdr
    assert "kOrangeGreenMax = 215" in hdr
    assert "kOrangeBlueMax = 60" in hdr
    assert "kYellowRedMin = 225" in hdr
    assert "kYellowGreenMin = 215" in hdr
    assert "kYellowBlueMax = 90" in hdr
    assert "kMinButtonPixels = 200" in hdr
    assert "kMinButtonWidth = 60" in hdr
    assert "kMaxButtonWidth = 600" in hdr
    assert "kMinButtonHeight = 16" in hdr
    assert "kMaxButtonHeight = 160" in hdr
    assert "kMergeGap = 8" in src
    assert "kMinRunPixels = 16" in src
    # The press point is the centroid, and the row is flipped: glReadPixels
    # returns the BOTTOM row first while the guest's input is top-down.
    assert "blob.sumX / blob.area" in src
    assert "blob.sumY / blob.area" in src
    assert "height - 1 - cy" in src, (
        "the reported y must be flipped: glReadPixels returns the BOTTOM row first")
    # The frame loop must use the new finder.
    assert "rr3::ui::findTargets" in probe, (
        "the frame loop does not call the target finder")
    # THE FEEDBACK LOOP: a press that changes nothing must not be repeated
    # for ever, and the guest's screen is the only feedback there is.
    assert "s_waitingFeedback" in probe, (
        "the frame loop has no feedback loop")
    assert "pressedStillThere" in probe, (
        "the frame loop does not check whether the press changed anything")
    print("static contract                      : the finder and the loop match the rules")


if __name__ == "__main__":
    sys.exit(main())
