#!/usr/bin/env python3
"""The scripted gamepad's SCRIPT FORMAT, pinned by executing the real parser.

WHAT IS PROVED HERE. The port's scripted pad is driven by a text file on the
card, and the format of that file is the thing a script writer has to get
right: a line the parser silently rejects is a line that never fires, and
nothing on screen says so. The interesting property of the format is not
that it is written down -- it is that the WRITTEN-DOWN format is the format
that PARSES.

So the test does two things:

  1. It EXTRACTS the real parseFloat() and parsePadLine() out of the port's
     source, compiles them with the HOST compiler and RUNS them against
     sample scripts. The code that answers the question is the code that
     ships; a comment that agrees with the code proves nothing about either.

  2. It CROSS-CHECKS the key list in the parser against the key list in
     native-switch/include/rr3/guest_input.hpp, and requires the two sets to
     be equal. A documented key the parser does not implement, or a key the
     parser implements and the header does not mention, both fail here.

THE HISTORY THIS PINS. The format used by the earlier experiments was

    [at <frame>] lx=<float> rx=<float> b=<hex mask>

and it is STILL ACCEPTED: the old scripts have to keep meaning what they
meant, and the test runs one of them verbatim. The format also covers all
six of the guest's ControllerAxis ordinals, which is what makes an analogue
trigger expressible at all.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PARSER_SOURCE = ROOT / "source" / "jni_compat" / "classes" / "rr3_InputDoors.cpp"
INPUT_HEADER = ROOT / "include" / "rr3" / "guest_input.hpp"
INPUT_SOURCE = ROOT / "source" / "guest_input.cpp"
HARNESS = ROOT / "tests" / "guest_pad_script_test.cpp"

# The guest's own ControllerAxis ordinals (ControllerManager.java). The
# script's keys are named after them.
GUEST_AXES = {
    "lx": "AXIS_LTHUMB_X",
    "ly": "AXIS_LTHUMB_Y",
    "rx": "AXIS_RTHUMB_X",
    "ry": "AXIS_RTHUMB_Y",
    "lt": "AXIS_LTRIGGER",
    "rt": "AXIS_RTRIGGER",
}


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


def extract_function(source: str, signature: str) -> str:
    """The text of one function, from its signature to its closing brace."""
    start = source.index(signature)
    end = source.index("\n}\n", start) + len("\n}\n")
    return source[start:end]


def parser_keys(source: str) -> set[str]:
    """The keys the parser actually implements."""
    body = extract_function(source, "bool parsePadLine(")
    return set(re.findall(r'std::strcmp\(keyBuf,\s*"(\w+)"\)', body))


def documented_keys(header: str) -> set[str]:
    """The keys the header documents, read out of the format line."""
    line = None
    for candidate in header.splitlines():
        if "[at <frame>]" in candidate and "lx=" in candidate:
            line = candidate
            break
    assert line is not None, (
        "guest_input.hpp does not document a '[at <frame>] ... lx=...' format "
        "line, so the documented format cannot be checked against the parser")
    return set(re.findall(r"(\w+)=<", line))


def run_harness(compiler: str) -> str:
    """Build and run the host harness. Returns its stdout."""
    source = PARSER_SOURCE.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="rr3-pad-script-") as work:
        workdir = Path(work)
        (workdir / "pad_parser.inc").write_text(
            extract_function(source, "float parseFloat(")
            + "\n"
            + extract_function(source, "bool parsePadLine("),
            encoding="utf-8",
        )
        binary = workdir / "guest_pad_script_test.exe"
        if os.name != "nt":
            binary = workdir / "guest_pad_script_test"
        command = [
            compiler,
            "-std=gnu++20",
            "-Wall",
            "-Wextra",
            "-O1",
            "-I",
            str(workdir),
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
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        if ran.returncode != 0:
            raise AssertionError(
                "the host harness FAILED:\n"
                f"stdout:\n{ran.stdout}\nstderr:\n{ran.stderr}"
            )
        return ran.stdout


def check_static_contracts() -> None:
    source = PARSER_SOURCE.read_text(encoding="utf-8")
    header = INPUT_HEADER.read_text(encoding="utf-8")
    input_source = INPUT_SOURCE.read_text(encoding="utf-8")

    implemented = parser_keys(source)
    documented = documented_keys(header)

    expected = set(GUEST_AXES) | {"b"}
    assert implemented == expected, (
        f"the parser implements {sorted(implemented)}, expected {sorted(expected)}")
    assert documented == expected, (
        f"guest_input.hpp documents {sorted(documented)}, expected "
        f"{sorted(expected)}")
    assert implemented == documented, (
        "the documented format and the implemented format differ: parser "
        f"{sorted(implemented)} vs header {sorted(documented)}")

    # The header has to say the trigger is 0..1 and the sticks are signed,
    # because that is the convention the guest's Java uses and it is not
    # guessable.
    assert "0..1" in header, (
        "the header must state the trigger's 0..1 range")
    assert "signed" in header, (
        "the header must state that the sticks are signed")

    # The source has to carry the same format, because the source is what a
    # reader of the bridge is reading.
    assert "[at <frame>]" in input_source, (
        "guest_input.cpp does not document the script format")
    for key in expected:
        assert f"{key}=" in input_source, (
            f"guest_input.cpp does not document the '{key}=' key")

    # The script's control file lives where the port reads it.
    assert "sdmc:/switch/rr3-native-rehost/rr3-pad.txt" in source, (
        "the port no longer reads the pad script from the card")


def main() -> None:
    check_static_contracts()

    compiler = find_host_compiler()
    if compiler is None:
        raise AssertionError(
            "no host C++ compiler found (looked for g++, clang++, c++; set "
            "RR3_HOST_CXX to point at one). The parser has to be EXECUTED by "
            "this test -- a static reading of it proves nothing.")
    print(f"compiler: {compiler}")

    output = run_harness(compiler)
    print(output.rstrip())

    assert "FAIL" not in output, "the host harness reported a failure"
    assert "PASS:" in output, "the host harness did not reach its verdict"
    checks = int(output.rsplit("PASS:", 1)[1].split("checks", 1)[0].strip())
    assert checks >= 30, f"only {checks} checks ran; the harness lost cases"

    print(f"PASS: the scripted gamepad format parses as documented ({checks} checks)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
