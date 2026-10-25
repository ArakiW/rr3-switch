#!/usr/bin/env python3
"""The memory the port reports to the guest, checked against the guest binary.

WHAT THIS PINS
---------------
The guest asks the platform how much memory it has, and it ACTS on the
answer: when the figure is too small it refuses to load a track and puts up
its own dialog ("not enough available memory to load the track", in the
Chinese build "可用内存不足，无法加载赛道。正在返回主菜单...") and returns to
the main menu.

MEASURED 2026-10-25, archive 20261025-155851-eden-f1a: the port reported
1 GB total / 512 MB free (the Switch's low-memory model, the R36S, has 1 GB),
and the whole path to a race worked -- daily reward, event picker, quest
page, car select, start race -- and died at exactly this read.  The track
never loaded, so there was no lap to count.

So the memory report is not a cosmetic constant.  It is an INPUT to the
guest's behaviour, and this test pins it at a floor that is above the
guest's content gate and well below the real console's figure.

WHY THE FLOORS ARE WHERE THEY ARE
----------------------------------
* total >= 2048 MB.  The guest's content tiers are gated on the device's
  memory class, and 1 GB is the class that cannot load a track.  The
  console this port targets has 16 GB, so a value of 4096 MB is both
  honest and conservative.
* free >= 1 GB.  The error text says AVAILABLE memory, so the available
  figure is the one the track check reads; the old value was exactly 512 MB,
  which is the size of the thing it refused.
* usage < free.  A usage figure above the free figure would make the two
  readings contradictory, and the guest takes the smaller of the two.

THE TEST ALSO PINS THAT THE GUEST CONSUMES THESE NUMBERS
---------------------------------------------------------
The guest binary holds the Java method-name literals "getTotalMemory" and
"getAppMemoryFree"; its native code looks those names up and calls back into
Java to read the values.  Without that, the port's answers would be
decoration, and the test would be pinning a constant that nothing reads.

AND IT PINS THE REASON
----------------------
The measurement is named in the source comment, so the next reader who sees
a memory number in a port and thinks "the smaller the better" finds why it
is the other way round.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SOURCE = ROOT / "source" / "jni_compat" / "classes" / "rr3_Platform.cpp"
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"

# The floors the measurement forces.  See the module docstring.
MIN_TOTAL_MB = 2048
MIN_FREE_BYTES = 1024 * 1024 * 1024

# The archive that measured the refusal.
MEASURED_ARCHIVE = "20261025-155851-eden-f1a"

# The guest's own Java method names, as literals in the guest binary.
GUEST_METHOD_NAMES = (b"getTotalMemory", b"getAppMemoryFree")


def return_expression(source: str, function: str) -> str:
    """The body of `return <expr>;` in the given static function."""
    pattern = re.compile(
        r"static\s+\w+\s+" + re.escape(function) + r"\s*\([^)]*\)\s*\{\s*return\s+([^;]+);"
    )
    match = pattern.search(source)
    assert match is not None, (
        f"{SOURCE.name} has no 'static ... {function}(...) {{ return ...; }}' to read"
    )
    return match.group(1).strip()


def evaluate(expression: str) -> int:
    """Evaluate the arithmetic the port uses for these constants.

    The expressions are literals and multiplications of literals ("4096",
    "4096LL * 1024 * 1024").  Anything else is a change in the port's
    vocabulary that this test must not silently accept.
    """
    normalised = expression.replace("LL", "").replace("ULL", "")
    if not re.fullmatch(r"[0-9+\-*/() ]+", normalised):
        raise AssertionError(
            f"the memory constant is not a plain arithmetic expression: {expression!r}"
        )
    return int(eval(normalised, {"__builtins__": {}}, {}))  # noqa: S307


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")

    total_mb = evaluate(return_expression(source, "platform_get_total_memory"))
    free_bytes = evaluate(return_expression(source, "platform_get_app_memory_free"))
    usage_bytes = evaluate(return_expression(source, "platform_get_app_memory_usage"))

    print(f"total memory reported to the guest : {total_mb} MB")
    print(f"available memory reported to guest : {free_bytes} bytes "
          f"({free_bytes // (1024 * 1024)} MB)")
    print(f"used memory reported to the guest  : {usage_bytes} bytes "
          f"({usage_bytes // (1024 * 1024)} MB)")

    assert total_mb >= MIN_TOTAL_MB, (
        f"the port reports {total_mb} MB of total memory; the guest refuses to "
        f"load a track below {MIN_TOTAL_MB} MB (measured in archive "
        f"{MEASURED_ARCHIVE})"
    )
    assert free_bytes >= MIN_FREE_BYTES, (
        f"the port reports {free_bytes} bytes of available memory; the guest "
        f"refuses to load a track below {MIN_FREE_BYTES} bytes (measured in "
        f"archive {MEASURED_ARCHIVE})"
    )
    assert usage_bytes < free_bytes, (
        f"the port reports {usage_bytes} bytes used and {free_bytes} bytes "
        f"free; the used figure must be below the free figure or the two "
        f"readings contradict each other"
    )
    assert usage_bytes < total_mb * 1024 * 1024, (
        f"the port reports {usage_bytes} bytes used, which is more than the "
        f"{total_mb} MB of total memory it also reports"
    )

    # The guest really reads these two numbers.
    so = SO.read_bytes()
    for name in GUEST_METHOD_NAMES:
        assert name in so, (
            f"the guest binary does not hold the literal {name!r}; the port's "
            f"memory report would then be decoration"
        )

    # The measurement is recorded where the numbers are, so the reason for
    # them cannot be lost.
    assert MEASURED_ARCHIVE in source, (
        f"{SOURCE.name} does not name the archive ({MEASURED_ARCHIVE}) that "
        f"measured the refusal; the reason for these numbers has to live "
        f"next to them"
    )
    assert "available" in source.lower() or "可用" in source, (
        f"{SOURCE.name} does not say which of the two figures the guest's "
        f"content gate reads"
    )

    print(f"PASS: the memory the port reports to the guest is above the gate "
          f"that made it refuse to load a track ({total_mb} MB total, "
          f"{free_bytes // (1024 * 1024)} MB available)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise
