#!/usr/bin/env python3
"""The paragraph line breaker has to break CJK where Android breaks it.

THE BUG
-------
A Chinese paragraph the guest draws into the career page's speech bubble was
laid out by a breaker that only knew two kinds of break: at a space, and
between the characters of a word wider than the whole paragraph.

A Chinese sentence has no spaces, so it was ONE word. It broke between
characters only when the WHOLE sentence was wider than the wrap width, and
when it did, it broke anywhere -- including inside a Latin word and,
worst of all, in front of a closing punctuation mark.

Measured on run 20261025-015035-eden-career2, the game's own career tip
("如果你想在现实世界赛车，请前往你本地的赛道。...") was laid out at 42 px as
THREE lines of 24 / 24 / 1 characters, and the last line held a single
full stop. Android's StaticLayout never starts a line with a full stop.

WHAT THIS PINS
--------------
  1. the port's layoutParagraph() runs the SHARED breaker
     (rr3/glyph_wrap.hpp), not a private copy of the old rule;
  2. the breaker breaks a CJK run between characters, keeps a Latin word
     whole, and never starts a line with closing punctuation;
  3. a Latin sentence still breaks at its spaces and a short string is still
     one line;
  4. the guest's OWN constants are re-derived from the guest binary and the
     port's source is asserted to carry the same values:
       - the JNI signature of initWithParagraph;
       - the fact that the guest promotes BOTH of its float arguments to
         double itself before the variadic call, which is why the port
         narrows a double back to a float when it unpacks the call;
       - the layout of the guest's own va_list, which is what the port's
         decoder reads the arguments out of.

The runtime part compiles native-switch/tests/host/glyph_wrap_selftest.cpp
for the host and runs it against the game's OWN faces
(native-switch/romfs/ui/NotoSansCJKsc-RR3Bootstrap.otf and
ConduitITCPro-Medium.otf), so the breaker is checked with real advances and
not with a made-up metric.
"""

import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

SOURCE = ROOT / "source" / "jni_compat" / "classes" / "glyph_vector.cpp"
BRIDGE = ROOT / "source" / "jni_bridge.cpp"
HEADER = ROOT / "include" / "rr3" / "glyph_wrap.hpp"
SELFTEST = ROOT / "tests" / "host" / "glyph_wrap_selftest.cpp"
UI_DIR = ROOT / "romfs" / "ui"
GUEST = REPO / "reverse" / "native" / "arm64" / "libRealRacing3.so"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest_disasm import load_segments  # noqa: E402

# THE GUEST'S JNI SIGNATURE, as the guest's own .rodata has it.
SIGNATURE = "(Lcom/firemint/realracing/Font;Ljava/lang/String;FFII)V"

# THE GUEST'S CALL SITE, at guest 0x12a77c8.
#
#   0x12a77c8  ldp  s0, s1, [x21, #8]      the two floats: wrap width, height
#   0x12a77d8  ldp  w5, w6, [x21, #0x10]    the two ints: break style, align
#   0x12a77dc  fcvt d0, s0                 the float is PROMOTED to a double
#   0x12a77e0  fcvt d1, s1                 and so is the other one
#
# The promotion is the guest's own: a variadic call promotes a float to a
# double, and the guest does it by hand so the double lands in q0 and q1.
# The port's decoder therefore has to read a double out of the vector
# register save area and narrow it back to a float.
GUEST_CALL_SITE = 0x12A77C8
GUEST_LDP_S0_S1 = 0x2D4106A0     # ldp s0, s1, [x21, #8]
GUEST_LDP_W5_W6 = 0x29421AA5     # ldp w5, w6, [x21, #0x10]
GUEST_FCVT_D0_S0 = 0x1E22C000
GUEST_FCVT_D1_S1 = 0x1E22C021

# THE GUEST'S OWN va_list, built by its variadic helper at guest 0xba7af8.
#
#   0xba7b14  stp  q0, q1, [sp]           the vector save area starts at sp
#   0xba7b34  sub  x10, x29, #0x78
#   0xba7b44  add  x10, x10, #0x28         gr_top = x29 - 0x50
#   0xba7b40  add  x9, x9, #0x80           vr_top = sp + 0x80
#   0xba7b4c  stp  x9, x8, [x29, #-0x18]   the block: [0] stack, [1] gr_top,
#   0xba7b50  stp  x12, x10, [x29, #-0x28]  [2] vr_top, [3] the offsets
#
# So the FP argument i lives at vr_top - 0x80 + 16*i and the GP argument i
# at gr_top - 0x28 + 8*i. The port's decoder must use exactly those.
GUEST_HELPER = 0xBA7B08
GUEST_VR_TOP_ADD = 0xBA7B40       # add x9, x9, #0x80
GUEST_GR_TOP_SUB = 0xBA7B34       # sub x10, x29, #0x78
GUEST_GR_TOP_ADD = 0xBA7B44       # add x10, x10, #0x28


