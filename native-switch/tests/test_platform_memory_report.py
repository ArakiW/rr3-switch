#!/usr/bin/env python3
"""The memory the guest actually reads, and where it comes from.

WHAT THIS PINS
--------------
The guest asks the platform how much memory it has.  It is not a cosmetic
question: in its device-tier code the guest compares getTotalMemory()
against 601 MB and forces the graphics/texture tier down to 0 when the value
is below that.  (Guest .text: the comparison is `cmp w19,#0x259` right after
the getTotalMemory() call, and the value is logged as "Available memory:
%dMb".)  A second read, in the font-atlas code, compares against 513 MB.

So the port's answer has to be ABOVE 601, and this test pins that.

WHERE THE ANSWER COMES FROM -- AND WHERE IT DOES NOT
------------------------------------------------------
The port has TWO places that could answer a Platform memory getter:

  1. native-switch/source/jni_compat/classes/rr3_Platform.cpp registers
     platform_get_total_memory / platform_get_app_memory_free /
     platform_get_app_memory_usage as MANAGED METHODS;
  2. native-switch/source/jni_bridge.cpp has platformIntForMethod(), which
     answers the guest's int-returning static call (JNI slot 130,
     jniCallStaticIntMethodV) BY NAME and never consults the managed
     method table for a name it recognises.

The guest's getTotalMemory() is an int, so it is answered by (2).
MEASURED 2026-10-25, archive 20261025-162550-eden-f1b: the guest printed
"Available memory: 2146435072Mb", which is exactly platformIntForMethod()'s
`2047 * 1024 * 1024` -- and NOT the value written in rr3_Platform.cpp.  The
numbers in rr3_Platform.cpp are therefore not what the guest reads, and a
change to them changes nothing; the comment there says so.

The guest's getAppMemoryFree() and getAppMemoryUsage() are longs (JNI slot
133, CallStaticLongMethodV).  That slot is NOT wired in the port's JNI
table, so the guest sees 0 for both.  The guest uses them for telemetry
only -- there is no comparison anywhere in the image with either of them on
the right-hand side -- so nothing gates on them, and this test does not
pretend otherwise.

WHY THE VALUE IS DELIBERATELY ABSURD
------------------------------------
The same read also sizes the font atlas pages, so LOWERING it towards a
"realistic" number is a risk, not a fix.  The value is kept far above both
thresholds, and the tier is never forced down.

WHAT ELSE THIS CHECKS
---------------------
That the guest binary really holds the two Java method-name literals the
native side calls back into ("getTotalMemory", "getAppMemoryFree"), so the
port's answers are not decoration; and that rr3_Platform.cpp carries the
note saying which path the guest actually uses -- otherwise the next
reader edits the wrong number.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
BRIDGE = ROOT / "source" / "jni_bridge.cpp"
PLATFORM = ROOT / "source" / "jni_compat" / "classes" / "rr3_Platform.cpp"
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"

# The guest's own thresholds, in MB. From the guest's .text (cmp w19,#0x259
# and cmp w22,#0x201); see the module docstring.
GUEST_TIER_FLOOR_MB = 601
GUEST_FONT_FLOOR_MB = 513

GUEST_METHOD_NAMES = (b"getTotalMemory", b"getAppMemoryFree")


def case_value(source: str, function: str, name: str) -> int:
    """The integer a `if (rawEquals(name, "<name>")) return <expr>;` returns."""
    pattern = re.compile(
        r'rawEquals\(name,\s*"' + re.escape(name) + r'"\)\s*\)\s*return\s+([^;]+);'
    )
    match = pattern.search(source)
    assert match is not None, (
        f"{function}() has no case for {name!r}; the guest's memory read "
        f"would fall through to 0"
    )
    expression = match.group(1).strip().replace("LL", "").replace("ULL", "")
    if not re.fullmatch(r"[0-9+\-*/() ]+", expression):
        raise AssertionError(
            f"the value for {name!r} is not a plain arithmetic expression: {expression!r}"
        )
    return int(eval(expression, {"__builtins__": {}}, {}))  # noqa: S307


def main() -> int:
    bridge = BRIDGE.read_text(encoding="utf-8")

    total_mb = case_value(bridge, "platformIntForMethod", "getTotalMemory")
    print(f"getTotalMemory() the guest sees : {total_mb} (the guest compares "
          f"it against {GUEST_TIER_FLOOR_MB} MB)")
    assert total_mb >= GUEST_TIER_FLOOR_MB, (
        f"platformIntForMethod() answers getTotalMemory() with {total_mb}, "
        f"which is below the guest's {GUEST_TIER_FLOOR_MB} MB tier threshold: "
        f"the guest would force the graphics/texture tier down to 0"
    )
    assert total_mb >= GUEST_FONT_FLOOR_MB, (
        f"platformIntForMethod() answers getTotalMemory() with {total_mb}, "
        f"which is below the guest's {GUEST_FONT_FLOOR_MB} MB font-atlas "
        f"threshold"
    )

    # The guest's own log must be able to show this number, which is the
    # cheapest way for the next run to confirm which path answered.
    assert "Available memory" in Path(SO).read_bytes().decode("latin-1"), (
        "the guest binary no longer holds the 'Available memory: %dMb' string; "
        "the read this test pins may have moved"
    )

    # The two names the native side calls back into must exist in the guest.
    so = SO.read_bytes()
    for name in GUEST_METHOD_NAMES:
        assert name in so, (
            f"the guest binary does not hold the literal {name!r}; the port's "
            f"memory answers would be decoration"
        )

    # The managed-method copy of these getters is not the path the guest
    # uses, and the file has to say so.
    platform = PLATFORM.read_text(encoding="utf-8")
    assert "DOES NOT READ THIS ONE" in platform or "NOT what the guest reads" in platform, (
        f"{PLATFORM.name} does not say that the guest does not read these "
        f"functions; the next reader would edit the wrong number"
    )
    assert "20261025-162550-eden-f1b" in platform, (
        f"{PLATFORM.name} does not name the archive that measured which path "
        f"answers the guest"
    )

    print(f"PASS: the guest's memory read is answered by platformIntForMethod() "
          f"({total_mb}), far above the {GUEST_TIER_FLOOR_MB} MB tier threshold, "
          f"and the managed-method copy says it is not the path")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
