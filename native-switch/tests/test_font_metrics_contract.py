#!/usr/bin/env python3
"""The Font's metrics must be the real face's metrics at the real size.

THE BUG
-------
com.firemint.realracing.Font.init() builds a Paint with the typeface at the
size the guest asks for and reads Paint.getFontMetrics() off it
(reverse/java/sources/com/firemint/realracing/Font.java:72-89).  The guest's
own layout reads those numbers back, so the port has to answer with the face's
own numbers.

The port answered a synthetic 48 px set for EVERY font:

    ascent 39   descent -9   height 30   top 40   bottom 11   leading 6

and Font.getSize() answered a constant 48 for every font too.

Two things were wrong with it:

  1. THE SIGNS.  Android's Paint.FontMetrics is documented as ascent and top
     NEGATIVE, descent and bottom POSITIVE, leading POSITIVE, so Font.java
     stores ascent POSITIVE, descent POSITIVE and height = ascent + descent.
     The port's DESCENT was negative, so its height was 39 + (-9) = 30 instead
     of 48: the guest sized every label against a font 38% shorter than the
     one the paragraph was actually laid out with.

  2. THE SIZE.  The guest asked for 140 px and the port rasterised the text at
     140 px, but the guest's own layout was told the font was 48 px.  The text
     therefore came out far too large and collided with the label below it --
     the "字符太大" the user reported.

WHAT THIS PINS
--------------
  1. metricsOf() does NOT negate the FreeType ascender (sm.ascender is
     already positive), so the ascent -- and with it the line height -- is
     positive;
  2. the line height is ascent + descent + leading, which is what Android's
     Layout.getDesiredLineHeight() returns and what the game's StaticLayout
     (spacingmult 1.0, spacingadd 0.0) uses;
  3. the six Font metrics the guest reads back are the MEASURED ones, and the
     synthetic set is only the fallback;
  4. Font.getSize() answers the size the guest asked for;
  5. the CJK ink fits inside the Latin line box, so two lines of Chinese text
     cannot touch.  This one is checked NUMERICALLY against the fonts the port
     actually ships, not against the source.

The numeric part is the important one: it is the criterion the user reported
("两行中文墨迹贴在一起"), computed from the shipped faces.
"""

import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLYPH = ROOT / "source" / "jni_compat" / "classes" / "glyph_vector.cpp"
BRIDGE = ROOT / "source" / "jni_bridge.cpp"
UI = ROOT / "romfs" / "ui"

LATIN_FACE = UI / "ConduitITCPro-Bold.otf"
CJK_FACE = UI / "NotoSansCJKsc-RR3Bootstrap.otf"

# The size the guest asks for for the UI labels.  Every single-line label in
# the q63 run asked for exactly this.
UI_SIZE = 140.0


def body_of(source, signature):
    start = source.find(signature)
    assert start >= 0, f"{signature} is not in the source"
    depth = 0
    seen = False
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
            seen = True
        elif source[i] == "}":
            depth -= 1
            if seen and depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"{signature} has no closing brace")


def face_metrics(path, size):
    """The hhea metrics scaled the way FreeType scales them."""
    from fontTools.ttLib import TTFont

    font = TTFont(str(path), lazy=True)
    try:
        head = font["head"]
        hhea = font["hhea"]
        upem = head.unitsPerEm
        scale = size / upem
        ascender = hhea.ascender * scale
        descender = -hhea.descender * scale
        height = (hhea.ascender - hhea.descender + hhea.lineGap) * scale
        return {
            "upem": upem,
            "ascent": ascender,
            "descent": descender,
            "height": height,
            "leading": height - (ascender + descender),
        }
    finally:
        font.close()


def ink_height(path, text, size):
    """The ink box of a string, in pixels, as the port rasterises it."""
    from fontTools.ttLib import TTFont
    from fontTools.pens.boundsPen import BoundsPen

    font = TTFont(str(path), lazy=True)
    try:
        upem = font["head"].unitsPerEm
        glyphs = font.getGlyphSet()
        cmap = font.getBestCmap()
        low = None
        high = None
        for char in text:
            name = cmap.get(ord(char))
            if name is None:
                continue
            pen = BoundsPen(glyphs)
            glyphs[name].draw(pen)
            if not pen.bounds:
                continue
            low = pen.bounds[1] if low is None else min(low, pen.bounds[1])
            high = pen.bounds[3] if high is None else max(high, pen.bounds[3])
        if low is None:
            return 0.0
        return (high - low) * size / upem
    finally:
        font.close()


