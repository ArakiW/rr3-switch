#!/usr/bin/env python3
"""The port's typeface table, checked against the game's own faces.

The guest asks for a face by NAME and measures the UI text with it. So the
port has to open the same face, and the table that decides which file a
request lands on is a CONTRACT:

  1. EVERY face the guest asks for must be matched BY ITS EXACT NAME.
     The previous table matched by substring, and a substring match is
     exactly the thing that cannot decide a typeface: the needle
     "EurostileLTStd" matches "EurostileLTStd.otf", "EurostileLTStd-Demi.otf"
     and "EurostileLTStd-Ex2.otf", so the table sent ALL of them to
     EurostileLTStd-Bold.otf -- the game's regular weight drawn bold.
  2. r3_symbols.ttf IS THE GAME'S ICON FONT AND IT IS OPENED AS ITSELF.
     MEASURED 2026-11-02 from the guest's own log: the guest asks for it
     EIGHTEEN TIMES PER RUN, and every single one of those draws is ONE
     CHARACTER of icon art -- U+0029 a trophy, U+002C a stack of cash,
     U+0111 the game's own logo.  The port used to answer them with
     ConduitITCPro-Regular, so every icon came out as a LATIN LETTER (a K
     with an accent where the coin bar is, a d with a stroke where the logo
     is) and 2.3x..5.8x too small.  That was the "the UI is missing
     characters" the user reported, and this test pins the fix.
  3. r3_decals.ttf is NOT drawn with.  It is the game's decal font, and the
     guest has not asked for it in any measured run.
  4. The table has no substring-style needle left (the old
     "EurostileLTStd" -> "EurostileLTStd-Bold.otf" mapping is the exact
     defect this test exists to pin).
  5. There are TWO r3_symbols.ttf files and they are NOT the same font: the
     one the guest maps (rr3core:/files/.depot/) is 125,764 bytes and the
     card's loose copy is 72,468 bytes, and they disagree on the glyph at
     several code points.  The build must ship the 125,764-byte one, and
     this test refuses the other.

WHAT IS WRITTEN HERE AND WHY
----------------------------
An earlier version of this file asserted the OPPOSITE of (2): it said the
symbol font must NOT be used, because the guest was believed to lay out a
whole ENGLISH PARAGRAPH with it.  That belief was wrong, and it was wrong
because the log window it was read from was six lines long and ran into the
NEXT request.  The measurement is repeated below, from the shipped fonts:
the icon codepoints the guest really draws are ICONS in this face and
LETTERS in Conduit, which is the whole difference.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
GLYPH = ROOT / "source" / "jni_compat" / "classes" / "glyph_vector.cpp"
BUILD = PROJECT / "tools" / "Build-NativeSwitch.ps1"
STAGE = PROJECT / "tools" / "stage_rr3_fonts.py"
ROMFS_UI = ROOT / "romfs" / "ui"
DEPOT = PROJECT / "analysis" / "rr3-ui-source" / "files" / ".depot"

# The faces the guest asks for, measured from the guest's own log
# (phase=map-app, the .depot font requests). These are the names the port
# has to answer for.
GUEST_REQUESTS = (
    "ConduitITCPro-Regular.otf",
    "ConduitITCPro-Bold.otf",
    "ConduitITCPro-Medium.otf",
    "ConduitITCPro-ExtraBold.otf",
    "EurostileLTStd.otf",
    "EurostileLTStd-Bold.otf",
    "EurostileLTStd-BoldEx2.otf",
    "EurostileLTStd-BoldOblique.otf",
    "EurostileLTStd-Demi.otf",
    "EurostileLTStd-DemiOblique.otf",
    "EurostileLTStd-Ex2.otf",
    "HelveticaNeueBold.ttf",
    "LCD.ttf",
    "minion_pro_italic.otf",
    "myriadp0.otf",
    "r3_symbols.ttf",
)

# The ICON font, and the decal face that is not drawn with.
ICON_FACE = "r3_symbols.ttf"
DECAL_FACE = "r3_decals.ttf"

# THE SIZE OF THE ICON FONT THE GUEST REALLY MAPS.  The card's loose copy
# is 72,468 bytes and is a DIFFERENT font.
ICON_FONT_BYTES = 125764

# The codepoints the guest draws with the icon font, measured from the
# q121 run (archive 20261102-172615-eden-rr3-q121-no-early-gl). Every one
# of them is a ONE-CHARACTER draw.
ICON_CODEPOINTS = (
    0x0031, 0x0030, 0x0038, 0x0039, 0x0056, 0x0068, 0x0069, 0x006A,
    0x007B, 0x007C, 0x007D, 0x00BD, 0x00C7, 0x00C9, 0x00CB, 0x010E,
)


def font_facts(path: Path):
    """upem, ascent in em, and the codepoints the face covers."""
    from fontTools.ttLib import TTFont

    font = TTFont(str(path), lazy=True)
    try:
        upem = font["head"].unitsPerEm
        ascent = font["hhea"].ascender / upem
        cmap = font.getBestCmap()
        return upem, ascent, set(cmap)
    finally:
        font.close()


def ink_of(path: Path, codepoint: int):
    """The ink box of one glyph, in em."""
    from fontTools.ttLib import TTFont
    from fontTools.pens.boundsPen import BoundsPen

    font = TTFont(str(path), lazy=True)
    try:
        upem = font["head"].unitsPerEm
        name = font.getBestCmap().get(codepoint)
        if name is None:
            return None
        pen = BoundsPen(font.getGlyphSet())
        font.getGlyphSet()[name].draw(pen)
        if not pen.bounds:
            return None
        return ((pen.bounds[2] - pen.bounds[0]) / upem,
                (pen.bounds[3] - pen.bounds[1]) / upem)
    finally:
        font.close()


def check_table(source: str):
    table = dict(re.findall(r'\{"([^"]+)",\s*"romfs:/ui/([^"]+)"\}', source))
    assert table, "the port's face table could not be read from the source"

    for name in GUEST_REQUESTS:
        assert name in table, (
            f"the port's table has no entry for {name}, which the guest asks "
            f"for; it would be drawn with the Conduit fallback")
        assert table[name] == name, (
            f"{name} is mapped to {table[name]}, expected the file of the "
            "same name")

    # 2. THE ICON FONT IS OPENED AS ITSELF.
    assert ICON_FACE in table, (
        "r3_symbols.ttf is the game's ICON font and the guest draws eighteen "
        "one-character icons with it per run; without an entry the icons come "
        "out as Latin letters")
    assert table[ICON_FACE] == ICON_FACE, (
        f"the icon font is mapped to {table.get(ICON_FACE)}")

    # 3. THE DECAL FONT IS NOT DRAWN WITH.
    assert DECAL_FACE not in table, (
        f"{DECAL_FACE} is the game's decal font and the guest has not asked "
        "for it")
    assert f'"{DECAL_FACE}"' in source, (
        f"{DECAL_FACE} is not even listed as a non-text face, so the port does "
        "not know it is art")

    # 4. NO SUBSTRING NEEDLE IS LEFT.
    old_style = re.findall(r'\{"([A-Za-z]+)(?:-Bold)?",\s*"romfs:/ui/', source)
    assert not old_style, (
        f"the table still has substring-style entries: {old_style}; every "
        "entry must name the file exactly")
    return table


def check_build():
    build = BUILD.read_text(encoding="utf-8")
    for name in GUEST_REQUESTS:
        assert f"'{name}'" in build or name == ICON_FACE, (
            f"the build does not copy {name} into the romfs")
    assert "ConduitITCPro-Regular.otf" in build and "ConduitITCPro-Bold.otf" in build, (
        "the two Conduit faces are the port's universal fallback and must "
        "stay REQUIRED")
    # THE ICON FONT IS STAGED, AND THE WRONG COPY IS REFUSED.
    assert ICON_FACE in build, (
        "the build does not stage the icon font, so the icons stay letters")
    assert str(ICON_FONT_BYTES) in build, (
        "the build does not check the icon font's size, so the card's other "
        "72,468-byte file could be shipped and the icons would be the wrong "
        "art")
    stage = STAGE.read_text(encoding="utf-8")
    assert "files/.depot/" in stage, (
        "the staging tool does not read the game's depot")


def check_shipped_faces():
    """The faces in the romfs, measured."""
    icon = ROMFS_UI / ICON_FACE
    if not icon.is_file():
        print("SKIP: the icon font is not staged in the romfs, so only the "
              "source contract was checked")
        return 0

    size = icon.stat().st_size
    assert size == ICON_FONT_BYTES, (
        f"the romfs carries a {size}-byte {ICON_FACE}; the copy the guest "
        f"maps is {ICON_FONT_BYTES} bytes and the other one is a DIFFERENT "
        "font")

    upem, ascent, cmap = font_facts(icon)
    assert upem == 2048, f"the icon font's em is {upem}, expected 2048"
    assert abs(ascent - 2.748) < 0.01, (
        f"the icon font's ascent is {ascent:.3f} em, expected 2.748 em; a "
        "text face ascends 0.75..1.0 em")

    missing = [cp for cp in ICON_CODEPOINTS if cp not in cmap]
    assert not missing, (
        "the icon font has no glyph for the code points the guest draws: "
        + ", ".join("U+%04X" % cp for cp in missing))

    # THE DIFFERENCE THAT MATTERS: in the icon font these code points are
    # PICTURES, and in the text face they are LETTERS.  The ink box is what
    # tells them apart, and it is what the fix is worth: the icon is
    # 1.0..6.2 em wide where the letter was 0.2..0.5 em.
    conduit = ROMFS_UI / "ConduitITCPro-Regular.otf"
    if conduit.is_file():
        wider = 0
        for cp in ICON_CODEPOINTS:
            art = ink_of(icon, cp)
            letter = ink_of(conduit, cp)
            if art is None or letter is None:
                continue
            if art[0] > letter[0]:
                wider += 1
        assert wider >= len(ICON_CODEPOINTS) - 2, (
            f"only {wider} of {len(ICON_CODEPOINTS)} icon code points are "
            "wider in the icon font than in Conduit; the fix would not change "
            "anything on screen")
    return len(ICON_CODEPOINTS)


def main() -> int:
    source = GLYPH.read_text(encoding="utf-8")
    check_table(source)
    check_build()
    pinned = check_shipped_faces()

    print(f"PASS: the port's face table matches {len(GUEST_REQUESTS)} guest "
          f"requests by exact name, the icon font ({ICON_FACE}) is opened as "
          "itself, and the decal font is excluded")
    if pinned:
        print(f"      {pinned} icon code points measured in the shipped "
              "faces: art in the icon font, letters in Conduit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