def read_guest(blob, segs, vaddr, size):
    for vaddr_of, filesz, _memsz, offset, _flags in segs:
        if vaddr_of <= vaddr < vaddr_of + filesz:
            start = offset + (vaddr - vaddr_of)
            return blob[start:start + size]
    raise AssertionError(f"guest address {vaddr:#x} is not in a PT_LOAD segment")


def body_of(source, signature):
    """The body of the DEFINITION of `signature`.

    A forward declaration ends in ';' and has no body, so the first match is
    not always the definition; the first match whose argument list is closed
    and followed by '{' is.
    """
    at = source.find(signature)
    while at >= 0:
        depth = 0
        after = at + len(signature) - 1  # the '(' of the argument list
        for i in range(after, len(source)):
            if source[i] == "(":
                depth += 1
            elif source[i] == ")":
                depth -= 1
                if depth == 0:
                    after = i + 1
                    break
        while after < len(source) and source[after] in " \t\r\n":
            after += 1
        if after < len(source) and source[after] == "{":
            break
        at = source.find(signature, at + 1)
    assert at >= 0, f"{signature} has no definition in the source"
    start = source.find("{", at)
    depth = 0
    seen = False
    for i in range(start, len(source)):
        if source[i] == "{":
            depth += 1
            seen = True
        elif source[i] == "}":
            depth -= 1
            if seen and depth == 0:
                return source[at:i + 1]
    raise AssertionError(f"{signature} has no closing brace")


