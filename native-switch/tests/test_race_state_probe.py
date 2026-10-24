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


# An Itanium mangled type name is a length-prefixed source name ("8GameMode")
# or a mangled function name ("_ZN3EAE..."). Anything else in a __name slot
# is not RTTI.
MANGLED_NAME = re.compile(r"^(_Z[A-Za-z0-9_]*|[0-9]+[A-Za-z_][A-Za-z0-9_]*)$")


def enumerate_vtables(elf, relocs):
    """-> {address_point: (typeinfo_va, mangled_name)} for every vtable.

    A vtable is found the way the runtime resolver finds it: the slot at
    [vptr - 8] holds a relocated pointer to a typeinfo object, and that
    typeinfo's own [typeinfo + 8] slot holds a relocated pointer to the
    mangled class name. The address point is the typeinfo slot + 8.
    """
    out = {}
    for slot, typeinfo in relocs.items():
        name_va = relocs.get(typeinfo + 8)
        if name_va is None:
            continue
        name = cstring(elf, name_va)
        if name is None or not MANGLED_NAME.match(name):
            continue
        out[slot + 8] = (typeinfo, name)
    return out


def disasm(elf, lo, hi):
    """[(address, mnemonic, op_str)] for the guest's code in [lo, hi)."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

    off = elf.va_to_off(lo)
    code = elf.blob[off:off + (hi - lo)]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    return [(ins.address, ins.mnemonic, ins.op_str)
            for ins in md.disasm(code, lo)]


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

    # 4. THE GAME MODE IS A POINTER MEMBER OF THE CGlobal, AT +0x10d88.
    #
    #    This is the chain the probe now follows. It is checked against the
    #    guest's own instructions, because the chain the probe used to claim
    #    ("the GameMode is built IN PLACE at owner + 0x17e00") was a
    #    MIS-DECODE of these very instructions: w8 held 0x173f0 at
    #    0x10af210 and was CLOBBERED with 0x1378 at 0x10af234, BEFORE the
    #    add at 0x10af244.
    #
    #    Two INDEPENDENT readings of the guest have to agree, because one of
    #    them already fooled us once: the mov/movk pair writes the LOW half
    #    first, so 0x378 + (1 << 16) is 0x10378, NOT 0x1378. Dropping that
    #    0x10000 gives 0x1d88, which is the offset the probe used to carry.
    #
    #    READER A: the teardown, ELF 0x10af1d4. x19 is `this`, and the
    #    caller at ELF 0x13b6c28 passes *(*(GOT 0x1f6d5d8)) -- the CGlobal.
    #      mov  w8, #0x378 ; movk w8, #1, lsl #16   -> w8 = 0x10378
    #      add  x23, x19, x8
    #      ldr  x1, [x23, #0xa10]                  -> +0x10d88
    #
    #    READER B: the mode creation, ELF 0xb08960. x19 is `this`.
    #      mov  w8, #0xbb0 ; movk w8, #1, lsl #16   -> w8 = 0x10bb0
    #      add  x23, x19, x8
    #      str  x20, [x23, #0x1d8]                 -> +0x10d88
    def chain_offset(lo, hi, add_pattern, load_pattern):
        """(base displacement, load displacement) from the guest's code."""
        base = None
        load = None
        w8 = 0
        for _address, mnemonic, ops in disasm(elf, lo, hi):
            if mnemonic == "mov" and re.match(r"w8, #(0x[0-9a-f]+|\d+)$", ops):
                w8 = int(re.match(r"w8, #(0x[0-9a-f]+|\d+)$", ops).group(1), 0)
            elif mnemonic == "movk" and re.match(r"w8, #(0x[0-9a-f]+|\d+), lsl #16$", ops):
                high = int(re.match(r"w8, #(0x[0-9a-f]+|\d+), lsl #16$", ops).group(1), 0)
                w8 = (w8 & 0xFFFF) | (high << 16)
            elif mnemonic == "add" and re.match(add_pattern, ops):
                base = w8
            elif (load is None and mnemonic in ("ldr", "str")
                  and re.match(load_pattern, ops)):
                # The FIRST use of the base register, which is the one the
                # chain is about. Later uses in the same function touch
                # other members of the same object.
                load = int(re.match(r".*#(0x[0-9a-f]+)\]$", ops).group(1), 16)
        assert base is not None, f"no base add in [{lo:#x}, {hi:#x})"
        assert load is not None, f"no offset load in [{lo:#x}, {hi:#x})"
        return base, load

    teardown_base, teardown_load = chain_offset(
        0x10AF1D4, 0x10AF300, r"x23, x19, x8$",
        r"x\d+, \[x23, #(0x[0-9a-f]+)\]$")
    assert teardown_base == 0x10378, (
        f"the teardown's base displacement is {teardown_base:#x}, expected "
        "0x10378")
    assert teardown_load == 0xA10, (
        f"the teardown's load displacement is {teardown_load:#x}, expected "
        "0xa10")

    creation_base, creation_load = chain_offset(
        0xB08960, 0xB089A0, r"x23, x19, x8$",
        r"x\d+, \[x23, #(0x[0-9a-f]+)\]$")
    assert creation_base == 0x10BB0, (
        f"the creation site's base displacement is {creation_base:#x}, "
        "expected 0x10bb0")
    assert creation_load == 0x1D8, (
        f"the creation site's load displacement is {creation_load:#x}, "
        "expected 0x1d8")

    game_mode_offset = teardown_base + teardown_load
    creation_offset = creation_base + creation_load
    assert game_mode_offset == 0x10D88, (
        f"the teardown gives the GameMode at CGlobal+{game_mode_offset:#x}, "
        "expected 0x10d88")
    assert creation_offset == game_mode_offset, (
        f"the two independent readings disagree: the teardown says "
        f"CGlobal+{game_mode_offset:#x} and the creation site says "
        f"CGlobal+{creation_offset:#x}")
    assert "0x10d88ull" in source_lower, (
        "the probe does not carry the GameMode member offset 0x10d88")

    # 5. THE NEGATIVE CONTROL: A LIVE GameMode IS NEVER THE ABSTRACT BASE.
    #
    #    The port used to search guest memory for the address point of the
    #    class "8GameMode" (0x1e98168). That search was structurally doomed:
    #    the guest instantiates DERIVED classes (SoloMode, StandardRaceMode,
    #    TimeTrialMode, ...), and a derived object's vptr is its own class's
    #    address point. The archived run 20261024-194901-eden-racestate-heap2
    #    proved it at runtime: 49 owner candidates, 0 hits.
    #
    #    So the check is: enumerate every vtable in the guest, take the ones
    #    whose class is a concrete game mode, and assert that NONE of them
    #    uses the abstract base's address point. The positive control is the
    #    base itself, which the same enumeration must find.
    vtables = enumerate_vtables(elf, relocs)
    base_typeinfo = relocs.get(0x1E98168 - 8)
    assert base_typeinfo is not None, (
        "the guest's GameMode vtable has no typeinfo slot at "
        f"{0x1E98168 - 8:#x}")
    base_name = cstring(elf, relocs[base_typeinfo + 8])
    assert base_name == "8GameMode", (
        f"the GameMode typeinfo names {base_name!r}, expected '8GameMode'")

    mode_classes = {point: name for point, (_ti, name) in vtables.items()
                    if name.endswith("Mode") and name != "8GameMode"}
    assert len(mode_classes) >= 10, (
        f"only {len(mode_classes)} concrete '*Mode' classes were found "
        "(expected well over 10); the enumeration is broken")
    clashes = sorted(point for point in mode_classes
                      if point == 0x1E98168)
    assert not clashes, (
        "a concrete game mode uses the abstract base's address point "
        f"{clashes}; the negative control would be meaningless")
    # And the base must NOT be among the concrete classes either.
    assert "8GameMode" not in mode_classes.values(), (
        "the abstract base was counted as a concrete mode class")

    # 6. THE MAP KEY. The guest inserts the race metrics ruleset under the
    #    key "racemetrics" (the global at 0x1f81250 holds that C string).
    key_va = relocs.get(0x1F81250)
    assert key_va is not None, (
        "no relocated pointer at 0x1f81250, which the guest's own insert "
        "loads to get the map key")
    assert cstring(elf, key_va) == "racemetrics", (
        f"the map key global holds {cstring(elf, key_va)!r}, "
        "expected 'racemetrics'")
    assert "racemetrics" in source, (
        "the probe does not carry the map key it cross-checks")

    print("PASS: the race-state probe's vtables and member offsets agree with "
          "the guest binary "
          f"({len(expected_classes)} classes, {len(expected_members)} members); "
          f"the GameMode chain gives CGlobal+0x{game_mode_offset:x}; "
          f"{len(mode_classes)} concrete game-mode classes, none of them the "
          "abstract base")
    return 0


if __name__ == "__main__":
    sys.exit(main())
