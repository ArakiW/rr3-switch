#!/usr/bin/env python3
"""The DXT decoder, executed: the rules a wrong decoder breaks silently.

WHAT IS PROVED HERE.  The rival cars in a race render magenta, and the pixels
say the green channel is zeroed -- so the texture is DECODED, but decoded with
the wrong layout.  Telling a wrong CPU decode from a wrong GPU decode needs
both sides measured against the same bytes, and that comparison is worthless if
the CPU decoder is itself wrong.

So the decoder is EXECUTED here, on the host, against hand-computed blocks.
Every expectation comes from the DXT1/DXT5 specification, not from another
decoder's output.  The two rules that get broken most often are covered
explicitly:

  * DXT1's THREE-COLOUR mode: colour0 <= colour1 means index 3 is
    TRANSPARENT, not a fourth colour.
  * DXT5's layout: the colour endpoints are at bytes 12..15 and the colour
    indices at bytes 8..11.

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
HARNESS = ROOT / "tests" / "host" / "dxt_selftest.cpp"
HEADER = ROOT / "include" / "rr3" / "dxt_decode.hpp"


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
    with tempfile.TemporaryDirectory(prefix="rr3-dxt-") as work:
        workdir = Path(work)
        binary = workdir / "dxt_selftest.exe"
        if os.name != "nt":
            binary = workdir / "dxt_selftest"
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
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        if ran.returncode != 0:
            raise AssertionError(
                "the host harness FAILED:\n"
                f"stdout:\n{ran.stdout}\nstderr:\n{ran.stderr}"
            )
        return ran.stdout


def check_static_contracts() -> None:
    """The header has to be the one the port would use."""
    assert HEADER.exists(), f"the decoder header is missing: {HEADER}"
    header = HEADER.read_text(encoding="utf-8")

    # The decoder is only useful if the port can reach it.
    sources = list((ROOT / "source").rglob("*.cpp"))
    assert sources, "the port has no sources to check"

    # The two DXT rules the port would get wrong have to be written down in the
    # header, because the next reader needs them.
    assert "THREE-COLOUR" in header, (
        "the header does not state the DXT1 three-colour rule, which is the "
        "one that turns dark textures into noise")
    assert "12..15" in header and "8..11" in header, (
        "the header does not state where a DXT5 block keeps its colour "
        "endpoints and its colour indices")


def main() -> None:
    check_static_contracts()

    compiler = find_host_compiler()
    if compiler is None:
        raise AssertionError(
            "no host C++ compiler found (looked for g++, clang++, c++; set "
            "RR3_HOST_CXX to point at one). The decoder has to be EXECUTED by "
            "this test -- a static reading of it proves nothing.")
    print(f"compiler: {compiler}")

    output = run_harness(compiler)
    print(output.rstrip())

    assert "FAIL" not in output, "the host harness reported a failure"
    assert "PASS:" in output, "the host harness did not reach its verdict"
    checks = int(output.rsplit("PASS:", 1)[1].split("checks", 1)[0].strip())
    assert checks >= 20, f"only {checks} checks ran; the harness lost cases"

    print(f"PASS: the DXT decoder decodes DXT1 and DXT5 as documented ({checks} checks)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
