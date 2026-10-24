#!/usr/bin/env python3
"""List (or search) the files inside the RR3 external RomFS data volumes.

The data pack is the ORACLE for "does the pack contain the file the guest
wants". The guest's own asset list is not the pack; the pack is the RomFS
images under switch/rr3-native-rehost/data.

Usage:
    python romfs_ls.py <package-root> [--grep REGEX] [--volumes a,b]
    python romfs_ls.py <package-root> --show files/.depot/gametext.txt
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

INVALID = 0xFFFFFFFF


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

    def directory(self, offset: int):
        self.stream.seek(self.dir_table + offset)
        raw = self.stream.read(24)
        _, sibling, child_dir, child_file, _, name_len = struct.unpack("<6I", raw)
        name = self.stream.read(name_len).decode("utf-8", "replace")
        return sibling, child_dir, child_file, name

    def file(self, offset: int):
        self.stream.seek(self.file_table + offset)
        raw = self.stream.read(32)
        _, sibling, _, size, _, name_len = struct.unpack("<IIQQII", raw)
        name = self.stream.read(name_len).decode("utf-8", "replace")
        return sibling, size, name

    def walk(self, node: int = 0, prefix: str = ""):
        """Yield (path, size) for every file under the directory node."""
        _, child_dir, child_file, name = self.directory(node)
        path = prefix + name + "/" if node != 0 else ""
        while child_file != INVALID:
            _, size, file_name = self.file(child_file)
            yield path + file_name, size
            child_file = self.file(child_file)[0]
        while child_dir != INVALID:
            sibling, _, _, dir_name = self.directory(child_dir)
            yield from self.walk(child_dir, path + dir_name + "/")
            child_dir = sibling


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("package", type=Path)
    ap.add_argument("--grep", default=None)
    ap.add_argument("--volumes", default=None)
    ap.add_argument("--show", default=None, help="print the head of one file")
    ap.add_argument("--bytes", type=int, default=4096)
    args = ap.parse_args()

    data = args.package / "switch" / "rr3-native-rehost" / "data"
    manifest = json.loads((data / "rr3-data-manifest.json").read_text(encoding="utf-8-sig"))
    volumes = manifest["volumes"]
    wanted = args.volumes.split(",") if args.volumes else list(volumes)
    pattern = re.compile(args.grep, re.I) if args.grep else None

    for name in wanted:
        record = volumes[name]
        romfs = RomFS(data / record["file"])
        try:
            for path, size in romfs.walk():
                if args.show is not None:
                    if path == args.show:
                        romfs.stream.seek(0)
                        break
                    continue
                if pattern is not None and pattern.search(path):
                    print("%-16s %10d  %s" % (name, size, path))
            if args.show is not None:
                # The file data starts after the directory/file tables; find the
                # data offset from the file record.
                node = self_find(romfs, args.show)
                if node is None:
                    print("not found: %s" % args.show)
                    return 1
                romfs.stream.seek(node)
                blob = romfs.stream.read(args.bytes)
                sys.stdout.write(blob.decode("utf-8", "replace"))
        finally:
            romfs.close()
    return 0


def self_find(romfs: RomFS, path: str):
    """Return the data offset of a file by its RomFS path."""
    parts = [p for p in path.strip("/").split("/") if p]
    node = 0
    for part in parts[:-1]:
        found = None
        _, child, _, _ = romfs.directory(node)
        while child != INVALID:
            sibling, _, _, name = romfs.directory(child)
            if name == part:
                found = child
                break
            child = sibling
        if found is None:
            return None
        node = found
    _, child, _, _ = romfs.directory(node)
    while child != INVALID:
        sibling, _, _, name = romfs.file(child)
        if name == parts[-1]:
            # The file record's first field is the data offset.
            romfs.stream.seek(romfs.file_table + child)
            offset = struct.unpack("<I", romfs.stream.read(4))[0]
            return offset
        child = sibling
    return None


if __name__ == "__main__":
    sys.exit(main())
