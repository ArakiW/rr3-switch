#!/usr/bin/env python3
"""The race-state probe's claims, checked against the guest binary.

The probe (native-switch/source/race_state_probe.cpp) reads the guest's race
state out of guest memory. It may only do that because the guest binary says
so, so THIS TEST is the oracle check: every vtable address and every member
offset the probe uses is re-derived here, from
reverse/native/arm64/libRealRacing3.so, and compared with the constants the
probe actually ships.

Nothing here is a re-implementation of the probe's RUNTIME behaviour; it is
a check that the static claims in the probe's source are true of the guest
image.

What is checked:
  1. The three vtable address points the probe uses really are the vtables
     of the three classes the probe names, through the guest's own Itanium
     RTTI (typeinfo -> __name).
  2. The six member offsets the probe uses really are the offsets the
     guest's own member-registration function registers, with the names the
     probe says.
  3. The probe's source carries those same constants, so the source and the
     binary cannot drift apart silently.
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"
PROBE = ROOT / "source" / "race_state_probe.cpp"
HEADER = ROOT / "include" / "rr3" / "race_state_probe.hpp"

R_AARCH64_RELATIVE = 1027


class Elf:
    def __init__(self, path):
        self.blob = path.read_bytes()
        self.progs = []
        self._parse()

    def _parse(self):
        b = self.blob
        e_phoff, = struct.unpack_from("<Q", b, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, _fl, p_offset, p_vaddr, _pa, p_filesz, _ms, _ = \
                struct.unpack_from("<IIQQQQQQ", b, off)
            self.progs.append((p_type, p_offset, p_vaddr, p_filesz))

    def off_to_va(self, off):
        for (p_type, p_offset, p_vaddr, p_filesz) in self.progs:
            if p_type == 1 and p_offset <= off < p_offset + p_filesz:
                return p_vaddr + (off - p_offset)
        return None

    def va_to_off(self, va):
        for (p_type, p_offset, p_vaddr, p_filesz) in self.progs:
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
        return None

    def section(self, name):
        b = self.blob
        e_shoff, = struct.unpack_from("<Q", b, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", b, 0x3A)
        sh = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, _fl, sh_addr, sh_offset, sh_size,
             _lk, _inf, _al, _es) = struct.unpack_from("<IIQQQQIIQQ", b, off)
            sh.append((sh_name, sh_type, sh_addr, sh_offset, sh_size))
        strtab_off = sh[e_shstrndx][3]
        for (sh_name, sh_type, sh_addr, sh_offset, sh_size) in sh:
            nm = b[strtab_off + sh_name:].split(b"\x00")[0].decode("ascii", "replace")
            if nm == name:
                return (sh_addr, sh_offset, sh_size)
        return None


def relative_relocs(elf):
    rela = elf.section(".rela.dyn")
    out = {}
    if rela is None:
        return out
    _addr, offset, size = rela
    blob = elf.blob
    for i in range(size // 24):
        off = offset + i * 24
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
        if (r_info & 0xFFFFFFFF) == R_AARCH64_RELATIVE:
            out[r_offset] = r_addend
    return out


def cstring(elf, va):
    off = elf.va_to_off(va)
    if off is None:
        return None
    end = elf.blob.find(b"\x00", off)
    if end < 0:
        return None
    try:
        return elf.blob[off:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def class_name_for_vptr(elf, relocs, vptr):
    """The mangled class name of the vtable whose address point is vptr."""
    typeinfo = relocs.get(vptr - 8)
    if typeinfo is None:
        return None
    name_va = relocs.get(typeinfo + 8)
    if name_va is None:
        return None
    return cstring(elf, name_va)


def member_table(elf, lo, hi, base="x19"):
    """(offset, name) pairs the guest registers in [lo, hi)."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

    off = elf.va_to_off(lo)
    code = elf.blob[off:off + (hi - lo)]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    insns = list(md.disasm(code, lo))

    pending_va = {}
    str_at = []
    off_at = []
    for idx, ins in enumerate(insns):
        mn, ops = ins.mnemonic, ins.op_str
        if mn == "adrp":
            m = re.match(r"(x\d+), #(0x[0-9a-f]+|\d+)", ops)
            if m:
                pending_va[m.group(1)] = int(m.group(2), 0)
            continue
        if mn == "add":
            m = re.match(r"(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)", ops)
            if not m:
                continue
            dst, src, imm = m.group(1), m.group(2), int(m.group(3), 0)
            if src in pending_va:
                va = pending_va[src] + imm
                s = cstring(elf, va)
                if s and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", s):
                    str_at.append((idx, s))
                pending_va[dst] = va
                continue
            if src == base:
                off_at.append((idx, imm))
            continue
        if mn == "str" and re.match(r"x\d+, \[x0\], #8", ops):
            # "str x9, [x0], #8" posts the vtable store: the object pointer
            # x0 becomes this+8, so the member that follows is at +0x08.
            off_at.append((idx, 8))
            continue

    rows = []
    for idx, ins in enumerate(insns):
        if ins.mnemonic != "bl":
            continue
        s_hit = None
        o_hit = None
        for j in range(idx - 1, max(-1, idx - 22), -1):
            if s_hit is None:
                for (k, name) in str_at:
                    if k == j:
                        s_hit = (j, name)
                        break
            if o_hit is None:
                for (k, offset) in off_at:
                    if k == j:
                        o_hit = (j, offset)
                        break
            if s_hit and o_hit:
                break
        if not (s_hit and o_hit):
            continue
        if abs(s_hit[0] - o_hit[0]) > 14:
            continue
        key = (s_hit[1], o_hit[1])
        if key not in rows:
            rows.append(key)
    return rows


