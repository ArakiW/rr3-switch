"""Read a PFS0 image (an NSP or an exefs) and print what is inside it.

WHY: an NSP is a binary container, and "the build said it succeeded" is not
a statement about the container.  This reads the PFS0 header, the file
table and the string table, so the package can be checked on the build
machine -- no console needed.

PFS0 layout, as devkitPro's build_pfs0 writes it:
    0x00  "PFS0"
    0x04  u32 file count
    0x08  u64 string table size
    0x10  the file table: count * 0x18
              +0x00 u64 offset
              +0x08 u64 size
              +0x10 u64 string table offset
    then the string table, then the data
"""
import io
import os
import struct
import sys


def read_pfs0(data):
    if data[:4] != b"PFS0":
        raise ValueError("not a PFS0 (magic %r)" % (data[:4],))
    count, = struct.unpack_from("<I", data, 4)
    string_size, = struct.unpack_from("<Q", data, 8)
    table = 0x10
    string_base = table + count * 0x18
    entries = []
    for index in range(count):
        base = table + index * 0x18
        offset, size, string_offset = struct.unpack_from("<QQQ", data, base)
        end = data.find(b"\x00", string_base + string_offset)
        name = data[string_base + string_offset:end].decode("utf-8", "replace")
        entries.append((name, offset, size))
    return entries, string_base + string_size


def show(label, data, entries, indent="   "):
    print("%s (%.2f MiB, %d entries)" % (label, len(data) / 1048576.0, len(entries)))
    for name, offset, size in entries:
        print("%s%-28s %12d bytes  @0x%x" % (indent, name, size, offset))
    for name, offset, size in entries:
        inner = data[offset:offset + size]
        # A nested container only.  The container's OWN first entry starts at
        # offset 0 and its data begins with this same PFS0 header, so
        # recursing into it would just print the same list again.
        if offset == 0 or inner[:4] != b"PFS0":
            continue
            try:
                inner_entries, _ = read_pfs0(inner)
            except ValueError:
                continue
            print("%s--- inside %s ---" % (indent, name))
            for inner_name, inner_offset, inner_size in inner_entries:
                print("%s    %-22s %12d bytes" % (indent, inner_name, inner_size))


def main():
    for path in sys.argv[1:]:
        if not os.path.exists(path):
            print("%s: MISSING" % path)
            continue
        with io.open(path, "rb") as handle:
            data = handle.read()
        entries, _ = read_pfs0(data)
        show(os.path.basename(path), data, entries)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
