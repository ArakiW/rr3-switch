# What FourCC do the pack's .dxt.dds.z files actually carry?
#
# WHY: the guest uploaded a texture with internalformat 0x83F3
# (GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 16 bytes per 4x4 block) while the rival
# car's own DDS says DXT1 (8 bytes per block). That is only a bug if the game
# expects DXT5 for this asset and the pack shipped DXT1 -- so the question is
# whether the pack contains real DXT5 files at all, or whether EVERY .dxt file
# in it is DXT1. A correct DDS loader would read the FourCC from the header,
# so if the pack has both, the trace line tells us nothing about the car and
# the mismatch theory has to be re-examined.
#
# Usage: python pack-fourcc.py <zip> [sample-size]
import collections
import struct
import sys
import zipfile
import zlib
from pathlib import Path

zip_path = Path(sys.argv[1])
sample = int(sys.argv[2]) if len(sys.argv) > 2 else 40

fourcc = collections.Counter()
by_prefix = collections.Counter()
examples = {}
with zipfile.ZipFile(zip_path) as zf:
    names = [i.filename for i in zf.infolist() if ".dds.z" in i.filename]
    print(f"pack      : {zip_path.name}")
    print(f"dds.z members: {len(names)}")
    step = max(1, len(names) // sample)
    picked = names[::step][:sample]
    for name in picked:
        try:
            blob = zf.read(name)
            raw = zlib.decompress(blob[4:])
        except Exception:
            continue
        if raw[:4] != b"DDS ":
            fourcc["not-a-dds"] += 1
            continue
        code = raw[84:88].decode("ascii", "replace")
        fourcc[code] += 1
        # which family the name claims
        if ".dxt." in name:
            by_prefix["name-dxt"] += 1
        elif ".etc." in name:
            by_prefix["name-etc"] += 1
        elif ".atc." in name:
            by_prefix["name-atc"] += 1
        else:
            by_prefix["name-other"] += 1
        examples.setdefault(code, name)

print(f"sampled   : {len(picked)}")
print(f"FourCC    : {dict(fourcc)}")
print(f"name says : {dict(by_prefix)}")
for code, name in examples.items():
    print(f"  example {code!r}: {name}")
