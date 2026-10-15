#!/usr/bin/env python3
"""Validate FAT32-safe RR3 data-volume routing and ABI path interception."""

from collections import Counter
import importlib.util
from pathlib import Path
import zipfile


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    project = root.parent
    module_path = project / "tools" / "prepare_rr3_external_data.py"
    spec = importlib.util.spec_from_file_location("rr3_data_builder", module_path)
    assert spec and spec.loader
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)

    # Globbed, never hardcoded. The previous literal was mojibake -- the UTF-8
    # bytes of "数据包" decoded as GBK, with one byte collapsed to "?" -- so it
    # raised OSError at ZipFile() before any project source was read, and the
    # suite reported 10/11 with no hint that the asset had moved. The dated
    # suffix also changes whenever the data volumes are regenerated, so a
    # literal would go stale a second time. Fail loudly on anything but a
    # single match instead of silently testing the wrong archive.
    candidates = sorted(project.glob("DXT_*.zip"))
    if len(candidates) != 1:
        raise SystemExit(
            f"expected exactly one DXT_*.zip under {project}, found "
            f"{len(candidates)}: {[c.name for c in candidates]}"
        )
    archive = candidates[0]
    totals: Counter[str] = Counter()
    counts: Counter[str] = Counter()
    names: set[str] = set()
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            if item.is_dir():
                continue
            volume = builder.classify(item.filename)
            assert volume in builder.VOLUMES
            totals[volume] += item.file_size
            counts[volume] += 1
            names.add(item.filename.rstrip("/"))

    assert sum(counts.values()) == 42_541
    assert sum(totals.values()) == 9_630_901_568
    assert all(0 < totals[name] < builder.BUILD_ROMFS_MAX_FILE for name in builder.VOLUMES)
    for volume, sentinel in builder.SENTINELS.items():
        if sentinel.endswith("/"):
            assert any(name.startswith(sentinel) for name in names), (volume, sentinel)
        else:
            assert sentinel in names, (volume, sentinel)

    source = (root / "source" / "guest_filesystem.cpp").read_text(encoding="utf-8")
    resolver = (root / "source" / "symbol_resolver.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    for image in (
        "core.romfs",
        "gui-hd.romfs",
        "gui-sd.romfs",
        "tracks.romfs",
        "vehicles-classic.romfs",
        "vehicles-mid.romfs",
        "vehicles-modern.romfs",
    ):
        assert image in source
    assert "romfsMountFromFsdev" in source
    assert "data/loose" in source
    assert "looseFallback" in source
    assert "guestFilesystemSelfTest" in source
    # Absolute sdmc: paths under getAppPath must remap into the seven volumes
    # instead of FAT passthrough (round 16: CC_Data hit SD, gui_assets never).
    assert "mapAppRootToContent" in source
    assert "kAppRoot" in source
    assert "files/.depot/" in source
    assert "0x1fff" in main_source
    assert "rr3::unmountExternalData();" in main_source
    for symbol in (
        "access", "fopen", "open", "__open_2", "stat", "lstat", "statfs",
        "statvfs", "mkdir", "opendir", "pathconf", "readlink", "realpath",
        "remove", "rename", "unlink", "link", "symlink",
    ):
        assert f'RR3_SHIM("{symbol}",' in resolver, symbol
    print(
        "PASS: 42,541 files / 9,630,901,568 bytes route into seven FAT32-safe "
        "read-only volumes with complete path ABI interception"
    )


if __name__ == "__main__":
    main()
