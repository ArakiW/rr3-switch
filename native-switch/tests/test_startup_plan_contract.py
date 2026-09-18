#!/usr/bin/env python3
"""Reference and source contracts for guarded dependency initialization."""

from pathlib import Path
import sys


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / "tests"))
    from reference_startup_plan import inspect

    paths = [
        root / "romfs" / "libRealRacing3.so",
        root / "romfs" / "libNimble.so",
        root / "romfs" / "libfmodex.so",
        root / "romfs" / "libc++_shared.so",
    ]
    plan = inspect(paths)
    assert plan["dependency_order"] == [
        "libc++_shared.so",
        "libNimble.so",
        "libfmodex.so",
        "libRealRacing3.so",
    ]
    assert plan["dependency_cycles"] == 0
    assert plan["dependency_constructor_count"] == 3
    assert plan["main_constructor_count"] == 181
    assert all(
        image["constructor_count"] == image["valid_constructors"]
        for image in plan["images"]
    )
    assert all(
        image["finalizer_count"] == image["valid_finalizers"]
        for image in plan["images"]
    )

    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    recovery = (root / "source" / "recovery_guard.cpp").read_text(encoding="utf-8")
    assert "initializeGuestDependencies" in linker
    assert "isMainGameImage(image)" in linker
    assert "finalizeInitializedGuestImages" in linker
    assert "reverseOrder-- > 0" in linker
    auto_argument = main_source.index('"--rr3-auto-deps"')
    snapshot = main_source.index("armDependencyInitialization", auto_argument)
    invoke = main_source.index("initializeGuestDependencies", snapshot)
    automatic_call = main_source.index("if (automaticDependencyTest || automaticGuestBoot)", invoke)
    zr = main_source.index("HidNpadButton_ZR", automatic_call)
    assert auto_argument < snapshot < invoke < automatic_call < zr
    assert "fsync(fileno(file))" in recovery
    print("PASS: dependency order 1/0/2, main 181 gated, one-write snapshot before call")


if __name__ == "__main__":
    main()