def check_numbers():
    """The faces the port ships, measured, and the criterion they must meet."""
    latin = face_metrics(LATIN_FACE, UI_SIZE)
    cjk = face_metrics(CJK_FACE, UI_SIZE)

    # FreeType rounds the ascent and the descent UP (the port's metricsOf
    # does ceilf on both), so these are the numbers the port must report.
    assert math.ceil(latin["ascent"]) == 99, (
        f"Conduit at {UI_SIZE:.0f} px: the ascent should be 99 px, got "
        f"{latin['ascent']:.1f}")
    assert math.ceil(latin["descent"]) == 42, (
        f"Conduit at {UI_SIZE:.0f} px: the descent should be 42 px, got "
        f"{latin['descent']:.1f}")

    # THE CRITERION: the Chinese ink has to fit inside the line box the
    # paragraph lays the lines out with.  The line box comes from the PRIMARY
    # typeface (that is what Paint.getFontMetrics() reports on Android, and the
    # CJK fallback only supplies glyphs), so the CJK ink must fit the Latin
    # line box -- with the leading, and it must fit even without it.
    line_box = math.ceil(latin["ascent"]) + math.ceil(latin["descent"])
    line_box_with_leading = line_box + round(latin["leading"])
    cjk_ink = ink_height(CJK_FACE, "滨海湾市街赛道", UI_SIZE)
    assert cjk_ink <= line_box, (
        f"the CJK ink is {cjk_ink:.1f} px at {UI_SIZE:.0f} px but the line box "
        f"is only {line_box} px, so two lines of Chinese text touch")
    assert cjk_ink <= line_box_with_leading, (
        f"the CJK ink is {cjk_ink:.1f} px but the line box with the leading is "
        f"only {line_box_with_leading} px")
    return latin, cjk, cjk_ink, line_box, line_box_with_leading


def check_source():
    glyph = GLYPH.read_text(encoding="utf-8")
    bridge = BRIDGE.read_text(encoding="utf-8")

    # 1. THE ASCENT IS NOT NEGATED.
    metrics = body_of(glyph, "Metrics metricsOf(")
    assert not re.search(r"-\s*sm\.ascender", metrics), (
        "metricsOf() negates sm.ascender, so the ascent is negative and the "
        "line height with it")
    assert re.search(r"sm\.ascender", metrics), (
        "metricsOf() does not read sm.ascender at all")
    assert re.search(r"-sm\.descender", metrics), (
        "metricsOf() does not read sm.descender (which is negative)")

    # 2. THE LINE HEIGHT IS ascent + descent + leading.
    render = body_of(glyph, "void renderText(")
    assert re.search(
        r"lineHeight\s*=\s*metrics\.ascent\s*\+\s*metrics\.descent\s*\+\s*"
        r"metrics\.leading", render), (
        "the line height is not ascent + descent + leading, which is what "
        "Android's Layout.getDesiredLineHeight() returns")
    # The silent fallback has to stay LOUD.
    assert re.search(r"NO LINE HEIGHT", render), (
        "the no-line-height fallback is silent again")

    # 3. THE MEASURED METRICS REACH THE GUEST.
    fields = body_of(bridge, "float fontFloatField(")
    for name in ("ascent", "descent", "height", "top", "bottom", "leading"):
        assert re.search(rf"font->{name}", fields), (
            f"fontFloatField() does not answer {name} from the measured font")
    assert re.search(r"fontMetricsFor", body_of(bridge, "void measureFontMetrics(")), (
        "measureFontMetrics() does not measure the face")

    # 4. getSize() ANSWERS THE SIZE THE GUEST ASKED FOR.
    call = body_of(bridge, "float jniCallFloatMethod(")
    assert re.search(r"getSize[\s\S]{0,120}return\s+font->size", call), (
        "Font.getSize() does not answer the size the guest asked for")
    assert re.search(r"getWidthScale[\s\S]{0,120}return\s+font->widthScale",
                     call), (
        "Font.getWidthScale() does not answer the scale the guest asked for")

    # 5. THE FIELDS THE GUEST READS ARE VISIBLE IN THE LOG.
    assert "fontNoteFieldRead" in body_of(bridge, "float fontFloatField("), (
        "fontFloatField() does not report which fields the guest reads, so a "
        "field that drives the layout looks the same as one nobody reads")

    # 5. THE FALLBACK IS IN THE GAME'S SIGNS.
    descent = re.search(r"constexpr float kFontDescent\s*=\s*(-?[\d.]+)f;",
                         bridge)
    ascent = re.search(r"constexpr float kFontAscent\s*=\s*(-?[\d.]+)f;",
                        bridge)
    height = re.search(r"constexpr float kFontHeight\s*=\s*(-?[\d.]+)f;",
                        bridge)
    assert descent and ascent and height, (
        "the synthetic font metrics are gone; the fallback must stay")
    assert float(descent.group(1)) > 0, (
        "the synthetic descent is negative; Font.java stores fm.descent, which "
        "is positive")
    assert float(height.group(1)) == float(ascent.group(1)) + float(
        descent.group(1)), (
        "the synthetic height is not ascent + descent, which is what "
        "Font.java:85 assigns")


def main() -> int:
    for path in (GLYPH, BRIDGE, LATIN_FACE, CJK_FACE):
        if not path.exists():
            print(f"SKIP: {path} is not there")
            return 0

    latin, cjk, cjk_ink, line_box, line_box_with_leading = check_numbers()
    check_source()

    print("PASS: the Font answers the real face's metrics at the real size, "
          "in the game's signs (ascent/descent positive, height = ascent + "
          "descent)")
    print("      Conduit at %.0f px: ascent %.0f descent %.0f leading %.0f "
          "line box %.0f (%.0f with the leading)"
          % (UI_SIZE, math.ceil(latin["ascent"]), math.ceil(latin["descent"]),
             latin["leading"], line_box, line_box_with_leading))
    print("      CJK ink of the event-card track name: %.0f px -- it fits the "
          "line box, so two lines cannot touch" % cjk_ink)
    return 0


if __name__ == "__main__":
    sys.exit(main())
