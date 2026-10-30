#!/usr/bin/env python3
"""The guest's device profile is ENCRYPTED -- and the cipher must stay
reversible, or the port cannot ship a profile the guest will accept.

WHAT IS PINNED HERE
-------------------
The cipher is the guest's RC4 variant (native-switch's guest binary,
libRealRacing3.so, the profile reader at ELF 0x13c5954 / the keystream at
0x13c5a08).  Two instruction-level quirks make it NOT a textbook RC4:

  * the S box lives at B[2..257] inside a 262-byte frame;
  * the 133-byte drop loop reads the UN-OFFSETED B[i] while writing the
    offsetted B[2+j].

The key is 8 bytes: a5 35 b3 b1 e8 43 e7 cf.

WHY IT MATTERS
--------------
The guest picks its GUI atlas tree and its quality switches from
`eds/<device>.plist`, and it only reads that file if it DECRYPTS it.  A
cipher that is one instruction out produces a file the guest silently
cannot read, and the symptom is a black menu a hundred frames later.  So
the algorithm is pinned here with a known-answer vector that is NOT any
EA material -- it is a synthetic profile this test file itself declares.
"""

import importlib.util
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT.parent / "tools" / "rr3-plist.py"

# ---- the known-answer vector (synthetic, written by this test) -----------
KAT_PLAIN = (b'<?xml version="1.0" encoding="UTF-8"?>\r\n'
              b'<plist version="1.0"><dict><key>EXPECTED_SCREEN_WIDTH</key>'
              b'<integer>2048</integer></dict></plist>')
KAT_CIPHER = bytes.fromhex(
    "e6ea60c54026f388a8a65a62e99576bfdbe5de278565ae74d6f4750b77ff"
    "2033836ea84d062355f83f2a3df5e381162c13d1413e9f87125a103de2dc012"
    "6fd854e54b818c43c75567d7c9997fa180bb68d7b21fc1b78af60ec465c63934f"
    "baa4a66fdb6f36c3c5e20360723f111fa25bc3c7608b51a89ebc6ffd7afc89c3c1"
    "bf674616d4398cc5a61c")

# The volumes live outside the repository (they are EA material and must
# never be committed), so the real-profile check is conditional.
VOLUME_CANDIDATES = [
    ROOT.parent / ".build" / "volumes-rebuild" / "switch" / "rr3-native-rehost"
    / "data" / "core.romfs",
]


def load_tool():
    spec = importlib.util.spec_from_file_location("rr3plist", str(TOOL))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    assert TOOL.exists(), "tools/rr3-plist.py is missing"
    m = load_tool()

    # ---- 1. the key -----------------------------------------------------
    assert m.KEY == bytes.fromhex("a535b3b1e843e7cf"), (
        "the profile cipher's key changed: %s" % m.KEY.hex())

    # ---- 2. the known-answer vector -------------------------------------
    got = m.encrypt(KAT_PLAIN)
    assert got == KAT_CIPHER, (
        "the cipher no longer produces the known ciphertext.\n"
        "  expected %s\n  got      %s" % (KAT_CIPHER.hex(), got.hex()))
    assert m.decrypt(KAT_CIPHER) == KAT_PLAIN, (
        "the cipher no longer decrypts the known ciphertext")
    print("known-answer vector : the cipher is the one the guest uses")

    # ---- 3. it is a stream cipher: the same operation both ways ---------
    for length in (1, 16, 137, 4096):
        data = bytes((i * 37 + 11) & 0xFF for i in range(length))
        assert m.decrypt(m.encrypt(data)) == data, (
            "the round trip failed for %d bytes" % length)
    print("round trip          : ok for 1, 16, 137 and 4096 bytes")

    # ---- 4. the plaintext length must not change -------------------------
    # The profile lives in a ROMFS record whose size is fixed, and the
    # cipher is a stream cipher, so the plaintext and the ciphertext have
    # the same length.  A profile that grew would not fit.
    assert len(m.encrypt(KAT_PLAIN)) == len(KAT_PLAIN), (
        "the cipher changed the length; the profile would not fit its record")

    # ---- 5. the length fit only touches whitespace BETWEEN elements ------
    xml = (b'<?xml version="1.0" encoding="UTF-8"?>\r\n'
           b'<plist version="1.0">\r\n  <dict>\r\n    <key>A</key>\r\n'
           b'    <string>keep me</string>\r\n  </dict>\r\n</plist>\r\n')
    fitted = m.fit_to_length(xml, len(xml) - 4)
    assert len(fitted) == len(xml) - 4, "the fit did not reach the wanted size"
    assert b'<string>keep me</string>' in fitted, (
        "the fit shortened a TEXT VALUE; that would change what the guest reads")
    assert b'<?xml' in fitted and b'</plist>' in fitted, "the fit broke the XML"
    print("length fit          : only the whitespace between elements moves")

    # ---- 6. the real profiles, when the volumes are here -----------------
    volume = None
    for candidate in VOLUME_CANDIDATES:
        if candidate.exists():
            volume = candidate
            break
    if volume is None:
        print("the volumes are not here: the real-profile check is skipped")
        print("(the data packages are EA material and are never committed)")
    else:
        with open(volume, 'rb') as f:
            blob = f.read()
        profiles = m.find_profiles(blob)
        assert profiles, "no device profile found in the volume"
        checked = 0
        for name, start, size in profiles:
            raw = blob[start:start + size]
            text = m.decrypt(raw)
            assert text.startswith(b'<?xml'), (
                "%s does not decrypt to a profile" % name)
            assert m.encrypt(text) == raw, (
                "%s does not round trip" % name)
            checked += 1
        print("real profiles       : %d decrypt to XML and round trip" % checked)

        # The guest asks for the profile by name, and the port ships one
        # tree; the tree selector has to name a tree that exists.
        android = [p for p in profiles if p[0] == 'AndroidHigh.plist']
        if android:
            name, start, size = android[0]
            text = m.decrypt(blob[start:start + size])
            kv = m.read_key_values(text)
            assert 'GUI_LAYOUT_EXTENSION' in kv, (
                "the profile has no GUI_LAYOUT_EXTENSION")
            assert 'EXPECTED_SCREEN_WIDTH' in kv, (
                "the profile has no EXPECTED_SCREEN_WIDTH")
            print("the guest's profile : %s, GUI_LAYOUT_EXTENSION=%s"
                  % (name, kv['GUI_LAYOUT_EXTENSION'][1]))

    # ---- 7. the static contract: the tool writes the CIPHERTEXT --------
    source = TOOL.read_text(encoding="utf-8")
    assert "blob[start:start + size] = cipher" in source, (
        "the tool writes the plaintext where the ciphertext belongs; the "
        "guest would then read a file it cannot decrypt")
    assert "THE WRITE DID NOT ROUND TRIP" in source, (
        "the tool does not check that the record decrypts again")
    assert "holds a PLAIN TEXT profile" in source, (
        "the tool cannot repair a record that was written the wrong way round")
    print("static contract     : the tool writes the ciphertext and checks it")
    print()
    print("PASS: the profile cipher is pinned and the tool round trips it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
