#!/usr/bin/env python3
"""The port's OpenSL device must be the device the guest's real FMOD asks for.

THE ORACLE IS THE GUEST'S OWN LIBRARY.  The strings inside
libfmodex.so name exactly what the library looks up when it needs an audio
backend: one C entry point and a set of IID symbols.  This test reads that
list out of the binary and requires the port's virtual libOpenSLES.so to
answer every name in it -- so if the guest ever asks for something new, the
test says so instead of the port failing at runtime.

The second half of the contract is the SHAPE of the answers:

  * a known IID must get the RIGHT interface;
  * an unknown IID must get an EXPLICIT failure, never another interface's
    vtable.  That is not a style rule: a mixer that reads a garbage volume
    out of the wrong vtable virtualises its voices away and the port looks
    like "no sound" instead of like a bug.
"""
import re
from pathlib import Path


def body_of(text, marker):
    """The text of the function whose DEFINITION starts at marker.

    A forward declaration has the same text as the definition, so the
    declaration is skipped: the definition is the occurrence whose next
    "{" comes before the next ";".
    """
    cursor = 0
    while True:
        start = text.find(marker, cursor)
        assert start >= 0, "no definition of " + marker
        open_brace = text.find("{", start)
        semicolon = text.find(";", start)
        if open_brace >= 0 and (semicolon < 0 or open_brace < semicolon):
            depth = 0
            for index in range(open_brace, len(text)):
                if text[index] == "{":
                    depth += 1
                elif text[index] == "}":
                    depth -= 1
                    if depth == 0:
                        return text[start:index + 1]
            raise AssertionError("unbalanced braces after " + marker)
        cursor = start + len(marker)


def guest_lookups(path):
    """The OpenSL symbols the guest's audio library names."""
    blob = path.read_bytes()
    names = set()
    for match in re.finditer(rb"[ -~]{4,}", blob):
        name = match.group(0).decode("ascii", "replace")
        if re.fullmatch(r"sl[A-Z][A-Za-z0-9_]*", name) or name.startswith("SL_IID"):
            names.add(name)
    return names


def main():
    root = Path(__file__).resolve().parents[1]
    stub = (root / "source" / "fmod_audio_stub.cpp").read_text(encoding="utf-8")
    header = (root / "include" / "rr3" / "fmod_audio_stub.hpp").read_text(encoding="utf-8")
    view = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    main_cpp = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    census = (root / ".." / "tools" / "Test-OnEden.ps1").resolve().read_text(encoding="utf-8")

    # 1. THE ORACLE: every symbol the guest names must be answered.
    guest = root / "romfs" / "libfmodex.so"
    if guest.exists():
        required = guest_lookups(guest)
        assert required, "no OpenSL symbols found in the guest's audio library"
        for name in sorted(required):
            assert ('strcmp(symbolName, "%s")' % name) in stub, \
                "the virtual libOpenSLES.so does not answer " + name
        # And the device must implement the two interfaces that carry the
        # volume and the format.
        assert "kGuidVolume" in stub and "kGuidAndroidConfiguration" in stub
        print("checked %d guest lookups against the virtual device" % len(required))
    else:
        print("SKIP: the guest's libfmodex.so is not staged, so the lookup "
              "list cannot be read from the oracle")

    # 2. A KNOWN IID GETS THE RIGHT INTERFACE.
    player = body_of(stub, "SlResult playerGetInterface(")
    assert "kGuidPlay" in player
    assert "kGuidVolume" in player and "gVolumeItfCell" in player, \
        "the player does not hand out the volume interface"
    assert "kGuidAndroidConfiguration" in player and "gAndroidConfigItfCell" in player, \
        "the player does not hand out the device configuration interface"
    assert "kGuidBufferQueue" in player and "kGuidAndroidSimpleBufferQueue" in player

    # 3. AN UNKNOWN IID IS AN EXPLICIT FAILURE -- NEVER ANOTHER INTERFACE.
    assert "kSlInterfaceUnavailable" in player, \
        "the player does not fail explicitly on an unknown IID"
    assert "// Buffer-queue IID or unknown" not in stub, \
        "the old 'the buffer queue for anything' fallback is back"
    engine = body_of(stub, "SlResult engineGetInterface(")
    assert "kSlInterfaceUnavailable" in engine, \
        "the engine does not fail explicitly on an unknown IID"
    assert "kGuidEngine" in engine

    # 4. THE DEVICE IS NOT MUTED BY DEFAULT.
    assert "kDefaultVolumeLevel = 100" in stub, \
        "the device's default volume level is not a live one"
    assert "level != 0" in stub, \
        "the self-test does not check that the device reports a live level"

    # 5. THE SELF-TEST'S EXPECTATION IS DERIVED, NOT A LITERAL IN A PRINT.
    assert "uint64_t fmodAudioStubSelfTestExpected();" in header
    assert "rr3::fmodAudioStubSelfTestExpected()" in main_cpp
    assert "expected=3f silent device" not in main_cpp, \
        "the boot log still prints a hardcoded expectation"

    # 6. THE DEVICE'S SELF-TEST IS A NUMBER SOMEBODY READS.
    assert "ast=0x%llx" in view, \
        "the census does not carry the audio device's self-test bits"
    assert "audioStats.selfTestBits" in view
    assert "' ast=0x([0-9a-fA-F]+)'" in census, \
        "the round census does not read the audio device's self-test"
    assert "$summary.msAudioSelfTest = $audioBits[$audioBits.Count - 1].Groups[1].Value" in census, \
        "the round census reads the audio bits and does not use them"
    assert "want 0x3ff" in census, \
        "the summary does not say what the audio self-test should be"

    print("PASS: the OpenSL device answers the guest's own lookups, exactly, "
          "and its self-test is in the census")


if __name__ == "__main__":
    main()
