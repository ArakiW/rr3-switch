#!/usr/bin/env python3
"""RR3 depot asset decoder / crypto analyzer.

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
   Suspect: data/*.nct, data/*.ncm (libNimble.so may own this codec).
   Header observation: every checked *.vsh shares bytes[2:6] == 31 42 3a 44.
     2dflat.vsh            1c 38 31 42 3a 44 c1 ca 82 2f f4 01 20 76 ff 00
     full_screen_effects   d1 37 31 42 3a 44 89 12 1e ca ed 77 24 4f 6e 15

asset_list_startup.txt format:  path <TAB> md5 <TAB> disk_size <TAB> uncompressed_size
  disk == uncompressed  →  family C (or already-decompressed A)
  disk <  uncompressed  →  family B (zlib)

Usage:
  python decode_rr3_depot.py probe  <file>...
  python decode_rr3_depot.py zlib   <file> [-o out]
  python decode_rr3_depot.py attack <encrypted> <known_plaintext_prefix> [-o out]
  python decode_rr3_depot.py batch  <depot_root> [-o outdir]
  python decode_rr3_depot.py romfs  <core.romfs> <inner_path> [-o out]
  python decode_rr3_depot.py keystream <encrypted> <known_plaintext_prefix>
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
    """Heuristic 0..1 score for looking like GLSL source."""
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
    """GUI processed files have a small fixed header (often script-like).
    Look for: balanced quotes, key=value lines, common gui tokens."""
    if not data:
        return 0.0
    text = data.decode("latin-1", errors="replace")
    needles = [
        "screen", "view", "button", "label", "image", "panel",
        "LoadingScreen", "title", "background",
        "font", "color", "size", "x", "y",
    ]
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
    """Each plaintext byte = ct[i] ^ state; state = (state + ct[i]) & 0xFF."""
    out = bytearray(len(data))
    state = seed
    for i, b in enumerate(data):
        out[i] = b ^ state
        state = (state + b) & 0xFF
    return bytes(out)


def xor_rolling_pt(data: bytes, seed: int) -> bytes:
    """pt[i] = ct[i] ^ state; state = (state + pt[i]) & 0xFF."""
    out = bytearray(len(data))
    state = seed
    for i, b in enumerate(data):
        p = b ^ state
        out[i] = p
        state = (state + p) & 0xFF
    return bytes(out)


def xor_prev_ct(data: bytes, key: int) -> bytes:
    """pt[0]=ct[0]^key, pt[i]=ct[i]^ct[i-1]^key."""
    if not data:
        return b""
    out = bytearray(len(data))
    out[0] = data[0] ^ key
    for i in range(1, len(data)):
        out[i] = data[i] ^ data[i - 1] ^ key
    return bytes(out)


def xor_prev_pt(data: bytes, key: int) -> bytes:
    """pt[i] = ct[i] ^ pt[i-1] ^ key  (pt[-1] treated as key)."""
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


def rc4(data: bytes, key: bytes) -> bytes:
    """Standard RC4."""
    S = list(range(256))
    j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 0xFF
        S[i], S[j] = S[j], S[i]
    out = bytearray(len(data))
    i = j = 0
    for n in range(len(data)):
        i = (i + 1) & 0xFF
        j = (j + S[i]) & 0xFF
        S[i], S[j] = S[j], S[i]
        out[n] = data[n] ^ S[(S[i] + S[j]) & 0xFF]
    return bytes(out)


def rc4_drop(data: bytes, key: bytes, drop: int = 0) -> bytes:
    """RC4 with optional keystream skip (defeats Fluhrer/Mantin Shamir)."""
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
    """pt[i] = ct[i] ^ ((base + i*step) & 0xFF)."""
    return bytes((b ^ ((base + i * step) & 0xFF)) & 0xFF for i, b in enumerate(data))


def xor_with_header_key(data: bytes, header: bytes) -> bytes:
    """Key = first len(header) bytes of ct used as repeating key.
    Used when a file header might contain the XOR key."""
    if not header:
        return data
    return bytes(b ^ header[i % len(header)] for i, b in enumerate(data))


def tea_decrypt_block(v0: int, v1: int, key: list[int], rounds: int = 32) -> tuple[int, int]:
    delta = 0x9E3779B9
    s = (delta * rounds) & 0xFFFFFFFF
    for _ in range(rounds):
        v1 = (v1 - (((v0 << 4) + key[2]) ^ (v0 + s) ^ ((v0 >> 5) + key[3]))) & 0xFFFFFFFF
        v0 = (v0 - (((v1 << 4) + key[0]) ^ (v1 + s) ^ ((v1 >> 5) + key[1]))) & 0xFFFFFFFF
        s = (s - delta) & 0xFFFFFFFF
    return v0, v1


def tea_decrypt(data: bytes, key_bytes: bytes, rounds: int = 32) -> bytes | None:
    """TEA/XXTEA-ish ECB over 8-byte blocks (skip first 0/4/6/8 byte header)."""
    if len(key_bytes) != 16:
        return None
    key = list(struct.unpack("<4I", key_bytes))
    out = bytearray()
    for off in range(0, len(data) - 7, 8):
        v0, v1 = struct.unpack_from("<II", data, off)
        d0, d1 = tea_decrypt_block(v0, v1, key, rounds)
        out += struct.pack("<II", d0, d1)
    out += data[len(out):]
    return bytes(out)


# ---------------------------------------------------------------------------
# known-plaintext attack
# ---------------------------------------------------------------------------

def derive_xor_keystream(ct: bytes, pt: bytes) -> bytes:
    n = min(len(ct), len(pt))
    return bytes(ct[i] ^ pt[i] for i in range(n))


def analyze_keystream(ks: bytes) -> dict:
    """Summarize properties of a derived keystream to help identify the cipher."""
    info = {
        "len": len(ks),
        "unique": len(set(ks)),
        "first16": ks[:16].hex(),
        "min": min(ks) if ks else None,
        "max": max(ks) if ks else None,
    }
    if len(ks) >= 16:
        # constant?
        info["is_const"] = len(set(ks)) == 1
        # repeating 4?
        for klen in (1, 2, 3, 4, 5, 6, 7, 8, 16):
            ok = True
            for i in range(klen, len(ks)):
                if ks[i] != ks[i % klen]:
                    ok = False
                    break
            if ok:
                info[f"repeat_{klen}"] = True
                info["key"] = ks[:klen].hex()
        # position-based? ks[i] = (a*i + b) mod 256 ?
        for step in (1, 2, 3):
            diffs = [(ks[i+1] - ks[i]) & 0xFF for i in range(min(15, len(ks)-1))]
            if len(set(diffs)) == 1:
                info[f"linear_step_{step}"] = True
    return info


def attack(encrypted: bytes, plaintext_prefix: bytes, skip: int = 0) -> list[tuple[str, bytes, float]]:
    """Try every scheme; return (name, decoded, score) sorted by score desc."""
    body = encrypted[skip:] if skip else encrypted
    results: list[tuple[str, bytes, float]] = []

    is_glsl = b"#" in plaintext_prefix or b"version" in plaintext_prefix
    scorer = (lambda d: score_glsl(d)) if is_glsl else (lambda d: score_gui(d) if b"screen" in plaintext_prefix.lower() or b"gui" in plaintext_prefix.lower() else score_text(d))

    # 1. constant XOR — derive key from known plaintext
    if plaintext_prefix:
        key = body[0] ^ plaintext_prefix[0]
        cand = xor_const(body, key)
        results.append((f"xor_const(0x{key:02x})", cand, scorer(cand)))

    # 2. repeating XOR — recover key length by checking periodicity of keystream
    ks = derive_xor_keystream(body, plaintext_prefix)
    if len(ks) >= 8:
        for klen in range(1, min(64, len(ks) + 1)):
            if all(ks[i] == ks[i % klen] for i in range(len(ks))):
                key = ks[:klen]
                cand = xor_repeating(body, key)
                results.append((f"xor_repeat(key={key.hex()})", cand, scorer(cand)))
                break

    # 3. rolling XOR with each seed
    for seed in range(256):
        cand = xor_rolling(body, seed)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_rolling(seed=0x{seed:02x})", cand, sc))
    # 3b. rolling XOR using plaintext feedback
    for seed in range(256):
        cand = xor_rolling_pt(body, seed)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_rolling_pt(seed=0x{seed:02x})", cand, sc))

    # 4. prev-ct XOR
    for k in range(256):
        cand = xor_prev_ct(body, k)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_prev_ct(k=0x{k:02x})", cand, sc))

    # 5. prev-pt XOR
    for k in range(256):
        cand = xor_prev_pt(body, k)
        sc = scorer(cand)
        if sc > 0.5:
            results.append((f"xor_prev_pt(k=0x{k:02x})", cand, sc))

    # 6. add / sub
    if plaintext_prefix:
        delta = (plaintext_prefix[0] - body[0]) & 0xFF
        cand = add_const(body, delta)
        results.append((f"add_const(+0x{delta:02x})", cand, scorer(cand)))
        cand = sub_const(body, delta)
        results.append((f"sub_const(-0x{delta:02x})", cand, scorer(cand)))

    # 7. nibble swap / bit not
    cand = nibble_swap(body)
    results.append(("nibble_swap", cand, scorer(cand)))
    cand = bit_not(body)
    results.append(("bit_not", cand, scorer(cand)))

    # 8. position-based XOR (linear step)
    for step in (1, 2, 3):
        for base in range(0, 256, 8 if step != 1 else 1):
            cand = xor_with_position(body, base, step)
            sc = scorer(cand)
            if sc > 0.6:
                results.append((f"xor_pos(base=0x{base:02x},step={step})", cand, sc))

    # 9. RC4 with short ASCII keys + common RR3 strings
    candidate_keys = [
        b"RR3", b"rr3", b"Firemint", b"Firemonkeys", b"Cloudcell",
        b"depot", b".depot", b"shader", b"EA", b"glu", b"csdk",
        b"RealRacing", b"realracing", b"vsh", b"fsh", b"gui",
        b"MT_GL_ES_3", b"MT_GL_ES_2",
        b"\x1b:D", b"1B:D", b"vsh1", b"vsh0", b"shad", b"GL\x00",
        b"\x00" * 4, b"\x00" * 8, b"\x00" * 16,
        b"12345678", b"abcdefgh",
    ]
    for key in candidate_keys:
        for drop in (0, 256, 512, 1024, 1536, 2048, 3072):
            cand = rc4_drop(body, key, drop) if drop else rc4(body, key)
            sc = scorer(cand)
            if sc > 0.4:
                tag = f"rc4({key!r})" + (f"+drop{drop}" if drop else "")
                results.append((tag, cand, sc))

    # 10. XOR with the file's own first N bytes as key (header-as-key)
    for n in (4, 6, 8, 12, 16):
        if n <= len(encrypted):
            cand = xor_with_header_key(encrypted, encrypted[:n])
            results.append((f"xor_header({n})", cand[skip:] if skip else cand, scorer(cand[skip:] if skip else cand)))

    # 11. TEA with zero / simple keys
    for kb in (b"\x00" * 16, b"RR3\0" + b"\x00" * 12,
               b"Firemonkeys!!\x00\x00", b"depot\x00" + b"\x00" * 10):
        cand = tea_decrypt(body, kb)
        if cand:
            sc = scorer(cand)
            if sc > 0.3:
                results.append((f"tea({kb[:8]!r})", cand, sc))

    # 12. try zlib after each promising decode
    extra: list[tuple[str, bytes, float]] = []
    for name, cand, sc in results:
        z = try_zlib(cand)
        if z is not None:
            extra.append((name + "+zlib", z, max(sc, score_text(z))))
    results.extend(extra)

    results.sort(key=lambda t: t[2], reverse=True)
    return results


# ---------------------------------------------------------------------------
# RomFS reader (matches build_romfs layout)
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

    def file(self, offset: int) -> tuple[int, int, str]:
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
        """Walk directory tree by '/' segments; return file-table offset or None."""
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
        """Locate the file inside the RomFS image and return its content."""
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
            # check shared magic
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

    # try skip 0..16 (header sizes)
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
        # write the top candidate
        out.write_bytes(best[0][1])
        print(f"\nwrote top candidate to {out}")


def cmd_keystream(path: Path, plaintext: str) -> None:
    enc = path.read_bytes()
    pt = plaintext.encode("latin-1")
    ks = derive_xor_keystream(enc, pt)
    info = analyze_keystream(ks)
    print(f"encrypted: {len(enc)} bytes")
    print(f"plaintext prefix: {pt!r} ({len(pt)} bytes)")
    print(f"derived keystream: {info.get('first16', '?')}")
    print(f"  unique bytes: {info.get('unique')} / 256")
    print(f"  min/max: {info.get('min'):#04x} / {info.get('max'):#04x}")
    print(f"  is constant: {info.get('is_const')}")
    for k in sorted(info):
        if k.startswith("repeat_") and info[k]:
            print(f"  repeating period {k.split('_')[1]}: key={info.get('key')}")
        if k.startswith("linear_step_") and info[k]:
            print(f"  linear step detected: {k}")


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


def cmd_batch(root: Path, outdir: Path | None) -> None:
    """Probe every .vsh/.fsh/.gui under root; also zlib-decode .bin/.atlas."""
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

    # zlib-decode everything that works
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

    # report vsh headers
    print("\n*.vsh headers:")
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
    elif args.cmd == "batch":
        cmd_batch(args.root, args.out)


if __name__ == "__main__":
    main()