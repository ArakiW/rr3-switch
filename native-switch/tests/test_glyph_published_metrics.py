#!/usr/bin/env python3
"""The metrics the guest reads back must describe the texture it draws.

THE BUG
-------
GlyphVector.createTexture() returns a texture, and the guest then draws it
using a source rectangle it derives from the GlyphVector's own metrics:
    v = pixelBoundsH / texHeight * 16384
(16384 is 1.0 of the texture; the guest's own vertex shader multiplies the
attribute by u_TexMatrix0 = 1/16384.)

The port published texId/texWidth/texHeight into the facade when it uploaded
the texture, but NOT the six metrics. Those six were whatever the last
measurement on that handle had written. The guest re-uses one GlyphVector
handle for dozens of strings, so a texture could be uploaded from one
measurement and drawn with another's height.

Measured on run 20261025-031524-eden-owner2: the string "XX / XX" was
rasterised 122 rows tall into a 512x128 texture, but the guest sampled only
30 of those rows (v = 3840) and drew the top third of every X -- two
uncrossed diagonal strokes each, which read as "WV / WV" on screen.

WHAT THIS PINS
--------------
  1. the metrics are COPIED into the render state when they are measured;
  2. uploadTexture() publishes those COPIED metrics together with the
     texture, so the two can never come from different measurements;
  3. a pooled handle that is handed to a new GlyphVector drops the previous
     occupant's state, so a createTexture() that arrives before the new
     object's own init() cannot upload the previous occupant's raster.

This is a SOURCE-LEVEL contract, like the other tests in this directory: the
behaviour only shows up in a run's guest-view log, and the log's cap hides
the createTexture record for the offending texture. The runtime check is the
one in the report: the text-quad's v must equal
pixelBoundsH/texHeight*16384 for the text that was actually rasterised.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source" / "jni_compat" / "classes" / "glyph_vector.cpp"

METRICS = (
    "offsetX",
    "offsetY",
    "boundsW",
    "boundsH",
    "pixelBoundsW",
    "pixelBoundsH",
    "numLines",
)


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


def main() -> int:
    if not SOURCE.exists():
        print(f"SKIP: {SOURCE} is not there")
        return 0
    source = SOURCE.read_text(encoding="utf-8")

    # 1. THE MEASUREMENT IS COPIED INTO THE STATE.
    render = body_of(source, "void renderText(")
    for name in METRICS:
        assert re.search(rf"state->published\.{name}\s*=\s*facade->{name}\s*;",
                         render), (
            f"renderText() does not copy {name} into state->published, so the "
            f"published metrics are not the measured ones")

    # 2. THE TEXTURE IS PUBLISHED WITH THOSE METRICS.
    upload = body_of(source, "bool uploadTexture(")
    for name in METRICS:
        assert re.search(rf"facade->{name}\s*=\s*state->published\.{name}\s*;",
                         upload), (
            f"uploadTexture() does not publish {name} from the state, so the "
            f"guest can draw the texture with another measurement's {name}")
    # The texture id and its size must still be published.
    for name in ("texId", "texWidth", "texHeight"):
        assert re.search(rf"facade->{name}\s*=", upload), (
            f"uploadTexture() no longer publishes {name}")

    # 3. A RECYCLED HANDLE DOES NOT INHERIT THE PREVIOUS OCCUPANT.
    new_object = body_of(source, "bool onNewObject(")
    assert "discardState(object)" in new_object, (
        "onNewObject() does not drop the previous occupant's render state, so "
        "a createTexture() that arrives before the new object's init() can "
        "upload the previous occupant's raster")
    discard = body_of(source, "void discardState(")
    assert "hasLayout = false" in discard, (
        "discardState() leaves hasLayout set, so the discarded layout can "
        "still be rasterised")
    assert re.search(r"free\(\s*state->text\s*\)", discard), (
        "discardState() does not free the text it drops")

    print("PASS: the GlyphVector metrics are measured into the render state, "
          "published with the texture they describe, and a recycled handle "
          "drops the previous occupant's state")
    return 0


if __name__ == "__main__":
    sys.exit(main())
