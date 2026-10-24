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
  2. The two SYMBOL fonts must NOT be used for paragraphs. Measured from
     the game's own files: upem 2048, ascent 2.75 em, and r3_symbols.ttf's
     '0' advances 2.38 em with an ink box of 2.29 x 2.28 em. Those are the
     placeholder boxes the game draws its currency art into, not letters.
  3. LCD.ttf is NOT a symbol font, even though its upem is 2048 too: its
     digit advances 0.71 em with an ink box 0.62 x 1.00 em, so it is the
     game's display face for the HUD's digits and it is used as it is.

What is checked:
  1. The port's table has an EXACT-NAME entry for every face the guest
     asks for, and no entry maps one name to another file.
  2. The two symbol fonts are in the NOT-TEXT list and are NOT in the
     table of faces the port draws text with.
  3. The table has no substring-style needle left (the old
     "EurostileLTStd" -> "EurostileLTStd-Bold.otf" mapping is the exact
     defect this test exists to pin).
  4. When the game's faces are staged, the romfs carries them.
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
)

# The two faces that are art, not text.
SYMBOL_FACES = ("r3_symbols.ttf", "r3_decals.ttf")


def main() -> int:
    source = GLYPH.read_text(encoding="utf-8")

    # 1. THE TABLE, parsed out of the source.
    table = dict(re.findall(r'\{"([^"]+)",\s*"romfs:/ui/([^"]+)"\}', source))
    assert table, "the port's face table could not be read from the source"

    for name in GUEST_REQUESTS:
        assert name in table, (
            f"the port's table has no entry for {name}, which the guest asks "
            f"for; it would be drawn with the Conduit fallback")
        assert table[name] == name, (
            f"{name} is mapped to {table[name]}, expected the file of the "
            "same name")

    # 2. THE SYMBOL FACES ARE NOT DRAWN WITH.
    for name in SYMBOL_FACES:
        assert name not in table, (
            f"{name} is a SYMBOL font (upem 2048, ascent 2.75 em) and must not "
            "be used to draw paragraphs")
        assert f'"{name}"' in source, (
            f"{name} is not even listed as a non-text face, so the port does "
            "not know it is art")

    # 3. NO SUBSTRING NEEDLE IS LEFT. The old table matched on a needle
    #    that was a PREFIX of several file names, which is the defect.
    old_style = re.findall(r'\{"([A-Za-z]+)(?:-Bold)?",\s*"romfs:/ui/', source)
    assert not old_style, (
        f"the table still has substring-style entries: {old_style}; every "
        "entry must name the file exactly")

    # 4. THE BUILD COPIES THE GAME'S FACES, AND THE STAGING TOOL KNOWS
    #    ABOUT THEM.
    build = BUILD.read_text(encoding="utf-8")
    for name in GUEST_REQUESTS:
        assert f"'{name}'" in build, (
            f"the build does not copy {name} into the romfs")
    assert "ConduitITCPro-Regular.otf" in build and "ConduitITCPro-Bold.otf" in build, (
        "the two Conduit faces are the port's universal fallback and must "
        "stay REQUIRED")
    stage = STAGE.read_text(encoding="utf-8")
    assert "files/.depot/" in stage, (
        "the staging tool does not read the game's depot")

    # 5. WHEN THE FACES ARE STAGED, THE ROMFS CARRIES THEM. The romfs is a
    #    build product of local assets, so this is checked only when the
    #    files are there.
    staged = 0
    for name in GUEST_REQUESTS:
        path = ROMFS_UI / name
        if path.is_file():
            staged += 1
            assert path.stat().st_size > 5_000, f"{name} is in the romfs but empty"
    if staged == 0:
        print("SKIP: no game faces are staged in the romfs, so only the "
              "source contract was checked")

    print(f"PASS: the port's face table matches {len(GUEST_REQUESTS)} guest "
          f"requests by exact name, {len(SYMBOL_FACES)} symbol fonts are "
          f"excluded, {staged}/{len(GUEST_REQUESTS)} staged faces are in the "
          "romfs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
