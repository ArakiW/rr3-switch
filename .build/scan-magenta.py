#!/usr/bin/env python3
"""Is the magenta car in OUR archives?

The user reported a car rendered in the renderer's fallback colour (magenta)
and a HUD widget rendered as a flat grey-white blob.  The symptom has never
been reproduced in this project's own archives, which means every claim
about it so far has been about the LOG, not about the PICTURE.

This scans the archived race frames for magenta pixels -- high red, high
blue, low green -- and reports the frames that contain them, so the
symptom can be located in a round, at a frame, and correlated with the
log of that moment.

Usage: python .build\scan-magenta.py <round-tag> [<round-tag> ...]
"""
import struct
import sys
import zlib
from pathlib import Path

RUNS = Path(r"E:\AI\RealRacing3-switch\.build\eden-runs")


def read_pixels(path):
    """Decode a PNG into (width, height, rows of RGB tuples)."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos = 8
    width = height = 0
    bitdepth = colortype = 0
    idat = bytearray()
    palette = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", body[:10])
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length
    if width == 0 or bitdepth != 8:
        return None
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colortype)
    if channels is None:
        return None
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out), colortype, palette


def magenta_stats(path):
    img = read_pixels(path)
    if img is None:
        return None
    width, height, channels, buf, colortype, palette = img
    count = 0
    total = 0
    for y in range(0, height, 2):
        row = y * width * channels
        for x in range(0, width, 2):
            i = row + x * channels
            if colortype == 3:
                idx = buf[i] * 3
                r, g, b = palette[idx], palette[idx + 1], palette[idx + 2]
            elif channels >= 3:
                r, g, b = buf[i], buf[i + 1], buf[i + 2]
            else:
                continue
            total += 1
            # The renderer's fallback: magenta means red and blue are high
            # and green is low.
            if r > 180 and b > 180 and g < 90:
                count += 1
    if total == 0:
        return None
    return count, total, (100.0 * count / total)


def main():
    tags = sys.argv[1:] or ["pursuit-fixed-centre", "pursuit-adaptive-centre",
                             "m9-nov4-binary-todays-card", "v5-semantic-ui"]
    for tag in tags:
        hits = sorted(RUNS.glob(f"*{tag}*"))
        if not hits:
            print("{0:38} NOT FOUND".format(tag))
            continue
        frames = hits[-1] / "frames"
        pngs = sorted(frames.glob("*.png"))
        if not pngs:
            print("{0:38} no frames".format(tag))
            continue
        bad = []
        for png in pngs:
            st = magenta_stats(png)
            if st and st[0] > 0:
                bad.append((png.name, st[0], st[2]))
        print("{0:38} {1} frames, {2} with magenta".format(tag, len(pngs), len(bad)))
        for name, n, pct in bad[:8]:
            print("      {0:24} {1} px  ({2:.3f} % of the sampled pixels)".format(name, n, pct))


if __name__ == "__main__":
    main()
