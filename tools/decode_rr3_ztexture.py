#!/usr/bin/env python3
"""Decode RR3's little-endian-size + zlib wrapped DDS/PVR texture files."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import zlib

from PIL import Image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    wrapped = args.source.read_bytes()
    if len(wrapped) < 6:
        raise SystemExit("input is too small")
    expected = struct.unpack_from("<I", wrapped)[0]
    decoded = zlib.decompress(wrapped[4:])
    if len(decoded) != expected:
        raise SystemExit(f"decoded {len(decoded)} bytes, expected {expected}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    is_dds = decoded[:4] == b"DDS "
    is_pvr = decoded[:4] == b"PVR\x03"
    is_pvr2 = len(decoded) >= 52 and decoded[44:48] == b"PVR!"
    if not is_dds and not is_pvr and not is_pvr2:
        raise SystemExit(f"unsupported decoded texture magic {decoded[:4]!r}")
    raw_output = args.output.with_suffix(".dds" if is_dds else ".pvr")
    raw_output.write_bytes(decoded)
    if is_dds:
        with Image.open(raw_output) as image:
            image.save(args.output)
            print(f"decoded {args.source.name}: {image.width}x{image.height} {image.mode}")
        return

    if is_pvr2:
        header_size, height, width, _mips, flags, data_size, bpp = struct.unpack_from(
            "<IIIIIII", decoded
        )
        if header_size != 52 or bpp != 32 or (flags & 0xff) != 0x12:
            raise SystemExit(
                f"unsupported PVR2 header size={header_size} bpp={bpp} flags=0x{flags:x}"
            )
        pixels = decoded[header_size:header_size + data_size]
        if len(pixels) != width * height * 4:
            raise SystemExit("truncated PVR2 RGBA image")
        image = Image.frombytes("RGBA", (width, height), pixels, "raw", "RGBA")
        image.save(args.output)
        print(f"decoded {args.source.name}: {width}x{height} RGBA (PVR2)")
        return

    (
        _version,
        _flags,
        pixel_format,
        _colour_space,
        _channel_type,
        height,
        width,
        _depth,
        _surfaces,
        _faces,
        _mips,
        metadata_size,
    ) = struct.unpack_from("<IIQIIIIIIIII", decoded)
    channels = pixel_format.to_bytes(8, "little")[:4].rstrip(b"\0").decode("ascii")
    bits = pixel_format.to_bytes(8, "little")[4:4 + len(channels)]
    if channels not in ("rgb", "rgba", "bgra") or any(bit != 8 for bit in bits):
        raise SystemExit(f"unsupported PVR pixel format channels={channels!r} bits={list(bits)}")
    mode = "RGB" if len(channels) == 3 else "RGBA"
    offset = 52 + metadata_size
    byte_count = width * height * len(channels)
    pixels = decoded[offset:offset + byte_count]
    if len(pixels) != byte_count:
        raise SystemExit("truncated PVR top-level image")
    raw_mode = "BGRA" if channels == "bgra" else mode
    image = Image.frombytes(mode, (width, height), pixels, "raw", raw_mode)
    image.save(args.output)
    print(f"decoded {args.source.name}: {width}x{height} {mode} ({channels})")


if __name__ == "__main__":
    main()