def main() -> int:
    if not SO.exists():
        print(f"SKIP: the guest binary is not at {SO}")
        return 0

    elf = Elf(SO)
    relocs = relative_relocs(elf)

    # 1. THE THREE CLASSES, through the guest's own RTTI.
    expected_classes = {
        0x1E98168: "8GameMode",
        0x1E4B378: "19RuleSet_RaceMetrics",
        0x1E4B3F8: "27RuleSetModifier_RaceMetrics",
    }
    for vptr, expected in expected_classes.items():
        got = class_name_for_vptr(elf, relocs, vptr)
        assert got == expected, (
            f"vtable {vptr:#x} is {got!r}, expected {expected!r}")

    # 2. THE MEMBER OFFSETS, from the guest's own registration function.
    #    The function is the RuleSetModifier_RaceMetrics constructor.
    rows = member_table(elf, 0xB35A30, 0xB35C60)
    expected_members = {
        "m_nLapDistance": 0x08,
        "m_nTotalLaps": 0x20,
        "m_nPreviousSplineDistance": 0x38,
        "m_nTotalSplineDistanceTravelled": 0x50,
        "m_nTopGameSpeed": 0x68,
        "m_nTotalTime": 0x88,
    }
    for name, offset in expected_members.items():
        pairs = [(n, o) for (n, o) in rows if n == name]
        assert pairs, f"{name} is not registered in the guest's constructor"
        assert offset in [o for (_n, o) in pairs], (
            f"{name} is registered at {[o for (_n, o) in pairs]}, "
            f"expected {offset:#x}")

    # 3. THE PROBE'S SOURCE carries the same constants.
    source = PROBE.read_text(encoding="utf-8")
    source_lower = source.lower()
    for vptr in expected_classes:
        assert f"0x{vptr:x}ull" in source_lower, (
            f"the probe does not carry the vtable {vptr:#x}")
    for name, offset in expected_members.items():
        assert name in source, f"the probe does not carry the member {name}"
        assert f"0x{offset:02x}ull" in source_lower, (
            f"the probe does not carry the offset {offset:#x} of {name}")
    header = HEADER.read_text(encoding="utf-8")
    assert "27RuleSetModifier_RaceMetrics" in header

    print("PASS: the race-state probe's vtables and member offsets agree with "
          "the guest binary "
          f"({len(expected_classes)} classes, {len(expected_members)} members)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
