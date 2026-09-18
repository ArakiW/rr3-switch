#!/usr/bin/env python3
"""Verify RR3 external RomFS hashes and sentinel paths without mounting them."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


INVALID = 0xFFFFFFFF


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


class RomFS:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.stream = path.open("rb")
        header = self.stream.read(80)
        if len(header) != 80:
            raise ValueError(f"short RomFS header: {path}")
        fields = struct.unpack("<10Q", header)
        if fields[0] != 80:
            raise ValueError(f"unexpected RomFS header size {fields[0]}: {path}")
        self.dir_table = fields[3]
        self.file_table = fields[7]

    def close(self) -> None:
        self.stream.close()

    def directory(self, offset: int) -> tuple[int, int, str]:
        self.stream.seek(self.dir_table + offset)
        raw = self.stream.read(24)
        if len(raw) != 24:
            raise ValueError("short directory record")
        _, sibling, child_dir, child_file, _, name_len = struct.unpack("<6I", raw)
        name = self.stream.read(name_len).decode("utf-8")
        return sibling, child_dir, child_file, name

    def file(self, offset: int) -> tuple[int, int, str]:
        self.stream.seek(self.file_table + offset)
        raw = self.stream.read(32)
        if len(raw) != 32:
            raise ValueError("short file record")
        _, sibling, _, size, _, name_len = struct.unpack("<IIQQII", raw)
        name = self.stream.read(name_len).decode("utf-8")
        return sibling, size, name

    def find_child_directory(self, parent: int, name: str) -> int | None:
        _, child, _, _ = self.directory(parent)
        while child != INVALID:
            sibling, _, _, child_name = self.directory(child)
            if child_name == name:
                return child
            child = sibling
        return None

    def find_child_file(self, parent: int, name: str) -> int | None:
        _, _, child, _ = self.directory(parent)
        while child != INVALID:
            sibling, _, child_name = self.file(child)
            if child_name == name:
                return child
            child = sibling
        return None

    def contains(self, path: str) -> bool:
        directory_only = path.endswith("/")
        parts = [part for part in path.strip("/").split("/") if part]
        current = 0
        for part in parts[:-1] if not directory_only else parts:
            found = self.find_child_directory(current, part)
            if found is None:
                return False
            current = found
        if directory_only:
            return True
        return bool(parts) and self.find_child_file(current, parts[-1]) is not None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("--skip-hash", action="store_true")
    args = parser.parse_args()
    package = args.package.resolve()
    data = package / "switch" / "rr3-native-rehost" / "data"
    manifest = json.loads((data / "rr3-data-manifest.json").read_text(encoding="utf-8"))
    assert manifest["format"] == "rr3-native-rehost-external-romfs-v1"

    for name, record in manifest["volumes"].items():
        image = data / record["file"]
        assert image.stat().st_size == record["romfs_bytes"], name
        assert image.stat().st_size <= 0xFFFFFFFF, name
        if not args.skip_hash:
            assert sha256(image) == record["sha256"], name
        romfs = RomFS(image)
        try:
            assert romfs.contains(record["sentinel"]), (name, record["sentinel"])
        finally:
            romfs.close()
        print(f"PASS {name}: {image.stat().st_size} bytes, sentinel present")

    nro = package / "switch" / "rr3-native-rehost" / manifest["program"]["file"]
    assert nro.stat().st_size == manifest["program"]["bytes"]
    assert sha256(nro) == manifest["program"]["sha256"]
    print("PASS program and seven external data volumes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
