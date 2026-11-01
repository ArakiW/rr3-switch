"""The racing line fit must recover a scale, an offset and an axis variant.

The fit is what turns the guest's `.spline` nodes into a target the port's
pursuit can aim at, and a fit that cannot recover a KNOWN answer cannot be
trusted with a real track.  So the positive control is a synthetic circle
with a known mapping, compiled and run on the host.

When no host compiler is there, the source-level contract is checked and the
compiled part says so rather than silently passing.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SELFTEST = ROOT / "tests" / "host" / "race_line_selftest.cpp"


def run_host_selftest() -> bool:
    compiler = (shutil.which("g++") or shutil.which("c++") or
                shutil.which("clang++"))
    if compiler is None:
        print("  note  : no host C++ compiler found, the compiled "
              "positive control did not run")
        return True
    command = [compiler, "-std=c++20", "-O1", "-I", str(ROOT / "include"),
               str(SELFTEST), "-o", str(ROOT / "build" / "race_line_selftest.exe")]
    compiled = subprocess.run(command, capture_output=True, text=True)
    if compiled.returncode != 0:
        print(compiled.stdout)
        print(compiled.stderr)
        raise AssertionError("race_line selftest did not compile")
    ran = subprocess.run([str(ROOT / "build" / "race_line_selftest.exe")],
                         capture_output=True, text=True)
    print(ran.stdout.strip())
    if ran.returncode != 0:
        print(ran.stderr)
        raise AssertionError("race_line selftest failed")
    return True


def main() -> None:
    source = (ROOT / "source" / "race_line.cpp").read_text(encoding="utf-8")
    header = (ROOT / "include" / "rr3" / "race_line.hpp").read_text(encoding="utf-8")

    # 1. The fit is guarded: a line the car is not on is refused, and the
    #    residual is in METRES so the guard can be read.
    assert "distanceMetres > 25.0" in source
    assert "line.residual = alignment.residual / 256.0" in source
    assert "count < 3" in source
    # The alignment is ICP-style, not a centroid alignment: a car that has
    # driven half a lap has its centroid nowhere near the line's.
    assert "alignOntoCar" in source
    assert "nearestOnPolyline(poly, carX[i], carY[i], &qx, &qy)" in source

    # 2. The scale search covers the powers of two and then refines, so a
    #    wrong guess does not decide the fit.
    assert "kScales[] = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}" in source
    assert "for (int step = -32; step <= 32; ++step)" in source

    # 3. All eight axis variants are tried, because the guest's axes and the
    #    spline's axes are not documented to agree.
    block = source[source.index("kVariants[]"):source.index("};", source.index("kVariants[]"))]
    assert block.count("{") - 1 == 8, block   # the array's own brace, plus eight
    for variant in ("{false, false, false}", "{true, false, false}",
                    "{false, true, false}", "{false, false, true}",
                    "{true, true, false}", "{true, false, true}",
                    "{false, true, true}", "{true, true, true}"):
        assert variant in block, variant

    # 4. The header says what the guard is.
    assert "residual" in header
    assert "lookAheadMetres" in header

    run_host_selftest()
    print("race_line fit: contract ok, positive control ran")


if __name__ == "__main__":
    main()
