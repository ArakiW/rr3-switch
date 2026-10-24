#!/usr/bin/env python3
"""Stage the game's own UI fonts out of an authorized data pack.

WHY THIS EXISTS
---------------
The port draws the game's UI text with the game's OWN typefaces. The guest
asks for a specific face by name -- the log shows fourteen distinct ones --
and the guest's own Java measures the text with THAT face, so the port has
to measure it with the same one or the laid-out paragraph is not the
paragraph the guest asked for.

The faces live in the data pack (files/.depot/*.otf and *.ttf). They are EA
assets, so they must never enter the repository. They are staged into
analysis/rr3-ui-source/files/.depot/, which .gitignore already excludes, and
tools/Build-NativeSwitch.ps1 copies them from there into the NRO's romfs.

WHAT IT STAGES
--------------
Every font file the pack carries at files/.depot/, plus a small report of
what each one actually is, because "which of these can be used to draw
paragraphs" is not a question the file names answer:

    upem      the font's design grid
    ascent    the hhea ascent, in design units
    glyphs    the number of glyphs
    ascii     how many of the printable ASCII code points it covers
    cjk       how many CJK code points it covers
    ascent_em the ascent in em, which is what makes a SYMBOL font obvious:
              a text face ascends about 0.75..1.0 em, and the game's
              symbol font ascends 2.75 em because its "letters" are the big
              placeholder boxes the game draws its currency art into.

Usage:
    python tools/stage_rr3_fonts.py <data-pack.zip> [--depot <dir>]

The pack is read as a zip: only the small font members are decompressed.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import zipfile


PREFIX = "files/.depot/"
FONT_SUFFIXES = (".otf", ".ttf", ".ttc", ".otc")

# The faces the port needs to have in order to draw the UI at all. The two
# Conduit faces are the universal fallback, so they are the only ones the
# build refuses to do without.
REQUIRED = ("ConduitITCPro-Regular.otf", "ConduitITCPro-Bold.otf")


def font_report(path: Path) -> dict:
    """Measure a face with fontTools, if it is installed."""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return {"measured": False, "reason": "fontTools is not installed"}
    try:
        face = TTFont(str(path), lazy=True, fontNumber=0)
        head = face["head"]
        hhea = face["hhea"]
        cmap = face.getBestCmap() or {}
        report = {
            "measured": True,
            "upem": head.unitsPerEm,
            "ascent": hhea.ascent,
            "descent": hhea.descent,
            "glyphs": face["maxp"].numGlyphs,
            "cmap": len(cmap),
            "ascii": sum(1 for c in range(0x20, 0x7F) if c in cmap),
            "cjk": sum(1 for cp in cmap if 0x4E00 <= cp <= 0x9FFF),
        }
        report["ascent_em"] = round(report["ascent"] / report["upem"], 4)
        face.close()
        return report
    except Exception as error:  # a broken face must not stop the staging
        return {"measured": False, "reason": str(error)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("archive", type=Path,
                        help="the authorized data pack (.zip)")
    parser.add_argument("--depot", type=Path, default=None,
                        help="where to stage the fonts "
                             "(default analysis/rr3-ui-source/files/.depot)")
    args = parser.parse_args()
    project = Path(__file__).resolve().parent.parent
    depot = args.depot or (project / "analysis" / "rr3-ui-source" / "files" / ".depot")

    if not args.archive.is_file():
        print(f"ERROR: no such data pack: {args.archive}", file=sys.stderr)
        return 1

    depot.mkdir(parents=True, exist_ok=True)
    staged = []
    with zipfile.ZipFile(args.archive) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/")
            if not name.startswith(PREFIX):
                continue
            relative = name[len(PREFIX):]
            if "/" in relative or not relative.lower().endswith(FONT_SUFFIXES):
                continue
            destination = depot / relative
            with archive.open(info) as source, destination.open("wb") as out:
                out.write(source.read())
            staged.append(destination)

    if not staged:
        print(f"ERROR: no fonts found under {PREFIX} in {args.archive}",
              file=sys.stderr)
        return 1

    report = {}
    for path in sorted(staged):
        info = font_report(path)
        report[path.name] = info
        measured = info.get("measured")
        if measured:
            print(f"  {path.name:<32} upem={info['upem']:<5} "
                  f"ascent={info['ascent']:<6} ({info['ascent_em']} em) "
                  f"glyphs={info['glyphs']:<5} ascii={info['ascii']:<4} "
                  f"cjk={info['cjk']}")
        else:
            print(f"  {path.name:<32} not measured: {info.get('reason')}")

    manifest = depot / "rr3-font-report.json"
    manifest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")

    missing = [name for name in REQUIRED
               if not (depot / name).is_file()]
    if missing:
        print(f"ERROR: the pack did not carry the required face(s): "
              f"{', '.join(missing)}", file=sys.stderr)
        return 1
    print(f"STAGED {len(staged)} font(s) into {depot}")
    print(f"REPORT  {manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
