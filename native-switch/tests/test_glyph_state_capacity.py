#!/usr/bin/env python3
"""The GlyphVector layout table must be able to hold every live GlyphVector.

WHAT WENT WRONG
---------------
The port keeps the measured layout of a text in a small table (one entry per
live GlyphVector) so that createTexture() can rasterise exactly what init()
measured. That table held 24 entries, while the port's fake-object pool
hands out 512 handles -- and the guest keeps more than 24 GlyphVectors alive
at once. The layout was therefore thrown away before createTexture() could
use it.

The symptom was invisible in a screenshot and loud in the log: the guest
called createTexture 4045 times in one run and only 140 produced a texture.
The other 3905 returned 0, so every paragraph they carried -- including the
description bubble under the career card -- was never drawn.

WHAT THIS PINS
--------------
  1. the layout table is at least as large as the fake-object pool, so a
     handle that the pool still owns always has a state;
  2. a slot that is handed out again never carries the previous object's
     layout (a stale layout rasterises the wrong glyphs);
  3. every way createTexture() can refuse has a REASON logged, because
     "the paragraph is invisible" and "the paragraph was never rasterised"
     are different bugs and a screenshot cannot tell them apart.

The numbers in the log this test is derived from are not in the repository,
so the test pins the RELATIONSHIP (table >= pool) and the properties, not a
run's counts. The counts themselves come out of the guest-view log at run
time, as phase=glyph-tex-fail.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GLYPH = ROOT / "source" / "jni_compat" / "classes" / "glyph_vector.cpp"
BRIDGE = ROOT / "source" / "jni_bridge.cpp"

CONST = re.compile(r"constexpr\s+size_t\s+(\w+)\s*=\s*(\d+)\s*;")


def read(path):
    return path.read_text(encoding="utf-8")


def constant(source, name):
    for found, value in CONST.findall(source):
        if found == name:
            return int(value)
    raise AssertionError(f"{name} is not defined as a constexpr size_t")


def body_of(source, signature):
    """The text of one function, from its signature to its closing brace."""
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
    glyph = read(GLYPH)
    bridge = read(BRIDGE)

    states = constant(glyph, "kStateCount")
    slots = constant(bridge, "kFakeObjectSlots")

    # 1. THE TABLE MUST COVER THE POOL.
    assert states >= slots, (
        f"the GlyphVector layout table holds {states} entries but the "
        f"fake-object pool hands out {slots} handles; a handle the pool "
        f"still owns can lose its layout before createTexture() uses it")

    # 2. A REUSED SLOT MUST NOT CARRY THE OLD LAYOUT.
    acquire = body_of(glyph, "RenderState* acquireState(")
    assert "hasLayout = false" in acquire, (
        "acquireState() hands out a slot without clearing hasLayout, so a "
        "stale layout can be paired with a fresh text")
    assert acquire.count("hasLayout = false") >= 1

    # 3. EVERY REFUSAL HAS A REASON.
    upload = body_of(glyph, "bool uploadTexture(")
    refusals = len(re.findall(r"\breturn false;", upload))
    reasons = len(re.findall(r"\bnoteTextureFailure\(", upload))
    assert refusals > 0, "uploadTexture() never refuses anything"
    assert reasons >= refusals, (
        f"uploadTexture() refuses {refusals} times but only logs a reason "
        f"{reasons} times; a silent refusal is indistinguishable from a "
        f"text that had nothing to say")

    # 4. THE REASONS ARE COUNTED, NOT SPAMMED.
    assert "glyph-tex-fail" in glyph, (
        "the failure reasons do not reach the guest-view log")
    assert "g_texFailCounts" in glyph, (
        "the failure reasons are not counted, so a run cannot say how many "
        "texts it lost")

    print(f"PASS: the GlyphVector layout table ({states} entries) covers the "
          f"fake-object pool ({slots} handles), a reused slot drops its stale "
          f"layout, and all {refusals} refusals in createTexture() are logged "
          f"with a reason")
    return 0


if __name__ == "__main__":
    sys.exit(main())
