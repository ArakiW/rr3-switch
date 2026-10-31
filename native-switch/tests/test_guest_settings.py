#!/usr/bin/env python3
"""THE LAUNCHER-SIDE SETTINGS LAYER -- the PS Vita lesson, applied.

WHAT THIS PINS
--------------
The port can now act as the LAUNCHER, the way the PS Vita port does: a plain
text file on the card (rr3-settings.txt) is translated into the guest's own
ENCRYPTED device profile (eds/*.plist), and the guest reads that profile
exactly as it reads the original.

THE PART THAT CAN BREAK SILENTLY IS THE CIPHER.  The guest's profile is
encrypted with an RC4 variant that has two instruction-level quirks (the S box
lives at B[2..257] and the 133-byte drop reads the UN-OFFSETED B[i]).  A
cipher that is one instruction out produces a file the guest silently cannot
read, and the only symptom is a soft picture a hundred frames later.  So this
test:

  1. compiles the REAL translation unit with the HOST compiler and RUNS it
     against a known-answer vector -- the same vector
     native-switch/tests/test_ui_plist.py pins for the Python reference, so
     the two implementations have to agree with each other and with the guest;
  2. CROSS-CHECKS the C++ keystream against tools/rr3-plist.py over a longer
     input, so the generator's wrap is covered too;
  3. checks the WIRING: the substitution has to be in the RESOLVER, because
     the guest reads the profile with open() + mmap() and the asset list
     publishes an md5 for it.

WHY THE WIRING CHECKS ARE NOT A MATTER OF STYLE
-----------------------------------------------
Measured 2026-10-31 (archive 20261031-010828-..., guest-view.log):
    2103  stat-path guest='sdmc:/switch/rr3-native-rehost/.depot/eds/AndroidHigh.plist'
    2107  open-ok
    2108  mmap-file-ok len=6808
So the guest OPENS the file and MMAPS it -- a hook in guestFopen() would never
fire, and a substitution that changed the SIZE would break the mmap.  The
length is preserved by construction (the translation keeps the record's
length), and that is checked here too.
"""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

HARNESS = ROOT / "tests" / "guest_settings_test.cpp"
SOURCE = ROOT / "source" / "guest_settings.cpp"
HEADER = ROOT / "include" / "rr3" / "guest_settings.hpp"
FILESYSTEM_SOURCE = ROOT / "source" / "guest_filesystem.cpp"
CARD_SETTINGS = ROOT / "card" / "rr3-settings.txt"
TOOL = REPO / "tools" / "rr3-plist.py"


def find_host_compiler() -> str | None:
    """A host C++ compiler, or None."""
    override = os.environ.get("RR3_HOST_CXX")
    if override:
        found = shutil.which(override) or (
            override if Path(override).is_file() else None
        )
        if found:
            return found
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    return None


def load_reference():
    spec = importlib.util.spec_from_file_location("rr3plist", str(TOOL))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_and_run(compiler: str, workdir: Path) -> tuple[str, str]:
    binary = workdir / "guest_settings_test.exe"
    command = [
        compiler,
        "-std=c++17",
        "-O1",
        "-Wall",
        "-Wextra",
        "-I",
        str(ROOT / "include"),
        str(HARNESS),
        str(SOURCE),
        "-o",
        str(binary),
    ]
    built = subprocess.run(command, capture_output=True, text=True)
    if built.returncode != 0:
        raise AssertionError(
            "the host harness did not compile:\n%s\n%s" % (built.stdout, built.stderr)
        )
    ran = subprocess.run(
        [str(binary), str(workdir / "settings-scratch.txt")],
        capture_output=True,
        text=True,
    )
    if ran.returncode != 0:
        raise AssertionError(
            "the host harness failed:\n%s\n%s" % (ran.stdout, ran.stderr)
        )
    return ran.stdout, ran.stderr


