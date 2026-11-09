# Decode a pack member (uint32 size + zlib -> DDS) and report what is inside.
#
# WHY: the rivals are magenta because their texture DECODES wrong -- the pixels
# are a normal grey/white car with the GREEN channel zeroed (192,4,192 /
# 255,4,255), and the window glass comes out dark purple, which is what a
# decoded-but-wrong texture looks like (a missing texture would be a flat
# (255,0,255)). The pack member inflates to a DDS, so the question is what the
# DDS says: which FourCC, what size, and what the pixels look like once the
# pixels are actually decoded.
#
# Usage: python dds-from-pack.py <zip> <member> <out.dds>
import struct
import sys
import zipfile
import zlib
from pathlib import Path

zip_path = Path(sys.argv[1])
member = sys.argv[2]
out = Path(sys.argv[3])

with zipfile.ZipFile(zip_path) as zf:
    blob = zf.read(member)

expected = struct.unpack_from("<I", blob)[0]
raw = zlib.decompress(blob[4:])
print(f"member    : {member}")
print(f"on disk   : {len(blob)} B, header says {expected}, inflated {len(raw)} B")
if raw[:4] != b"DDS ":
    print(f"NOT A DDS: magic={raw[:4]!r}")
    raise SystemExit(1)

height = int.from_bytes(raw[12:16], "little")
width = int.from_bytes(raw[16:20], "little")
mips = int.from_bytes(raw[28:32], "little")
pf_flags = int.from_bytes(raw[88:92], "little")
fourcc = raw[84:88]
caps2 = int.from_bytes(raw[112:116], "little") if len(raw) > 115 else 0
print(f"size      : {width}x{height}  mips={mips}  pf_flags=0x{pf_flags:x}")
print(f"fourcc    : {fourcc!r}  ({fourcc.decode('ascii', 'replace')})")
print(f"caps2     : 0x{caps2:x}  (0x200000 = DX10 extended header present)")
out.write_bytes(raw)
print(f"written   : {out} ({len(raw)} B)")

# And the pixel answer, if Pillow can decode this DDS.
try:
    from PIL import Image

    img = Image.open(out).convert("RGB")
    small = img.resize((64, 64))
    px = list(small.getdata())
    n = len(px)
    means = [sum(p[i] for p in px) / n for i in range(3)]
    pink = sum(1 for p in px if p[0] > 150 and p[2] > 150 and p[1] < 60)
    print(f"decoded   : {img.size[0]}x{img.size[1]} mean RGB = "
          f"({means[0]:.1f}, {means[1]:.1f}, {means[2]:.1f})")
    print(f"green-zeroed pixels: {pink} ({100.0 * pink / n:.1f} %)")
    print(f"sample    : {px[:6]}")
except Exception as exc:
    print(f"Pillow could not decode it: {exc}")
