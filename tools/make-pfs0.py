"""Build a PFS0 container from a directory tree.

WHY THIS EXISTS INSTEAD OF devkitPro's build_pfs0.  build_pfs0 packs the
TOP LEVEL of the input directory only: it prints "Directories aren't
supported, skipping..." for a subdirectory and does not recurse into it.
An NSP's entries have to be named "exefs/main" and "exefs/main.npdm" --
the directory layout has to be INSIDE the container -- so the container is
written here, where the layout is visible.

THE FORMAT, AS build_pfs0 WRITES IT (read back off its own output):
    0x00  "PFS0"
    0x04  u32 file count
    0x08  u64 string table size
    0x10  the file table, count * 0x18:
              +0x00 u64 offset into the data
              +0x08 u64 size
              +0x10 u32 offset into the string table
              +0x14 u32 parent index (0 = none)
    then the string table (names, NUL separated, padded to 0x10)
    then the data, in table order, with no padding between the files

Usage:
    python make-pfs0.py <input directory> <output PFS0 file>
"""
import io
import os
import sys


def collect(root):
    """Every file under root, named by its path relative to root."""
    entries = []
    for current, _dirs, files in os.walk(root):
        for name in sorted(files):
            full = os.path.join(current, name)
            relative = os.path.relpath(full, root).replace(os.sep, "/")
            entries.append((relative, full))
    entries.sort(key=lambda item: item[0])
    return entries


def build_pfs0(entries):
    string_table = bytearray()
    string_offsets = []
    for relative, _full in entries:
        string_offsets.append(len(string_table))
        string_table += relative.encode("utf-8") + b"\x00"
    while len(string_table) % 0x10 != 0:
        string_table += b"\x00"

    header = bytearray()
    header += b"PFS0"
    header += len(entries).to_bytes(4, "little")
    header += len(string_table).to_bytes(8, "little")

    data = bytearray()
    table = bytearray()
    for index, (relative, full) in enumerate(entries):
        with io.open(full, "rb") as handle:
            blob = handle.read()
        offset = len(data)
        data += blob
        table += offset.to_bytes(8, "little")
        table += len(blob).to_bytes(8, "little")
        table += string_offsets[index].to_bytes(4, "little")
        table += (0).to_bytes(4, "little")
    return bytes(header) + bytes(table) + bytes(string_table) + bytes(data)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    root = sys.argv[1]
    out = sys.argv[2]
    if not os.path.isdir(root):
        print("%s: not a directory" % root)
        return 1
    entries = collect(root)
    if not entries:
        print("%s: no files to pack" % root)
        return 1
    blob = build_pfs0(entries)
    with io.open(out, "wb") as handle:
        handle.write(blob)
    for relative, full in entries:
        print("  packed %-28s %d bytes" % (relative, os.path.getsize(full)))
    print("wrote %s (%d bytes, %d entries)" % (out, len(blob), len(entries)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
