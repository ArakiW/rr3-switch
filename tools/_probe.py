#!/usr/bin/env python3
"""Probe depot files for encryption patterns."""
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(r"E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files\.depot")

def hexdump(d, n=64):
    return " ".join(f"{b:02x}" for b in d[:n])

def try_zlib(data):
    if len(data) >= 8:
        expected = struct.unpack_from("<I", data)[0]
        if 0 < expected < 256 * 1024 * 1024:
            try:
                out = zlib.decompress(data[4:])
                if len(out) == expected:
                    return out
            except zlib.error:
                pass
    for wbits in (15, -15, 31):
        try:
            return zlib.decompress(data, wbits)
        except zlib.error:
            continue
    return None

def score_text(d):
    return sum(1 for b in d if 32 <= b < 127 or b in (9,10,13))/len(d)

def score_glsl(d):
    text = d.decode("latin-1","replace")
    needles = ["#version","attribute","uniform","varying","precision","gl_Position","gl_FragColor","void main","vec2","vec3","vec4","mat4","highp","mediump","lowp"]
    hits = sum(1 for n in needles if n in text)
    pr = score_text(d)
    return min(1.0, (hits/6.0)*0.6 + pr*0.4)

# 1. 2dflat.vsh from depot
p1 = ROOT / "Shaders" / "rr2" / "2dflat.vsh"
print(f"=== {p1.name} ===")
d = p1.read_bytes()
print(f"len={len(d)} md5={hashlib.md5(d).hexdigest()}")
print(f"head={hexdump(d,32)}")
print(f"bytes[2:6]={d[2:6].hex()}")
z = try_zlib(d)
print(f"zlib: {'OK -> '+str(len(z))+' bytes' if z else 'fail'}")
if z:
    print(f"  text={score_text(z):.2f} glsl={score_glsl(z):.2f}")
    print(f"  head={hexdump(z,128)}")
    print(f"  text-preview={z[:200].decode('latin-1','replace')!r}")
else:
    # device read first 16 bytes: 1c3831423a44c1ca822ff4012076ff00
    dev = bytes.fromhex("1c3831423a44c1ca822ff4012076ff00")
    print(f"device-head match? local[:16]==dev: {d[:16]==dev}")
    print(f"  local[:16]={d[:16].hex()}  device[:16]={dev.hex()}")

# 2. LoadingScreen_Init.gui
p2 = ROOT / "gui" / "processed" / "LoadingScreen_Init.gui"
print(f"\n=== {p2.name} ===")
d2 = p2.read_bytes()
print(f"len={len(d2)} md5={hashlib.md5(d2).hexdigest()}")
print(f"head={hexdump(d2,32)}")
print(f"bytes[2:6]={d2[2:6].hex()}")
z = try_zlib(d2)
print(f"zlib: {'OK -> '+str(len(z))+' bytes' if z else 'fail'}")

# 3. Survey magics across .vsh files
magics = {}
for p in ROOT.rglob("*.vsh"):
    dd = p.read_bytes()
    if len(dd) >= 6:
        m = dd[2:6]
        magics.setdefault(m, []).append((p.relative_to(ROOT), len(dd)))
print(f"\n=== vsh magics[2:6] ===")
for m, hits in sorted(magics.items(), key=lambda kv: -len(kv[1])):
    if len(magic := m) == 4:
        sample = hits[0][0]
        print(f"  {m.hex()}  x{len(hits)}  e.g. {sample} ({hits[0][1]}b)")

# 4. Survey magics across .gui files
magics_g = {}
for p in ROOT.rglob("*.gui"):
    dd = p.read_bytes()
    if len(dd) >= 6:
        m = dd[2:6]
        magics_g.setdefault(m, []).append((p.relative_to(ROOT), len(dd)))
print(f"\n=== gui magics[2:6] ===")
for m, hits in sorted(magics_g.items(), key=lambda kv: -len(kv[1])):
    print(f"  {m.hex()}  x{len(hits)}  e.g. {hits[0][0]} ({hits[0][1]}b)")