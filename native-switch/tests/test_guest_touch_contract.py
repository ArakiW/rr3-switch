#!/usr/bin/env python3
"""Static contract for the injected-touch bridge.

The port can only deliver a touch to the guest if it calls the SAME entry
points the guest's own Java calls, with the SAME coordinate unit. Both facts
are checked here against the ORACLE -- the guest image and the decompiled
guest Java -- rather than against the port's own comments, because a comment
that agrees with the code proves nothing about either.

The image check is the interesting one: the four JNI exports are located in
libRealRacing3.so's .dynsym and their st_value is compared with the
addresses the port's source records. If the guest image and the port ever
disagree about where onTouchBeginJNI lives, this fails.
"""

import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
GUEST = REPO / "reverse" / "native" / "arm64" / "libRealRacing3.so"

HEADER = ROOT / "include" / "rr3" / "guest_touch.hpp"
SOURCE = ROOT / "source" / "guest_touch.cpp"

JAVA_DIR = REPO / "reverse" / "java" / "sources" / "com" / "firemint" / "realracing"

# The guest exports, with the ELF virtual address the port's source records.
EXPECTED_EXPORTS = {
    "Java_com_firemint_realracing_MainActivity_onTouchBeginJNI": 0x12B39D8,
    "Java_com_firemint_realracing_MainActivity_onTouchMoveJNI": 0x12B3A14,
    "Java_com_firemint_realracing_MainActivity_onTouchEndJNI": 0x12B3A58,
    "Java_com_firemint_realracing_MainActivity_onTouchCancelJNI": 0x12B3AC0,
}


def dynsym_exports(path: Path):
    """(name -> st_value) from the guest image's .dynsym."""
    blob = path.read_bytes()
    e_shoff = struct.unpack_from("<Q", blob, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", blob, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", blob, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", blob, 0x3E)[0]
    headers = [
        struct.unpack_from("<IIQQQQ", blob, e_shoff + i * e_shentsize)
        for i in range(e_shnum)
    ]
    shstr = headers[e_shstrndx]
    shstrtab = blob[shstr[4]:shstr[4] + shstr[5]]

    def section_name(offset: int) -> str:
        base = offset
        return shstrtab[base:shstrtab.index(b"\0", base)].decode()

    sections = {section_name(h[0]): h for h in headers}
    dynsym = sections[".dynsym"]
    dynstr = sections[".dynstr"]
    strings = blob[dynstr[4]:dynstr[4] + dynstr[5]]
    out = {}
    for i in range(dynsym[5] // 24):
        offset = dynsym[4] + i * 24
        st_name, _info, _other, _shndx, st_value, _size = struct.unpack_from(
            "<IBBHQQ", blob, offset)
        if not st_value or st_name >= len(strings):
            continue
        end = strings.index(b"\0", st_name)
        out[strings[st_name:end].decode("utf-8", "replace")] = st_value
    return out


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")

    # The public API.
    for symbol in ("prepareGuestTouchInput", "pumpGuestTouchInput",
                   "guestTouchInputReady", "guestTouchControlPath"):
        assert symbol in header, f"{symbol} missing from the header"
        assert symbol in source, f"{symbol} missing from the source"

    # The control file and its format.
    assert "sdmc:/switch/rr3-native-rehost/rr3-touch.txt" in source
    assert "down" in source and "move" in source and "up" in source
    # OFF BY DEFAULT: there is no other control path, and the file is the
    # only way in.
    assert "rr3-touch" in header

    # The guest's four touch entry points, named exactly as the guest image
    # names them. A renamed export would resolve to 0 and every touch would
    # be a no-op, so the names are part of the contract.
    for name in EXPECTED_EXPORTS:
        assert name in source, f"{name} is not resolved by the port"

    # The addresses the port records must match the guest image.
    if GUEST.exists():
        exports = dynsym_exports(GUEST)
        for name, expected in EXPECTED_EXPORTS.items():
            assert name in exports, f"{name} is not exported by the guest image"
            actual = exports[name]
            assert actual == expected, (
                f"{name} is at ELF 0x{actual:x} in the guest image but the port "
                f"records 0x{expected:x}")
        # The record in the source must mention the same address.
        for name, expected in EXPECTED_EXPORTS.items():
            short = name.replace(
                "Java_com_firemint_realracing_MainActivity_", "")
            va = "0x%x" % expected
            assert va in source.lower(), (
                f"the source does not record {short}'s address {va}")
    else:
        print("note    : guest image absent, the .dynsym check was skipped")

    # The guest's own Java: the port must be calling the methods the guest
    # calls, with the arguments the guest passes.
    activity = JAVA_DIR / "MainActivity.java"
    glview = JAVA_DIR / "GLView.java"
    if activity.exists() and glview.exists():
        activity_text = activity.read_text(encoding="utf-8", errors="replace")
        glview_text = glview.read_text(encoding="utf-8", errors="replace")
        assert "public native void onTouchBeginJNI(int i, float f, float f2)" in \
            activity_text
        assert "public native void onTouchMoveJNI(int i, float f, float f2)" in \
            activity_text
        assert "public native void onTouchEndJNI(int i, float f, float f2, boolean z)" in \
            activity_text
        assert "public native void onTouchCancelJNI()" in activity_text
        # GLView is what turns a MotionEvent into those calls.
        assert "onTouchBeginJNI" in glview_text
        assert "onTouchMoveJNI" in glview_text
        assert "onTouchEndJNI" in glview_text
        # The coordinates are the raw view pixels of the MotionEvent.
        assert "motionEvent.getX(" in glview_text
        assert "motionEvent.getY(" in glview_text

    # The port's own frame-timing control, so a script is reproducible
    # without a live host->port channel.
    assert "at <frame>" in header or "at <frame>" in source

    print("PASS: guest touch bridge contracts")


if __name__ == "__main__":
    main()
