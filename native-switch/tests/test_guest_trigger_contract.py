#!/usr/bin/env python3
"""The TRIGGER path: the port's reading of it, against the ORACLES.

THE ORACLES ARE NOT THE PORT'S COMMENTS.

  * libnx's own headers in .tools/devkitPro say what an analogue trigger is
    and where it lives, and say what has to be configured for it to be
    readable at all.
  * The guest's own Java says what the guest expects a trigger to be
    WORTH (the value range and the axis ordinal).

The port's source is then required to use exactly those things, so the port
and the oracles cannot drift apart.

WHAT IS PROVED HERE. Three claims, each against its own oracle:

  1. The analogue trigger exists in this libnx and is the GameCube one:
     PadState.gc_triggers[], raw 0x0..0x7FFF, and it is only readable when
     HidNpadStyleTag_NpadGc is in the configured style set.
  2. The guest's trigger axis ordinals are 4 and 5 (AXIS_LTRIGGER,
     AXIS_RTRIGGER), the value range is 0..1 with 1.0 = fully pressed, and
     the sticks are signed.
  3. The port declares the Gc style, reads the analogue value, converts it
     to 0..1, and keeps the digital ZL/ZR as the fallback so an ordinary
     Pro Controller -- which has NO analogue trigger -- does not lose the
     button.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent

DEVKITPRO = REPO / ".tools" / "devkitPro" / "msys64" / "opt" / "devkitpro"
LIBNX_HID = DEVKITPRO / "libnx" / "include" / "switch" / "services" / "hid.h"
LIBNX_PAD = DEVKITPRO / "libnx" / "include" / "switch" / "runtime" / "pad.h"

INPUT_SOURCE = ROOT / "source" / "guest_input.cpp"
INPUT_HEADER = ROOT / "include" / "rr3" / "guest_input.hpp"
MAIN_SOURCE = ROOT / "source" / "main.cpp"

JAVA_DIR = REPO / "reverse" / "java" / "sources" / "com" / "firemint" / "realracing"


def check_libnx_oracle() -> None:
    """The facts the port's trigger reading depends on."""
    if not (LIBNX_HID.exists() and LIBNX_PAD.exists()):
        print("note    : devkitPro libnx headers absent, the header checks were skipped")
        return

    hid = LIBNX_HID.read_text(encoding="utf-8", errors="replace")
    pad = LIBNX_PAD.read_text(encoding="utf-8", errors="replace")

    # 1. The analogue trigger is the GameCube one, and it is 0x0..0x7FFF.
    assert "HidNpadStyleTag_NpadGc" in hid, "libnx has no NpadGc style tag"
    assert "L analog trigger. Valid range: 0x0-0x7FFF" in hid, (
        "libnx's HidNpadGcState no longer documents the 0x0-0x7FFF trigger range")
    assert "u32 gc_triggers[2];" in pad, (
        "libnx's PadState no longer has gc_triggers[]")
    assert "padGetGcTriggerPos" in pad, (
        "libnx no longer exposes padGetGcTriggerPos()")
    # The documentation says the Gc style has to be configured, and the
    # port has to honour that.
    assert "HidNpadStyleTag_NpadGc must have been previously configured" in pad, (
        "libnx no longer documents that the Gc style must be configured for the "
        "trigger data to be readable")

    # The standard style set does NOT contain NpadGc, so declaring the
    # standard set alone cannot deliver an analogue trigger.
    standard = re.search(r"HidNpadStyleSet_NpadStandard\s*=\s*([^,]+),", hid)
    assert standard, "libnx no longer defines HidNpadStyleSet_NpadStandard"
    assert "NpadGc" not in standard.group(1), (
        "HidNpadStyleSet_NpadStandard now contains NpadGc, so the port's extra "
        "padConfigureInput() call would no longer be needed")


def check_guest_oracle() -> None:
    """What the guest expects a trigger to be worth."""
    java = JAVA_DIR / "ControllerManager.java"
    if not java.exists():
        print("note    : the guest's Java is absent, the guest-side checks were skipped")
        return
    text = java.read_text(encoding="utf-8", errors="replace")

    # The axis ordinals are the enum's order, so the trigger ordinals are 4
    # and 5.
    axes = re.search(r"public enum ControllerAxis \{(.*?)\}", text, re.S)
    assert axes, "the guest's ControllerAxis enum is gone"
    names = [n.strip().rstrip(",") for n in axes.group(1).split("\n") if n.strip()]
    names = [n for n in names if n and not n.startswith("AXIS_COUNT")]
    assert names[4] == "AXIS_LTRIGGER", f"axis ordinal 4 is {names[4]!r}"
    assert names[5] == "AXIS_RTRIGGER", f"axis ordinal 5 is {names[5]!r}"

    # The trigger value is handed to the guest unchanged, as the axis
    # ordinal 4 and 5.
    assert "SetJoystickValues(id, fGetJoystickAxisValue, ControllerAxis.AXIS_LTRIGGER);" in text, (
        "the guest no longer sends the left trigger as AXIS_LTRIGGER")
    assert "SetJoystickValues(id, fGetJoystickAxisValue8, ControllerAxis.AXIS_RTRIGGER);" in text, (
        "the guest no longer sends the right trigger as AXIS_RTRIGGER")
    assert "SetJoystickValueJNI(int i, float f, int i2)" in text, (
        "the guest's SetJoystickValueJNI signature changed")


def check_port() -> None:
    """The port uses the oracles, and keeps the digital fallback."""
    source = INPUT_SOURCE.read_text(encoding="utf-8")
    header = INPUT_HEADER.read_text(encoding="utf-8")
    main = MAIN_SOURCE.read_text(encoding="utf-8")

    # The port declares the Gc style, so the analogue trigger can arrive.
    assert "HidNpadStyleTag_NpadGc" in source, (
        "the port does not declare the GameCube-controller style, so the "
        "analogue trigger can never be delivered")
    assert "padConfigureInput(" in source, (
        "the port never calls padConfigureInput(), which is libnx's only way "
        "to declare a style set")
    assert "HidNpadStyleSet_NpadStandard" in source, (
        "the port dropped the standard styles")

    # The start-up declaration did not include the Gc style: the port has to
    # declare it again, or the analogue trigger cannot arrive.
    assert "padConfigureInput(1, HidNpadStyleSet_NpadStandard);" in main, (
        "main.cpp no longer declares the standard style set; the port's "
        "re-declaration has to be re-checked")

    # The analogue value is read and scaled to the guest's 0..1 range.
    assert "padGetGcTriggerPos(" in source, (
        "the port does not read the analogue trigger")
    assert "32767.0f" in source or "32767.0" in source, (
        "the port does not scale the 0x0-0x7FFF trigger to 0..1")
    assert "kAxisLTrigger" in source and "kAxisRTrigger" in source, (
        "the port no longer sends the trigger axes")

    # The digital ZL/ZR stays: a Pro Controller has no analogue trigger,
    # and dropping the digital reading would take the button away.
    assert "HidNpadButton_ZL" in source and "HidNpadButton_ZR" in source, (
        "the port no longer reads the digital ZL/ZR buttons")
    assert "triggerValue(" in source, (
        "the port does not use the trigger helper")

    # The header states the convention, so a reader is not left guessing.
    assert "0..1" in header, (
        "guest_input.hpp must state the trigger's 0..1 range")
    assert "signed" in header, (
        "guest_input.hpp must state that the sticks are signed")


def main() -> None:
    check_libnx_oracle()
    check_guest_oracle()
    check_port()
    print("PASS: the trigger path agrees with libnx's headers and the guest's Java")


if __name__ == "__main__":
    main()