def main() -> int:
    for path in (SOURCE, BRIDGE, HEADER, SELFTEST):
        if not path.exists():
            print(f"SKIP: {path} is not there")
            return 0

    source = SOURCE.read_text(encoding="utf-8")
    bridge = BRIDGE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    # -----------------------------------------------------------------------
    # 1. THE PORT RUNS THE SHARED BREAKER.
    # -----------------------------------------------------------------------
    assert "rr3/glyph_wrap.hpp" in source, (
        "glyph_vector.cpp does not include rr3/glyph_wrap.hpp, so the port's "
        "layout is not the breaker's the host self-test checks")
    assert "rr3::glyphwrap::breakLines(" in source, (
        "glyph_vector.cpp does not call rr3::glyphwrap::breakLines, so the "
        "port lays paragraphs out with its own rule and not with the one the "
        "tests check")
    assert "One word: the run of codepoints up to the next space" not in source, (
        "the old word-based breaker is still in glyph_vector.cpp; a CJK "
        "sentence is one word and only breaks when the WHOLE sentence is "
        "wider than the wrap width")
    for name in ("isIdeographic", "isClosingPunctuation", "isOpeningPunctuation",
                 "canBreakBetween", "breakLines"):
        assert name in header, f"rr3/glyph_wrap.hpp has no {name}"
    assert "CharClass::Close" in header, (
        "rr3/glyph_wrap.hpp does not have a closing-punctuation class, so no "
        "line can be kept from starting with a full stop")

    # -----------------------------------------------------------------------
    # 2. THE INSTRUMENT: the guest's layout arguments have to be in the log.
    # -----------------------------------------------------------------------
    assert 'guestViewWideLog("glyph-para"' in source, (
        "the port does not log the paragraph's layout arguments, so a run "
        "cannot say what wrap width the guest asked for")
    for key in ("wrap=", "maxH=", "break=", "align=", "wrapping=", "lines="):
        assert key in source, (
            f"the glyph-para log line does not carry {key}")
    assert "noteParagraph(" in source, "the paragraph arguments are not logged"

    # -----------------------------------------------------------------------
    # 3. THE GUEST'S CONSTANTS, RE-DERIVED FROM THE GUEST BINARY.
    # -----------------------------------------------------------------------
    if not GUEST.exists():
        print(f"SKIP: {GUEST} is not there")
        return 0
    blob = GUEST.read_bytes()
    segs = load_segments(blob)

    assert blob.find(SIGNATURE.encode()) >= 0, (
        "the guest binary does not carry the initWithParagraph signature "
        f"{SIGNATURE}")
    assert SIGNATURE in source, (
        "the port registers a different signature for initWithParagraph than "
        "the guest's own .rodata carries")
    assert SIGNATURE in bridge or SIGNATURE in source, (
        "neither the bridge nor the class carries the guest's signature")

    call = read_guest(blob, segs, GUEST_CALL_SITE, 0x28)
    ldp_floats = struct.unpack_from("<I", call, 0x00)[0]
    ldp_ints = struct.unpack_from("<I", call, 0x10)[0]
    fcvt0 = struct.unpack_from("<I", call, 0x14)[0]
    fcvt1 = struct.unpack_from("<I", call, 0x18)[0]
    assert ldp_floats == GUEST_LDP_S0_S1, (
        "the guest's call site no longer loads its two floats with "
        "'ldp s0, s1, [x21, #8]', so the wrap width and the height limit are "
        "no longer known to arrive in that order")
    assert ldp_ints == GUEST_LDP_W5_W6, (
        "the guest's call site no longer loads its two ints with "
        "'ldp w5, w6, [x21, #0x10]', so the break style and the alignment are "
        "no longer known to arrive in that order")
    assert fcvt0 == GUEST_FCVT_D0_S0, (
        f"the guest's call site no longer promotes the first float with "
        f"fcvt d0, s0 (found {fcvt0:#010x}, expected {GUEST_FCVT_D0_S0:#010x}); "
        "the port's decoder reads a double out of the vector register save "
        "area, so this promotion is what makes the wrap width survive")
    assert fcvt1 == GUEST_FCVT_D1_S1, (
        f"the guest's call site no longer promotes the second float with "
        f"fcvt d1, s1 (found {fcvt1:#010x}, expected {GUEST_FCVT_D1_S1:#010x})")

    helper = read_guest(blob, segs, GUEST_HELPER, 0x40)
    vr_add = struct.unpack_from("<I", helper, GUEST_VR_TOP_ADD - GUEST_HELPER)[0]
    gr_sub = struct.unpack_from("<I", helper, GUEST_GR_TOP_SUB - GUEST_HELPER)[0]
    gr_add = struct.unpack_from("<I", helper, GUEST_GR_TOP_ADD - GUEST_HELPER)[0]
    assert vr_add == 0x91020129, (
        "the guest's variadic helper no longer sets the vector save area top "
        "to sp + 0x80")
    assert gr_sub == 0xD101E3AA and gr_add == 0x9100A14A, (
        "the guest's variadic helper no longer sets the general-purpose "
        "register save area top to x29 - 0x50")
    assert "fpTop - 0x80 + 16 * fpIndex" in bridge, (
        "the port's decoder does not read the float arguments at "
        "vr_top - 0x80 + 16*i, which is where the guest's own va_list puts "
        "them")
    assert "gpTop - 0x28 + 8 * gpIndex" in bridge, (
        "the port's decoder does not read the reference and integer arguments "
        "at gr_top - 0x28 + 8*i, which is where the guest's own va_list puts "
        "them")
    decode = body_of(bridge, "static size_t decodeGuestVarArgs(")
    assert "double promoted" in decode and "static_cast<jfloat>(promoted)" in decode, (
        "the port's decoder does not narrow the guest's promoted double back "
        "to a float, so the wrap width would arrive as garbage")

    # The port's own upper bound on a wrap width. It is the one place where a
    # LARGE width silently turns wrapping off, so the test pins its value.
    assert "kMaxWrapWidth = 4096.f" in source, (
        "the port's maximum wrap width is no longer 4096.f, so a guest wrap "
        "width above it would stop wrapping without saying so")

    # -----------------------------------------------------------------------
    # 4. THE RUNTIME: the shared breaker against the game's own faces.
    # -----------------------------------------------------------------------
    compiler = shutil.which("g++") or shutil.which("c++") or shutil.which("clang++")
    if compiler is None:
        print("SKIP: no host C++ compiler, the runtime part did not run")
        print("PASS: the port runs the shared CJK line breaker and the guest's "
              "own constants match")
        return 0
    if not (UI_DIR / "NotoSansCJKsc-RR3Bootstrap.otf").exists():
        print("SKIP: the game's Chinese face is not staged")
        print("PASS: the port runs the shared CJK line breaker and the guest's "
              "own constants match")
        return 0

    with tempfile.TemporaryDirectory() as work:
        binary = Path(work) / "glyph_wrap_selftest"
        build = subprocess.run(
            [compiler, "-std=c++20", "-O1", "-I", str(ROOT / "include"),
             str(SELFTEST), "-o", str(binary)],
            capture_output=True, text=True)
        assert build.returncode == 0, (
            "the host self-test does not compile:\n" + build.stdout + build.stderr)
        run = subprocess.run([str(binary), str(UI_DIR)],
                             capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        assert run.returncode == 0, (
            "the host self-test failed:\n" + run.stdout + run.stderr)
        assert "PASS:" in run.stdout, (
            "the host self-test did not report PASS:\n" + run.stdout)

    print("PASS: the port runs the shared CJK line breaker, the CJK break "
          "points are legal, the Latin behaviour is unchanged and the guest's "
          "own constants match the port's source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
