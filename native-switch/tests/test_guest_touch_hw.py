#!/usr/bin/env python3
"""The hardware touch path: the conversion is EXECUTED, not just inspected.

WHAT IS PROVED HERE. The interesting part of the Switch's touchscreen wiring
is a pure function -- "this frame's digitizer samples" -> "these guest touch
events" -- and the interesting decisions in it are the pointer id, the
stale-pointer release and the coordinate mapping. This test COMPILES that
function with the HOST compiler and RUNS it against synthetic samples, so the
answers come from the code that ships rather than from a comment about it.

WHAT IS NOT PROVED HERE. Nothing about the physical digitizer: no finger is
touched, no digitizer is read, and the guest is not involved. The emulator
run (tools/Test-OnEden.ps1 -Tag touchhw) is what says whether the platform
reports a touchscreen at all.

The conversion lives in native-switch/include/rr3/guest_touch_convert.hpp on
purpose: it depends on neither libnx nor JNI, so this file can include it
unchanged and the host compiler can build it.

The static half of the test checks the wiring -- that the port reads the
digitizer through the libnx API that EXISTS in this toolchain, that the
mapping is named rather than buried, that the file control still owns the
guest's touch path -- against the ORACLE (the devkitPro headers in
.tools/devkitPro and the guest's own Java), not against the port's comments.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

CONVERT_HEADER = ROOT / "include" / "rr3" / "guest_touch_convert.hpp"
HW_HEADER = ROOT / "include" / "rr3" / "guest_touch_hw.hpp"
HW_SOURCE = ROOT / "source" / "guest_touch_hw.cpp"
TOUCH_HEADER = ROOT / "include" / "rr3" / "guest_touch.hpp"
TOUCH_SOURCE = ROOT / "source" / "guest_touch.cpp"
INPUT_SOURCE = ROOT / "source" / "guest_input.cpp"
HARNESS = ROOT / "tests" / "guest_touch_convert_test.cpp"

DEVKITPRO = REPO / ".tools" / "devkitPro" / "msys64" / "opt" / "devkitpro"
LIBNX_HID = DEVKITPRO / "libnx" / "include" / "switch" / "services" / "hid.h"

JAVA_DIR = REPO / "reverse" / "java" / "sources" / "com" / "firemint" / "realracing"


def find_host_compiler() -> str | None:
    """A host C++ compiler, or None.

    The environment variable wins so that a machine without a compiler on
    PATH can still run this test.
    """
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
    with tempfile.TemporaryDirectory(prefix="rr3-touch-hw-") as work:
        binary = Path(work) / "guest_touch_convert_test.exe"
        if os.name != "nt":
            binary = Path(work) / "guest_touch_convert_test"
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
    convert = CONVERT_HEADER.read_text(encoding="utf-8")
    hw_header = HW_HEADER.read_text(encoding="utf-8")
    hw_source = HW_SOURCE.read_text(encoding="utf-8")
    touch_header = TOUCH_HEADER.read_text(encoding="utf-8")
    touch_source = TOUCH_SOURCE.read_text(encoding="utf-8")
    input_source = INPUT_SOURCE.read_text(encoding="utf-8")

    # ---- the libnx API that EXISTS in this toolchain ----------------------
    #
    # The brief this test was written against named hidInitializeTouch(),
    # hidTouchRead() and hidTouchCount(). libnx 4.12 has NONE of them; the
    # API it has is hidInitializeTouchScreen() and hidGetTouchScreenStates().
    # The port has to use the API that exists, so the test checks the real
    # one is used and the three that do not exist are not.
    if LIBNX_HID.exists():
        hid = LIBNX_HID.read_text(encoding="utf-8", errors="replace")
        assert "hidGetTouchScreenStates" in hid, (
            "libnx in this toolchain has no hidGetTouchScreenStates()")
        assert "hidInitializeTouchScreen" in hid, (
            "libnx in this toolchain has no hidInitializeTouchScreen()")
        assert "HidTouchState" in hid and "finger_id" in hid, (
            "libnx's HidTouchState has no finger_id")
        assert "HidTouchAttribute_Start" in hid and "HidTouchAttribute_End" in hid, (
            "libnx's HidTouchState has no Start/End attributes")
        for absent in ("hidTouchRead", "hidTouchCount", "hidInitializeTouch("):
            assert absent not in hid, (
                f"{absent} exists in this libnx; the port's assumption about "
                "the API needs re-checking")
    else:
        print("note    : devkitPro libnx headers absent, the API check was skipped")

    assert "hidGetTouchScreenStates" in hw_source, (
        "the hardware path does not read the digitizer")
    assert "HidTouchAttribute_Start" in hw_source, (
        "the hardware path ignores the digitizer's Start attribute")
    assert "HidTouchAttribute_End" in hw_source, (
        "the hardware path ignores the digitizer's End attribute")
    assert "finger_id" in hw_source, (
        "the hardware path ignores the digitizer's finger id")

    # ---- the conversion is factored out and is pure ----------------------
    assert "rr3PlanTouchFrame" in convert, (
        "the conversion is not factored out of the hardware path")
    assert "rr3PlanTouchFrame" in hw_source, (
        "the hardware path does not use the factored-out conversion")
    # The pure header must be self-contained: an #include of libnx, or of
    # anything else, would tie it to the Switch toolchain and the host test
    # could not build it.
    includes = [
        line.strip()
        for line in convert.splitlines()
        if line.strip().startswith("#include")
    ]
    assert not includes, (
        f"the pure conversion header includes {includes}; it has to be "
        "self-contained so the host compiler can build it")
    for forbidden in ("JNIEnv", "HidTouchState"):
        assert forbidden not in convert, (
            f"the pure conversion header must not depend on {forbidden}")

    # ---- the mapping is NAMED, not buried --------------------------------
    assert "sourceWidth" in convert and "viewWidth" in convert, (
        "the coordinate mapping is not named")
    assert "1280" in hw_source and "720" in hw_source, (
        "the digitizer/view sizes are not stated")

    # ---- the guest's own Java is the oracle for the event shape ----------
    activity = JAVA_DIR / "MainActivity.java"
    glview = JAVA_DIR / "GLView.java"
    if activity.exists() and glview.exists():
        activity_text = activity.read_text(encoding="utf-8", errors="replace")
        glview_text = glview.read_text(encoding="utf-8", errors="replace")
        assert "public native void onTouchBeginJNI(int i, float f, float f2)" in activity_text
        assert "public native void onTouchMoveJNI(int i, float f, float f2)" in activity_text
        assert "public native void onTouchEndJNI(int i, float f, float f2, boolean z)" in activity_text
        assert "motionEvent.getPointerId(" in glview_text, (
            "the guest's pointer id comes from the MotionEvent")
        assert "motionEvent.getActionMasked() == 1" in glview_text, (
            "the guest's 'last' flag is the ACTION_UP test")
    else:
        print("note    : the guest's Java is absent, the event-shape check was skipped")

    # ---- the file control still owns the guest's touch path --------------
    assert "guestTouchControlFilePresent" in touch_header, (
        "the file control does not report its own presence")
    assert "guestTouchControlFilePresent" in touch_source, (
        "the file control does not track its own presence")
    assert "guestTouchControlFilePresent" in hw_source, (
        "the hardware path ignores the file control's presence")
    assert "injectGuestTouchEvent" in touch_header, (
        "there is no shared injection primitive")
    assert "injectGuestTouchEvent" in touch_source, (
        "the shared injection primitive is not implemented")
    assert "injectGuestTouchEvent" in hw_source, (
        "the hardware path does not use the shared injection primitive")
    assert "pumpGuestTouchHardware" in input_source, (
        "the hardware path is not pumped from the frame loop")

    # ---- the file control's own contract is untouched ---------------------
    assert "sdmc:/switch/rr3-native-rehost/rr3-touch.txt" in touch_source
    assert "[at <frame>] <x> <y> <action> [pointerId]" in touch_header or \
        "[at <frame>] <x> <y> <action> [pointerId]" in touch_source
    for export in ("Java_com_firemint_realracing_MainActivity_onTouchBeginJNI",
                   "Java_com_firemint_realracing_MainActivity_onTouchMoveJNI",
                   "Java_com_firemint_realracing_MainActivity_onTouchEndJNI"):
        assert export in touch_source, f"{export} is no longer resolved"


def main() -> None:
    check_static_contracts()

    compiler = find_host_compiler()
    if compiler is None:
        raise AssertionError(
            "no host C++ compiler found (looked for g++, clang++, c++; set "
            "RR3_HOST_CXX to point at one). The conversion has to be "
            "EXECUTED by this test -- a static reading of it proves nothing.")
    print(f"compiler: {compiler}")

    output = run_harness(compiler)
    print(output.rstrip())

    assert "FAIL" not in output, "the host harness reported a failure"
    assert "PASS:" in output, "the host harness did not reach its verdict"
    checks = int(output.rsplit("PASS:", 1)[1].split("checks", 1)[0].strip())
    assert checks >= 40, f"only {checks} checks ran; the harness lost cases"

    print(f"PASS: hardware touch conversion executed ({checks} checks)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
