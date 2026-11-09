# What is actually inside a DDS the guest loaded?
#
# WHY: the rival cars are magenta, the texture was delivered (the served size
# matched the volume entry byte for byte), and the rendered pixels are NOT the
# classic missing-texture (255,0,255) but "a normal grey car with the GREEN
# channel zeroed" (192,4,192 / 255,4,255). So either the source DDS in the data
# pack is magenta, or the port/GPU path drops green. Only reading the bytes can
# tell those two apart.
#
# Reads the DDS header, reports the FourCC and the per-channel statistics of the
# decoded pixels. Pillow's DDS plugin handles DXT1/DXT3/DXT5 and uncompressed.
#
# Usage: python dds-stats.py <file.dds> [<file.dds> ...]
import sys
from pathlib import Path

try:
    from PIL import Image
except Exception as exc:  # pragma: no cover
    print(f"PIL unavailable: {exc}")
    raise SystemExit(2)


def fourcc(raw: bytes) -> str:
    return raw.decode("ascii", "replace")


def report(path: Path) -> None:
    blob = path.read_bytes()
    print(f"--- {path.name} ({len(blob)} bytes) ---")
    if blob[:4] != b"DDS ":
        print("    not a DDS file (magic mismatch)")
        return
    size = int.from_bytes(blob[4:8], "little")
    height = int.from_bytes(blob[12:16], "little")
    width = int.from_bytes(blob[16:20], "little")
    flags = int.from_bytes(blob[80:84], "little")
    pf_size = int.from_bytes(blob[84:88], "little")
    pf_flags = int.from_bytes(blob[88:92], "little")
    code = fourcc(blob[84:88]) if (flags & 0x4) else "-"
    mip = int.from_bytes(blob[28:32], "little")
    print(f"    header={size} {width}x{height} fourcc={code!r} pf_flags=0x{pf_flags:x} mips={mip}")
    try:
        img = Image.open(path)
        img = img.convert("RGB")
    except Exception as exc:
        print(f"    Pillow cannot decode it: {exc}")
        return
    small = img.resize((64, 64)) if max(img.size) > 256 else img
    pixels = list(small.getdata())
    n = len(pixels)
    means = [sum(p[i] for p in pixels) / n for i in range(3)]
    zeros = [sum(1 for p in pixels if p[i] == 0) for i in range(3)]
    pink = sum(1 for p in pixels if p[0] > 150 and p[2] > 150 and p[1] < 60)
    print(f"    decoded {img.size[0]}x{img.size[1]}  mean RGB = "
          f"({means[0]:.1f}, {means[1]:.1f}, {means[2]:.1f})")
    print(f"    zero-count per channel R/G/B = {zeros[0]}/{zeros[1]}/{zeros[2]}  of {n}")
    print(f"    magenta-looking pixels: {pink} ({100.0 * pink / n:.1f} %)")
    print(f"    first pixels: {pixels[:6]}")


for arg in sys.argv[1:]:
    report(Path(arg))
