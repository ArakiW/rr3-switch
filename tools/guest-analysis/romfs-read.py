"""Read one file from a data volume, using the EXACT record layout that
tools/verify_rr3_data_volumes.py was verified against:

    file record = struct "<IIQQII"  (32 bytes)
        parent(4) sibling(4) data_offset(8) size(8) f20(4) name_len(4)
    and the name follows immediately at +32, then the bytes live at
    header[9] (0x200) + data_offset.

Both halves of that were paid for twice already: a 7-field guess raises
"not enough values to unpack", and a record-count-times-32 table offset lands
in the middle of a different file while still looking like text.

!! UNVALIDATED LOOKUPS -- 2026-10-04 !!
    A "file not found" from this script is NOT evidence that the file is
    absent from the volume. It reported files/.depot/eds/AndroidHigh.plist as
    missing, and the guest's own stat probe had just measured that file at
    6,808 bytes through the port, so it is present. find_dir/find_file were
    also off by one field (they started at the sibling pointer and the
    dir_child pointer respectively instead of child_dir and child_file); that
    is fixed here, but the fix is NOT yet validated by a passing control, and
    an unvalidated change to a measuring instrument is worse than a known-bad
    one because it looks trustworthy.
    Until a control passes, use verify_rr3_data_volumes.py for existence
    questions, or ask the port itself (the guest's stat probe is authoritative:
    it reads through the same resolver the game uses).
"""
import re, struct, sys
from pathlib import Path

INVALID = 0xFFFFFFFF
VOL = Path(sys.argv[1]) if len(sys.argv) > 1 else \
    Path(r"E:\Eden\user\sdmc\switch\rr3-native-rehost\data\core.romfs")
WANT = sys.argv[2] if len(sys.argv) > 2 else "files/.depot/no3g.xml"

s = VOL.open("rb")
f = struct.unpack("<10Q", s.read(80))
assert f[0] == 80
DIR_TABLE, FILE_TABLE, DATA_BASE = f[3], f[7], f[9]
print("%s size=%d dir_table=0x%x file_table=0x%x data_base=0x%x"
      % (VOL.name, VOL.stat().st_size, DIR_TABLE, FILE_TABLE, DATA_BASE))


def directory(off):
    s.seek(DIR_TABLE + off)
    raw = s.read(24)
    _, sib, cd, cf, _, nlen = struct.unpack("<6I", raw)
    return sib, cd, cf, s.read(nlen).decode("utf-8")


def file(off):
    s.seek(FILE_TABLE + off)
    raw = s.read(32)
    parent, sib, doff, size, _f20, nlen = struct.unpack("<IIQQII", raw)
    return parent, sib, doff, size, s.read(nlen).decode("utf-8")


def find_dir(parent, name):
    # directory() returns (sibling, dir_child, file_child, name). The search has
    # to start at the CHILD pointer, not the sibling pointer: starting at the
    # sibling walks a chain that does not belong to this directory at all, and
    # for a leaf directory (no subdirectories) it starts at INVALID and reports
    # "not found" immediately even though the directory has files.
    _, cd, _, _ = directory(parent)
    c = cd
    while c != INVALID:
        sib, _, _, cn = directory(c)
        if cn == name:
            return c
        c = sib
    return None


def find_file(parent, name):
    # Symmetrically: files hang off the file_child pointer, not the dir_child
    # one. The previous version started at dir_child, so it either reported
    # "not found" for a directory that has no subdirectories, or parsed
    # directory records as file records and compared garbage names.
    _, _, cf, _ = directory(parent)
    c = cf
    while c != INVALID:
        _, sib, _, _, fn = file(c)
        if fn == name:
            return c
        c = sib
    return None


parts = [p for p in WANT.strip("/").split("/") if p]
cur = 0
for p in parts[:-1]:
    cur = find_dir(cur, p)
    if cur is None:
        sys.exit("directory not found: %s" % p)
rec = find_file(cur, parts[-1])
if rec is None:
    sys.exit("file not found: %s" % WANT)
_parent, _sib, doff, size, name = file(rec)
abs_off = DATA_BASE + doff
print("FOUND %s : rel=0x%x -> abs 0x%x  size=%d" % (name, doff, abs_off, size))
s.seek(abs_off)
blob = s.read(size)
print("hex 96: %s" % blob[:96].hex())
t = blob.decode("utf-8", "replace")
pr = sum(1 for c in t[:2000] if c.isprintable() or c in "\r\n\t")
print("printable ratio in first 2k: %.3f" % (pr / 2000.0))
if pr / 2000.0 > 0.8:
    print("---- content ----")
    print(t[:3000])
else:
    print("binary; printable runs:")
    for m in list(re.finditer(rb"[ -~]{4,}", blob))[:40]:
        print("  %r" % m.group())
