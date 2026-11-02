#!/usr/bin/env python3
"""The gamepad layer, executed: the report parsers and the mapping file.

WHAT IS PROVED HERE.  The port accepts a gamepad from a family of
controllers -- the Switch Pro Controller, a DualShock 4, a DualSense, an
Xbox 360 pad, an Xbox One pad and an Xbox Elite pad -- and it maps whatever
the controller says onto the guest's own controls with a file on the card.

That mapping is only worth anything if the parsers DECODE the reports
correctly, and a parser that is wrong in a way nobody notices is worse than
no parser at all: the user plugs in a controller, the game reads the inputs
as garbage, and there is nothing on screen to say why.

So the parsers are EXECUTED here, on the host, against the exact bytes those
controllers send, and the mapping file is executed too -- including the lines
it cannot understand, because a line that is silently dropped is a line that
never fires.

The code that answers the question is the code that ships: the harness
includes the port's own header.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests" / "host" / "gamepad_selftest.cpp"
HEADER = ROOT / "include" / "rr3" / "gamepad.hpp"
CARD_FILE = ROOT / "card" / "rr3-gamepad.txt"


def find_host_compiler() -> str | None:
    """A host C++ compiler, or None."""
    override = os.environ.get("RR3_HOST_CXX")
    if override:
        found = shutil.which(override) or (override if Path(override).is_file() else None)
        if found:
            return found
    for name in ("g++", "clang++", "c++"):
        found = shutil.which(name)
        if found:
            return found
    return None


def run_harness(compiler: str) -> str:
    """Build and run the host harness. Returns its stdout."""
    with tempfile.TemporaryDirectory(prefix="rr3-gamepad-") as work:
        workdir = Path(work)
        binary = workdir / "gamepad_selftest.exe"
        if os.name != "nt":
            binary = workdir / "gamepad_selftest"
        command = [
            compiler,
            "-std=gnu++20",
            "-Wall",
            "-Wextra",
            "-O1",
            "-I",
            str(ROOT / "include"),
            str(HARNESS),
            "-o",
            str(binary),
        ]
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(
                "the host harness did not compile:\n"
                f"command: {' '.join(command)}\n"
                f"stdout:\n{built.stdout}\nstderr:\n{built.stderr}"
            )
        # THE CARD FILE IS PASSED IN, so that the harness can check the file
        # that actually ships against the built-in defaults.
        ran = subprocess.run([str(binary), str(CARD_FILE)], capture_output=True, text=True)
        if ran.returncode != 0:
            raise AssertionError(
                "the host harness FAILED:\n"
                f"stdout:\n{ran.stdout}\nstderr:\n{ran.stderr}"
            )
        return ran.stdout


def check_static_contracts() -> None:
    """The header and the card file have to agree about the format."""
    header = HEADER.read_text(encoding="utf-8")
    card = CARD_FILE.read_text(encoding="utf-8")

    # Every canonical input the header defines has to be in the header's own
    # name table, and the card file's example mapping has to use names the
    # header knows.
    for name in ("A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START",
                 "HOME", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
                 "L3", "R3", "LSTICK_X", "LSTICK_Y", "RSTICK_X", "RSTICK_Y",
                 "LTRIGGER", "RTRIGGER"):
        assert f'"{name}"' in header, f"the header does not define the input {name}"

    for name in ("BTN_A", "BTN_B", "BTN_X", "BTN_Y", "BTN_L1", "BTN_R1",
                 "BTN_L2", "BTN_R2", "BTN_SELECT", "BTN_START",
                 "BTN_DPAD_CENTER", "AXIS_LX", "AXIS_LY", "AXIS_RX", "AXIS_RY",
                 "AXIS_LT", "AXIS_RT"):
        assert f'"{name}"' in header, f"the header does not define the guest control {name}"

    # The card file is the format the port reads, so it has to document the
    # keys the parser implements.
    assert "autopilot" in card, "the card file does not document 'autopilot'"
    assert "deadzone" in card, "the card file does not document 'deadzone'"
    assert "invert" in card, "the card file does not document 'invert'"
    assert "scale" in card, "the card file does not document 'scale'"
    assert "thresh" in card, "the card file does not document 'thresh'"
    assert "none" in card, "the card file does not document 'none'"
    assert "assist" in card, "the card file does not document 'assist'"

    # THE PORT HAS TO ACTUALLY READ THE FILE.  A mapping file that nothing
    # reads is a decoration, and the user would find out the hard way.
    source = (ROOT / "source" / "guest_input.cpp").read_text(encoding="utf-8")
    assert "rr3-gamepad.txt" in source, (
        "the port never mentions rr3-gamepad.txt, so the mapping file on the "
        "card would be ignored")
    assert "gamepad-map" in source, (
        "the port does not report what it found in the mapping file, so 'the "
        "file is not there' and 'the file says nothing' look the same")
    assert "gamepad-state" in source, (
        "the port does not report the controller state, so a dead controller "
        "and a mis-mapped one look the same")
    for mode in ("kAutopilotManual", "kAutopilotAuto", "kAutopilotAssist"):
        assert mode in source, f"the port does not implement the {mode} mode"

    # THE DEPLOY SCRIPT HAS TO PUT THE FILE ON THE CARD.
    deploy = (ROOT.parent / "tools" / "Deploy-RR3CardScripts.ps1").read_text(
        encoding="utf-8")
    assert "rr3-gamepad.txt" in deploy, (
        "the deploy script does not put rr3-gamepad.txt on the card")


def main() -> None:
    check_static_contracts()

    compiler = find_host_compiler()
    if compiler is None:
        raise AssertionError(
            "no host C++ compiler found (looked for g++, clang++, c++; set "
            "RR3_HOST_CXX to point at one). The parsers have to be EXECUTED by "
            "this test -- a static reading of them proves nothing.")
    print(f"compiler: {compiler}")

    output = run_harness(compiler)
    print(output.rstrip())

    assert "FAIL" not in output, "the host harness reported a failure"
    assert "PASS:" in output, "the host harness did not reach its verdict"
    checks = int(output.rsplit("PASS:", 1)[1].split("checks", 1)[0].strip())
    assert checks >= 40, f"only {checks} checks ran; the harness lost cases"

    print(f"PASS: the gamepad layer decodes and maps as documented ({checks} checks)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
