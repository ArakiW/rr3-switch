# Compare one member of the three source packs (ATC / DXT / ETC) with the bytes
# that are actually inside the external RomFS volume.
#
# WHY: the guest inflates every ".z" unconditionally -- it reads a 4-byte LE
# size and calls inflate() on the rest (disassembly quoted in
# guest_filesystem.cpp: "bl inflate"). The card's original Android asset
# (.etc.dds.z) IS that shape: uint32 size + zlib -> "DDS ". The volume's copy
# of a vehicle texture is NOT. So either the volume builder mangled the bytes,
# or the pack we picked holds a different container than the guest can read.
# The three packs on disk (ATC_/DXT_/ETC_, ~6 GB each) let us answer that
# directly instead of by inference.
#
# Usage: python pack-compare.py <zip> <member-suffix>
import hashlib
import struct
import sys
import zipfile
import zlib
from pathlib import Path

ZIP = Path(sys.argv[1])
SUFFIX = sys.argv[2]

print(f"pack: {ZIP.name} ({ZIP.stat().st_size} bytes)")
with zipfile.ZipFile(ZIP) as zf:
    names = [i.filename for i in zf.infolist() if SUFFIX in i.filename]
    print(f"members matching {SUFFIX!r}: {len(names)}")
    for name in names[:12]:
        info = zf.getinfo(name)
        blob = zf.read(name)
        head = blob[:16].hex(" ")
        hdr = struct.unpack_from("<I", blob)[0] if len(blob) >= 4 else 0
        detail = "not-zlib"
        try:
            out = zlib.decompress(blob[4:])
            detail = f"zlib OK -> {out[:4]!r} ({len(out)} B) expected={hdr}"
        except Exception as exc:
            detail = f"zlib FAIL ({exc})"
        print(f"  {info.file_size:>10} B  md5={hashlib.md5(blob).hexdigest()}")
        print(f"      {name}")
        print(f"      head={head}  {detail}")
