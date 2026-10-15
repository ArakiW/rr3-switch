#!/usr/bin/env python3
"""RR3 depot asset decoder / crypto analyzer — comprehensive version.

Asset families observed in files/.depot (2026-09 survey):

A. Plaintext — langs.utf8, animations.txt, *.reg, *.decomp, *.html, *.svg,
   *.js, dataoffsets.txt, asset_packs.cfg, and all *.atlas (XML) in depot.

B. Size-prefixed zlib — [u32le uncompressed_size][zlib stream].
   Confirmed: data.bin (472→1184), MenuTransitions.xml (2513→14359),
   *.otf/*.ttf fonts, and the existing *.dds.z / *.pvr.z textures.
   NOTE: .atlas in the APK are zlib-compressed but the depot copies are
   already decompressed plaintext XML.

C. Opaque / encrypted — same on-disk size as asset_list column 2.
   Confirmed: Shaders/**/*.vsh|fsh, gui/processed/*.gui.
   Header observation: every checked *.vsh shares bytes[2:6] == 31 42 3a 44.
     2dflat.vsh            1c 38 31 42 3a 44 c1 ca 82 2f f4 01 20 76 ff 00
     full_screen_effects   d1 37 31 42 3a 44 89 12 1e ca ed 77 24 4f 6e 15

   gui/processed/*.gui observed to share a 21-byte constant header
   (bytes 0..20), then vary. First 16 bytes identical across files:
     LoadingScreen_Init.gui  e6 ea 60 c5 40 26 f3 88 a8 a6 5a 62 e9 95 76 bf
     GlobalStyles.gui       e6 ea 60 c5 40 26 f3 88 a8 a6 5a 62 e9 95 76 bf
     SpriteAnimation.gui    e6 ea 60 c5 40 26 f3 88 a8 a6 5a 62 e9 95 76 bf
   Bytes 16..20 also identical (db e5 de 38 de), giving a 21-byte constant
   header across all observed .gui files.

   Crypto observations: high Shannon entropy (~7.13 bits/byte) is consistent
   with a stream cipher. libRealRacing3.so contains the ChaCha20 / Salsa20
   "expand 32-byte k" constant at offsets 0x1B567A0 and 0x1B584C0 inside
   the .text section (the implementation is named "ChaCha20 for ARMv...").
   AES S-box and SHA256 round constants are NOT present, ruling out AES.
   The same 16-byte .gui header across files with different sizes is the
   strongest hint that the cipher is initialized with a constant nonce for
   the gui/ subtree while .vsh files use a per-file nonce.

asset_list_startup.txt format:  path <TAB> md5 <TAB> disk_size <TAB> uncompressed_size
  disk == uncompressed  →  family C (or already-decompressed A)
  disk <  uncompressed  →  family B (zlib)

Usage:
  python decode_rr3_depot.py probe  <file>...
  python decode_rr3_depot.py zlib   <file> [-o out]
  python decode_rr3_depot.py attack <encrypted> <known_plaintext_prefix> [-o out]
  python decode_rr3_depot.py keystream <encrypted> <known_plaintext_prefix>
  python decode_rr3_depot.py romfs  <core.romfs> <inner_path> [-o out]
  python decode_rr3_depot.py batch  <depot_root> [-o outdir]
  python decode_rr3_depot.py chacha20 <encrypted> <key_hex32> <nonce_hex12> [-o out]
  python decode_rr3_depot.py gui-headers <depot_root>
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def hexdump(data: bytes, limit: int = 64) -> str:
    lines = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {off:04x}  {hexpart:<48}  {asciipart}")
    if len(data) > limit:
        lines.append(f"  ... ({len(data)} bytes total)")
    return "\n".join(lines)


def try_zlib(data: bytes) -> bytes | None:
    """Try [u32le size][zlib] then raw zlib at offset 0."""
    if len(data) >= 8:
        expected = struct.unpack_from("<I", data)[0]
        if 0 < expected < 256 * 1024 * 1024:
            try:
                out = zlib.decompress(data[4:])
                if len(out) == expected:
                    return out
            except zlib.error:
                pass
    for wbits in (15, -15, 31):  # zlib, raw deflate, gzip
        try:
            return zlib.decompress(data, wbits)
        except zlib.error:
            continue
    return None


def score_glsl(data: bytes) -> float:
    if not data:
        return 0.0
    text = data.decode("latin-1", errors="replace")
    needles = [
        "#version", "attribute", "uniform", "varying",
        "precision", "gl_Position", "gl_FragColor",
        "void main", "vec2", "vec3", "vec4", "mat4",
        "highp", "mediump", "lowp",
    ]
    hits = sum(1 for n in needles if n in text)
    printable = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    ratio = printable / len(data)
    return min(1.0, (hits / 6.0) * 0.6 + ratio * 0.4)


def score_text(data: bytes) -> float:
    if not data:
        return 0.0
    printable = sum(1 for b in data if 32 <= b < 127 or b in (9, 10, 13))
    return printable / len(data)


def score_gui(data: bytes) -> float:
    if not data:
        return 0.0
    text = data.decode("latin-1", errors="replace")
    needles = ["screen", "view", "button", "label", "image", "panel",
               "LoadingScreen", "title", "background", "font", "color", "size"]
    hits = sum(1 for n in needles if n.lower() in text.lower())
    pr = score_text(data)
    return min(1.0, (hits / 4.0) * 0.5 + pr * 0.5)


# ---------------------------------------------------------------------------
# stream-cipher candidates
# ---------------------------------------------------------------------------

def xor_const(data: bytes, key: int) -> bytes:
    return bytes(b ^ key for b in data)


def xor_repeating(data: bytes, key: bytes) -> bytes:
    klen = len(key)
    return bytes(data[i] ^ key[i % klen] for i in range(len(data)))


def xor_rolling(data: bytes, seed: int) -> bytes:
    out = bytearray(len(data))
    state = seed
    for i, b in enumerate(data):
        out[i] = b ^ state
        state = (state + b) & 0xFF
    return bytes(out)


def xor_rolling_pt(data: bytes, seed: int) -> bytes:
    out = bytearray(len(data))
    state = seed
    for i, b in enumerate(data):
        p = b ^ state
        out[i] = p
        state = (state + p) & 0xFF
    return bytes(out)


def xor_prev_ct(data: bytes, key: int) -> bytes:
    if not data:
        return b""
    out = bytearray(len(data))
    out[0] = data[0] ^ key
    for i in range(1, len(data)):
        out[i] = data[i] ^ data[i - 1] ^ key
    return bytes(out)


def xor_prev_pt(data: bytes, key: int) -> bytes:
    out = bytearray(len(data))
    prev = key
    for i, b in enumerate(data):
        out[i] = b ^ prev ^ key
        prev = out[i]
    return bytes(out)


def add_const(data: bytes, n: int) -> bytes:
    return bytes((b + n) & 0xFF for b in data)


def sub_const(data: bytes, n: int) -> bytes:
    return bytes((b - n) & 0xFF for b in data)


def nibble_swap(data: bytes) -> bytes:
    return bytes(((b & 0x0F) << 4) | (b >> 4) for b in data)


def bit_not(data: bytes) -> bytes:
    return bytes((~b) & 0xFF for b in data)


def rc4(data: bytes, key: bytes, drop: int = 0) -> bytes:
    """Standard RC4 with optional keystream drop."""
    S = list(range(256))
    j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    i = j = 0
    for _ in range(drop):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(len(data))
    for n in range(len(data)):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out[n] = data[n] ^ S[(S[i] + S[j]) & 0xFF]
    return bytes(out)


def xor_with_position(data: bytes, base: int, step: int) -> bytes:
    return bytes((b ^ ((base + i * step) & 0xFF)) & 0xFF for i, b in enumerate(data))


def xor_with_header_key(data: bytes, header: bytes) -> bytes:
    if not header:
        return data
    return bytes(b ^ header[i % len(header)] for i, b in enumerate(data))


# ---------------------------------------------------------------------------
# ChaCha20 (per RFC 7539)
# ---------------------------------------------------------------------------

def _quarter_round(state, a, b, c, d):
    state[a] = (state[a] + state[b]) & 0xFFFFFFFF
    state[d] ^= state[a]
    state[d] = ((state[d] << 16) | (state[d] >> 16)) & 0xFFFFFFFF
    state[c] = (state[c] + state[d]) & 0xFFFFFFFF
    state[b] ^= state[c]
    state[b] = ((state[b] << 12) | (state[b] >> 20)) & 0xFFFFFFFF
    state[a] = (state[a] + state[b]) & 0xFFFFFFFF
    state[d] ^= state[a]
    state[d] = ((state[d] << 8) | (state[d] >> 24)) & 0xFFFFFFFF
    state[c] = (state[c] + state[d]) & 0xFFFFFFFF
    state[b] ^= state[c]
    state[b] = ((state[b] << 7) | (state[b] >> 25)) & 0xFFFFFFFF


def _chacha20_block(key: bytes, counter: int, nonce: bytes) -> bytes:
    if len(key) != 32:
        raise ValueError("ChaCha20 key must be 32 bytes")
    if len(nonce) != 12:
        raise ValueError("ChaCha20 nonce must be 12 bytes")
    constants = (0x61707865, 0x3320646E, 0x79622D32, 0x6B206574)
    state = list(constants)
    state += list(struct.unpack("<8I", key))
    state.append(counter & 0xFFFFFFFF)
    state += list(struct.unpack("<3I", nonce))
    working = state[:]
    for _ in range(10):
        _quarter_round(working, 0, 4, 8, 12)
        _quarter_round(working, 1, 5, 9, 13)
        _quarter_round(working, 2, 6, 10, 14)
        _quarter_round(working, 3, 7, 11, 15)
        _quarter_round(working, 0, 5, 10, 15)
        _quarter_round(working, 1, 6, 11, 12)
        _quarter_round(working, 2, 7, 8, 13)
        _quarter_round(working, 3, 4, 9, 14)
    out = bytearray(64)
    for i, w in enumerate(working):
        s = (w + state[i]) & 0xFFFFFFFF
        out[i * 4:i * 4 + 4] = struct.pack("<I", s)
    return bytes(out)


def chacha20_decrypt(data: bytes, key: bytes, nonce: bytes, counter: int = 0) -> bytes:
    out = bytearray(len(data))
    pos = 0
    c = counter
    while pos < len(data):
        block = _chacha20_block(key, c, nonce)
        end = min(64, len(data) - pos)
        for i in range(end):
            out[pos + i] = data[pos + i] ^ block[i]
        pos += 64
        c += 1
    return bytes(out)


# ---------------------------------------------------------------------------
# known-plaintext attack
# ---------------------------------------------------------------------------

def derive_xor_keystream(ct: bytes, pt: bytes) -> bytes:
    n = min(len(ct), len(pt))
    return bytes(ct[i] ^ pt[i] for i in range(n))


def analyze_keystream(ks: bytes) -> dict:
    info = {"len": len(ks), "unique": len(set(ks)),
            "first16": ks[:16].hex() if ks else ""}
    if len(ks) >= 16:
        info["is_const"] = len(set(ks)) == 1
        for klen in (1, 2, 3, 4, 5, 6, 7, 8, 16):
            if all(ks[i] == ks[i % klen] for i in range(len(ks))):
                info[f"repeat_{klen}"] = True
                info["key"] = ks[:klen].hex()
                break
    return info


def attack(encrypted: bytes, plaintext_prefix: bytes, skip: int = 0) -> list[tuple[str, bytes, float]]:
    body = encrypted[skip:] if skip else encrypted
    results: list[tuple[str, bytes, float]] = []
    is_glsl = b"#" in plaintext_prefix or b"version" in plaintext_prefix
    scorer = (lambda d: score_glsl(d)) if is_glsl else (lambda d: score_text(d))

    # constant XOR
    if plaintext_prefix:
        key = body[0] ^ plaintext_prefix[0]
        results.append((f"xor_const(0x{key:02x})", xor_const(body, key), scorer(xor_const(body, key))))

    # repeating XOR
    ks = derive_xor_keystream(body, plaintext_prefix)
    if len(ks) >= 8:
        for klen in range(1, min(64, len(ks) + 1)):
            if all(ks[i] == ks[i % klen] for i in range(len(ks))):
                key = ks[:klen]
                cand = xor_repeating(body, key)
                results.append((f"xor_repeat(key={key.hex()})", cand, scorer(cand)))
                break

    # rolling XOR
    for seed in range(256):
        cand = xor_rolling(body, seed)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_rolling(seed=0x{seed:02x})", cand, sc))
    for seed in range(256):
        cand = xor_rolling_pt(body, seed)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_rolling_pt(seed=0x{seed:02x})", cand, sc))

    # prev-ct / prev-pt XOR
    for k in range(256):
        cand = xor_prev_ct(body, k)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_prev_ct(k=0x{k:02x})", cand, sc))
    for k in range(256):
        cand = xor_prev_pt(body, k)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_prev_pt(k=0x{k:02x})", cand, sc))

    # add / sub / nibble / bit_not
    if plaintext_prefix:
        delta = (plaintext_prefix[0] - body[0]) & 0xFF
        results.append((f"add_const(+0x{delta:02x})", add_const(body, delta), scorer(add_const(body, delta))))
        results.append((f"sub_const(-0x{delta:02x})", sub_const(body, delta), scorer(sub_const(body, delta))))
    results.append(("nibble_swap", nibble_swap(body), scorer(nibble_swap(body))))
    results.append(("bit_not", bit_not(body), scorer(bit_not(body))))

    # position-based XOR
    for step in (1, 2, 3):
        for base in range(0, 256, 8 if step != 1 else 1):
            cand = xor_with_position(body, base, step)
            sc = scorer(cand)
            if sc > 0.6:
                results.append((f"xor_pos(base=0x{base:02x},step={step})", cand, sc))

    # RC4 with common keys + drops
    candidate_keys = [
        b"RR3", b"rr3", b"Firemint", b"Firemonkeys", b"Cloudcell",
        b"depot", b".depot", b"shader", b"EA", b"glu", b"csdk",
        b"RealRacing", b"realracing", b"vsh", b"fsh", b"gui",
        b"MT_GL_ES_3", b"MT_GL_ES_2", b"\x1b:D", b"vsh1", b"vsh0", b"shad",
        b"\x00" * 4, b"\x00" * 8, b"\x00" * 16,
        b"12345678", b"abcdefgh",
    ]
    for key in candidate_keys:
        for drop in (0, 256, 512, 1024, 1536, 2048, 3072):
            cand = rc4(body, key, drop)
            sc = scorer(cand)
            if sc > 0.4:
                tag = f"rc4({key!r})" + (f"+drop{drop}" if drop else "")
                results.append((tag, cand, sc))

    # header-as-key XOR
    for n in (4, 6, 8, 12, 16):
        if n <= len(encrypted):
            cand = xor_with_header_key(encrypted, encrypted[:n])
            results.append((f"xor_header({n})", cand[skip:] if skip else cand,
                           scorer(cand[skip:] if skip else cand)))

    # zlib after each candidate
    extra: list[tuple[str, bytes, float]] = []
    for name, cand, sc in results:
        z = try_zlib(cand)
        if z is not None:
            extra.append((name + "+zlib", z, max(sc, score_text(z))))
    results.extend(extra)

    results.sort(key=lambda t: t[2], reverse=True)
    return results


# ---------------------------------------------------------------------------
# RomFS reader
# ---------------------------------------------------------------------------

INVALID = 0xFFFFFFFF


class RomFS:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.stream = path.open("rb")
        header = self.stream.read(80)
        if len(header) != 80:
            raise ValueError(f"short RomFS header: {path}")
        fields = struct.unpack("<10Q", header)
        if fields[0] != 80:
            raise ValueError(f"unexpected RomFS header size {fields[0]}: {path}")
        self.dir_table = fields[3]
        self.file_table = fields[7]

    def close(self) -> None:
        self.stream.close()

    def directory(self, offset: int) -> tuple[int, int, int, str]:
        self.stream.seek(self.dir_table + offset)
        raw = self.stream.read(24)
        if len(raw) != 24:
            raise ValueError("short directory record")
        _, sibling, child_dir, child_file, _, name_len = struct.unpack("<6I", raw)
        name = self.stream.read(name_len).decode("utf-8")
        return sibling, child_dir, child_file, name

    def file(self, offset: int) -> tuple[int, int, str, int]:
        self.stream.seek(self.file_table + offset)
        raw = self.stream.read(32)
        if len(raw) != 32:
            raise ValueError("short file record")
        _, sibling, data_off, size, _, name_len = struct.unpack("<IIQQII", raw)
        name = self.stream.read(name_len).decode("utf-8")
        return sibling, size, name, data_off

    def find_child_directory(self, parent: int, name: str) -> int | None:
        _, child, _, _ = self.directory(parent)
        while child != INVALID:
            sibling, _, _, child_name = self.directory(child)
            if child_name == name:
                return child
            child = sibling
        return None

    def find_child_file(self, parent: int, name: str) -> int | None:
        _, _, child, _ = self.directory(parent)
        while child != INVALID:
            sibling, _, _, child_name = self.file(child)
            if child_name == name:
                return child
            child = sibling
        return None

    def resolve_path(self, path: str) -> int | None:
        parts = [p for p in path.strip("/").split("/") if p]
        if not parts:
            return None
        current = 0
        for part in parts[:-1]:
            found = self.find_child_directory(current, part)
            if found is None:
                return None
            current = found
        return self.find_child_file(current, parts[-1])

    def read_file(self, path: str) -> bytes:
        foff = self.resolve_path(path)
        if foff is None:
            raise FileNotFoundError(path)
        _, size, _, data_off = self.file(foff)
        self.stream.seek(data_off)
        return self.stream.read(size)


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_probe(paths: list[Path]) -> None:
    for p in paths:
        data = p.read_bytes()
        print(f"\n=== {p.name}  ({len(data)} bytes)  md5={hashlib.md5(data).hexdigest()} ===")
        print(hexdump(data, 64))
        z = try_zlib(data)
        if z is not None:
            print(f"  zlib: OK -> {len(z)} bytes")
            print(hexdump(z, 64))
            print(f"  text-score={score_text(z):.2f} glsl-score={score_glsl(z):.2f}")
        else:
            print("  zlib: fail")
            if len(data) >= 6:
                print(f"  bytes[2:6]={data[2:6].hex()}  (shared vsh magic candidate: 31423a44)")


def cmd_zlib(path: Path, out: Path | None) -> None:
    data = path.read_bytes()
    z = try_zlib(data)
    if z is None:
        print("zlib decode failed")
        sys.exit(1)
    dest = out or path.with_suffix(path.suffix + ".out")
    dest.write_bytes(z)
    print(f"wrote {dest} ({len(z)} bytes)")
    print(hexdump(z, 128))


def cmd_attack(path: Path, plaintext: str, out: Path | None) -> None:
    enc = path.read_bytes()
    pt = plaintext.encode("latin-1")
    print(f"encrypted: {len(enc)} bytes")
    print(hexdump(enc, 64))
    print(f"known plaintext prefix: {pt!r}")

    best: list[tuple[str, bytes, float]] = []
    for skip in (0, 2, 4, 6, 8, 12, 16):
        if skip >= len(enc):
            continue
        cands = attack(enc, pt, skip=skip)
        for name, cand, sc in cands[:5]:
            best.append((f"skip={skip} {name}", cand, sc))

    best.sort(key=lambda t: t[2], reverse=True)
    seen = set()
    printed = 0
    for name, cand, sc in best:
        h = hashlib.md5(cand).hexdigest()
        if h in seen:
            continue
        seen.add(h)
        print(f"\n--- {name}  score={sc:.2f} ---")
        print(hexdump(cand, 96))
        try:
            print(cand[:400].decode("utf-8", errors="replace"))
        except Exception:
            pass
        printed += 1
        if printed >= 12:
            break

    if out and printed:
        out.write_bytes(best[0][1])
        print(f"\nwrote top candidate to {out}")


def cmd_keystream(path: Path, plaintext: str) -> None:
    enc = path.read_bytes()
    pt = plaintext.encode("latin-1")
    ks = derive_xor_keystream(enc, pt)
    info = analyze_keystream(ks)
    print(f"encrypted: {len(enc)} bytes")
    print(f"plaintext prefix: {pt!r} ({len(pt)} bytes)")
    print(f"derived keystream (full): {ks.hex()}")
    print(f"  unique bytes: {info.get('unique')} / 256")
    print(f"  is constant: {info.get('is_const')}")
    for k in sorted(info):
        if k.startswith("repeat_") and info[k]:
            print(f"  repeating period {k.split('_')[1]}: key={info.get('key')}")


def cmd_romfs(romfs_path: Path, inner_path: str, out: Path | None) -> None:
    romfs = RomFS(romfs_path)
    try:
        data = romfs.read_file(inner_path)
    except FileNotFoundError:
        print(f"file not found inside RomFS: {inner_path}")
        sys.exit(1)
    finally:
        romfs.close()
    print(f"extracted {inner_path} from {romfs_path.name}: {len(data)} bytes")
    print(hexdump(data, 64))
    if out:
        out.write_bytes(data)
        print(f"wrote {out}")


def cmd_chacha20(path: Path, key_hex: str, nonce_hex: str, out: Path | None) -> None:
    data = path.read_bytes()
    key = bytes.fromhex(key_hex)
    nonce = bytes.fromhex(nonce_hex)
    dec = chacha20_decrypt(data, key, nonce, 0)
    print(f"decrypted {len(dec)} bytes with ChaCha20")
    print(hexdump(dec, 64))
    print(f"text-score={score_text(dec):.2f} glsl-score={score_glsl(dec):.2f}")
    print(f"first 200 bytes: {dec[:200].decode('utf-8', errors='replace')!r}")
    if out:
        out.write_bytes(dec)


def cmd_gui_headers(root: Path) -> None:
    """Confirm that every .gui file in a depot subtree shares the same 21-byte header."""
    for p in sorted(root.rglob("*.gui")):
        data = p.read_bytes()
        if len(data) < 21:
            continue
        head = data[:21].hex()
        print(f"  {p.relative_to(root)}  ({len(data)} bytes)  head[:21]={head}")


def cmd_batch(root: Path, outdir: Path | None) -> None:
    outdir = outdir or Path("rr3_depot_decoded")
    outdir.mkdir(parents=True, exist_ok=True)

    interesting = {".vsh", ".fsh", ".gui", ".bin", ".atlas", ".nct", ".ncm", ".xml"}
    files = [p for p in root.rglob("*") if p.suffix.lower() in interesting and p.is_file()]
    print(f"found {len(files)} candidate files under {root}")

    magics: dict[bytes, int] = {}
    for p in files:
        data = p.read_bytes()
        if len(data) >= 6:
            mag = data[2:6]
            magics[mag] = magics.get(mag, 0) + 1

    print("\nshared bytes[2:6] histogram (top 10):")
    for mag, cnt in sorted(magics.items(), key=lambda t: -t[1])[:10]:
        print(f"  {mag.hex()}  x{cnt}")

    zcount = 0
    for p in files:
        data = p.read_bytes()
        z = try_zlib(data)
        if z is not None:
            rel = p.relative_to(root)
            dest = outdir / rel.parent / (rel.name + ".dec")
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(z)
            zcount += 1
    print(f"\nzlib-decoded {zcount} files -> {outdir}")

    print("\n*.vsh headers (first 16 bytes):")
    for p in sorted(root.rglob("*.vsh"))[:20]:
        data = p.read_bytes()[:16]
        print(f"  {p.name:40s} {data.hex()}")


# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="hexdump + zlib test")
    p.add_argument("files", nargs="+", type=Path)

    p = sub.add_parser("zlib", help="decode size-prefixed zlib")
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--out", type=Path)

    p = sub.add_parser("attack", help="known-plaintext stream-cipher attack")
    p.add_argument("file", type=Path)
    p.add_argument("plaintext", help='e.g. "#version 100"')
    p.add_argument("-o", "--out", type=Path)

    p = sub.add_parser("keystream", help="derive XOR keystream and summarize")
    p.add_argument("file", type=Path)
    p.add_argument("plaintext", help='e.g. "#version 100"')

    p = sub.add_parser("romfs", help="extract one file from a RomFS image")
    p.add_argument("romfs", type=Path)
    p.add_argument("inner_path", help='e.g. "files/.depot/Shaders/rr2/2dflat.vsh"')
    p.add_argument("-o", "--out", type=Path)

    p = sub.add_parser("chacha20", help="ChaCha20 decrypt with explicit 32-byte key + 12-byte nonce")
    p.add_argument("file", type=Path)
    p.add_argument("key_hex", help="32-byte key as hex")
    p.add_argument("nonce_hex", help="12-byte nonce as hex")
    p.add_argument("-o", "--out", type=Path)

    p = sub.add_parser("gui-headers", help="show the constant 21-byte header of every .gui file")
    p.add_argument("root", type=Path)

    p = sub.add_parser("batch", help="scan a depot tree")
    p.add_argument("root", type=Path)
    p.add_argument("-o", "--out", type=Path)

    args = ap.parse_args()
    if args.cmd == "probe":
        cmd_probe(args.files)
    elif args.cmd == "zlib":
        cmd_zlib(args.file, args.out)
    elif args.cmd == "attack":
        cmd_attack(args.file, args.plaintext, args.out)
    elif args.cmd == "keystream":
        cmd_keystream(args.file, args.plaintext)
    elif args.cmd == "romfs":
        cmd_romfs(args.romfs, args.inner_path, args.out)
    elif args.cmd == "chacha20":
        cmd_chacha20(args.file, args.key_hex, args.nonce_hex, args.out)
    elif args.cmd == "gui-headers":
        cmd_gui_headers(args.root)
    elif args.cmd == "batch":
        cmd_batch(args.root, args.out)


if __name__ == "__main__":
    main()