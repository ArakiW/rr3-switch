#!/usr/bin/env python3
"""Static contract for restoring hbloader memory state before NRO return."""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    header = (root / "include" / "rr3" / "elf_preflight.hpp").read_text(encoding="utf-8")
    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")

    assert "struct ElfLoaderUnload" in header
    assert "ElfLoaderUnload unloadLinkElfSet();" in header
    cleanup = linker[linker.index("ElfLoaderUnload closeSegmentedMappingsDetailed") :]
    cleanup = cleanup[: cleanup.index("bool mapNormalSegmentedRange")]
    assert "reverseIndex-- > 0" in cleanup
    assert "svcUnmapProcessCodeMemory" in cleanup
    assert cleanup.index("svcUnmapProcessCodeMemory") < cleanup.index("std::free(image.processBacking)")
    assert "if (!image.processMapped)" in cleanup

    y_branch = main_source.index("if ((down & HidNpadButton_Y) != 0)")
    handoff_lambda = main_source.index("prepareCleanHandoff")
    handoff_unload = main_source.index("unloadLinkElfSet", handoff_lambda)
    y_cleanup = main_source.index("prepareCleanHandoff()", y_branch)
    y_schedule = main_source.index("scheduleSelfLoad", y_cleanup)
    assert handoff_lambda < handoff_unload < y_branch < y_cleanup < y_schedule
    plus_branch = main_source.index("if ((down & HidNpadButton_Plus) != 0)")
    plus_cleanup = main_source.index("prepareCleanHandoff(&dirtyWorkers)", plus_branch)
    assert plus_branch < plus_cleanup
    assert 'exitRequested = prepareCleanHandoff(&dirtyWorkers);' in main_source
    assert "skipUnloadAfterExit" in main_source
    assert "handoff-dirty-exit" in main_source
    assert 'guestPthreadLiveCount' in main_source
    print("PASS: reverse guest unmap gates Y handoff and PLUS exit")


if __name__ == "__main__":
    main()