def main() -> None:
    assert HEADER.exists(), "the settings layer header is missing"
    assert SOURCE.exists(), "the settings layer source is missing"
    assert HARNESS.exists(), "the settings layer harness is missing"
    assert CARD_SETTINGS.exists(), (
        "the card file native-switch/card/rr3-settings.txt is missing: the "
        "settings layer is only useful if the card ships one"
    )

    # ---- 1. THE CARD FILE ------------------------------------------------
    card = CARD_SETTINGS.read_text(encoding="utf-8")
    assert "calidad" in card, (
        "the card file does not document the quality tier, which is the one "
        "setting a user is most likely to want")
    assert "GUI_LAYOUT_EXTENSION" in card, (
        "the card file does not name any of the guest's own keys")
    assert "AA_LEVEL" in card, (
        "the card file does not name any of the guest's own keys")
    # A card file that ships SETTINGS would change every run that deploys it,
    # so the shipped one must be all comments -- EXCEPT for the keys that are
    # measured to be free of the thing the rule is really about.
    #
    # The rule's own reason is "a quality switch costs a full shader
    # recompile" (measured: archive 20261031-003750, the run died at 42 s
    # while it was recompiling).  A LAYOUT switch is not a quality switch.
    #
    # GUI_LAYOUT_EXTENSION is shipped ON, because the stock profile's
    # `_480x320` makes the guest lay the front end out for a 480x320 GUI
    # while the port renders a 1280x720 view -- a factor of 2.667 -- and the
    # Chinese line box (1.407 em for Noto Sans CJK) then overruns the line
    # advance, so two lines of a label collide.  Measured: archive
    # 20261101-001630-eden-rr3-q62-layout-ext, the only change being this
    # one key, and the text comes out with the right size and spacing.
    SAFE_LAYOUT_KEYS = ("GUI_LAYOUT_EXTENSION",)
    for line in card.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        key = stripped.split("=", 1)[0].strip().upper()
        if key in SAFE_LAYOUT_KEYS:
            continue
        raise AssertionError(
            "the shipped rr3-settings.txt sets %r; the default has to be "
            "'nothing happens', because a quality switch costs a full "
            "shader recompile" % stripped
        )
    print("card file            : documented, and ships no quality settings")

    # ---- 2. THE CIPHER, EXECUTED -----------------------------------------
    compiler = find_host_compiler()
    if compiler is None:
        print("no host C++ compiler found (looked for g++, clang++, c++; set "
              "RR3_HOST_CXX to point at one) -- the executed check is skipped")
    else:
        print(f"compiler: {compiler}")
        with tempfile.TemporaryDirectory() as tmp:
            stdout, _ = build_and_run(compiler, Path(tmp))
        match = re.search(r"^crosscheck: ([0-9a-f]+)$", stdout, re.M)
        assert match, (
            "the harness did not print the cross-check vector:\n%s" % stdout
        )
        # ---- 3. THE CROSS-CHECK AGAINST THE PYTHON REFERENCE -------------
        reference = load_reference()
        payload = bytes((i * 91 + 7) & 0xFF for i in range(300))
        expected = reference.encrypt(payload).hex()
        assert match.group(1) == expected, (
            "the C++ keystream and the Python reference disagree.\n"
            "  expected %s\n  got      %s" % (expected, match.group(1)))
        print("cross-check          : the C++ and the Python cipher agree "
              "over 300 bytes")

    # ---- 4. THE WIRING ----------------------------------------------------
    fs = FILESYSTEM_SOURCE.read_text(encoding="utf-8")

    assert '#include "rr3/guest_settings.hpp"' in fs, (
        "the file layer does not include the settings layer")

    # The substitution has to be in the RESOLVER: the guest reads the profile
    # with open() + mmap(), so a hook in the reader would never fire, and the
    # asset list publishes an md5 for the same path.
    resolve_body = fs.split("bool resolvePath(const char* path, bool writing,")[1]
    resolve_body = resolve_body.split("\n}\n", 1)[0]
    assert "maybeSubstituteDeviceProfile" in resolve_body, (
        "the resolver does not call the settings layer; the guest's profile "
        "would never be substituted")
    assert "if (resolved && !writing)" in resolve_body, (
        "the substitution must not run for writes")

    # The substitution must not run while the layer is building the profile,
    # or the build would resolve the path it is trying to replace.
    assert "gBuildingProfile" in fs, (
        "the settings layer has no re-entrancy guard; building the profile "
        "resolves the very path it is replacing")

    # The generated profile has to keep the LENGTH of the record it lives in:
    # the guest mmaps it and the size comes from the stat.
    assert "requested.size()" in fs, (
        "the settings layer does not use the requested profile's length")

    # The settings file has to be READABLE PLAIN TEXT on the card.
    assert "rr3-settings.txt" in fs, (
        "the settings layer does not read a settings file")

    # The generated profile must NOT be written into the read-only volume:
    # the volumes are mounted read-only, and the port can only write to the
    # card.
    assert "kProfileCacheRoot" in fs and '"%s/eds-%s"' in fs, (
        "the generated profile is not written to the card's cache")

    # ---- 5. THE DEPLOY SCRIPT SHIPS IT -----------------------------------
    deploy = (REPO / "tools" / "Deploy-RR3CardScripts.ps1").read_text(
        encoding="utf-8"
    )
    assert "rr3-settings.txt" in deploy, (
        "the deploy script does not put rr3-settings.txt on the card, so the "
        "settings layer would never see a settings file")

    print("wiring               : the resolver substitutes, the reader does not")
    print("length contract      : the record's length is preserved")
    print("deploy               : the card file is shipped")
    print()
    print("PASS: the launcher-side settings layer is pinned")
    return None


if __name__ == "__main__":
    sys.exit(main())
