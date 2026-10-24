#!/usr/bin/env python3
"""The guest RTTI resolver's constants, checked against the guest binary.

native-switch/source/guest_rtti.cpp resolves, at runtime, the guest's C++
class name of an object whose vptr is known, using the guest's own Itanium
RTTI:

    [vptr - 8]      -> the typeinfo object
    [typeinfo + 8]  -> the __name C string (the mangled class name)

The module ships that resolution together with a set of address ranges.
Those ranges are only useful if they are true of the guest image, so THIS
TEST is the oracle: every range and every RTTI claim is re-derived here,
from reverse/native/arm64/libRealRacing3.so, and compared with what the
shipped header says.

The important check is the vptr region. The previous version of this port
used 0x1f6d280 as the upper bound of the vtable range (see
source/race_state_probe.cpp, kVtableSectionHi). The guest's "8GameMode"
entry lives past that bound, so the old range silently rejected a real
vptr. The positive control below pins that entry on purpose: if the
enumeration ever goes empty, or the range ever shrinks again, this test
fails.

What is checked:
  1. The data and bss ranges are the guest's real PT_LOAD segments.
  2. Every vtable in the ELF -- found through the guest's own
     R_AARCH64_RELATIVE relocations -- has its address point inside the
     vptr region, and the "8GameMode" entry that the old bound missed is
     in there too.
  3. The RTTI convention ([vptr-8] -> typeinfo -> [typeinfo+8] -> name)
     holds for the classes the module is expected to resolve.
  4. Exactly thirteen entries carry the "8GameMode" typeinfo.
  5. The shipped header carries the same constants.
  6. The build globs source/*.cpp, so the module cannot be left out of the
     NRO.
"""

import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT.parent
SO = PROJECT / "reverse" / "native" / "arm64" / "libRealRacing3.so"
HEADER = ROOT / "include" / "rr3" / "guest_rtti.hpp"
MAKEFILE = ROOT / "Makefile"

R_AARCH64_RELATIVE = 1027
PT_LOAD = 1
PF_W = 0x2

# The constants the module ships. These are the SPEC this test checks; the
# values are re-derived from the ELF below and must agree with them.
EXPECTED_CONSTANTS = (
    ("kGuestVptrRegionLo", 0x1E1AAE0),
    ("kGuestVptrRegionHi", 0x1F7F880),
    ("kGuestDataSegmentLo", 0x1E1AAE0),
    ("kGuestDataSegmentHi", 0x1F7F880),
    ("kGuestBssSegmentLo", 0x1F80880),
    ("kGuestBssSegmentHi", 0x1FC7A99),
)

# The upper bound the previous version of this port used for the vtable
# range (native-switch/source/race_state_probe.cpp, kVtableSectionHi).
OLD_VPTR_BOUND = 0x1F6D280

# The "8GameMode" entry that the old bound missed.
#
# In the guest image the slot at 0x1f6d6a8 holds the GameMode typeinfo, so
# the Itanium address point of that entry is 0x1f6d6b0. The slot at
# 0x1f6d6a0 is the word in front of the typeinfo slot; both are past the old
# 0x1f6d280 bound, which is the whole point of the control.
GAMEMODE_HIGH_TYPEINFO_SLOT = 0x1F6D6A8
GAMEMODE_HIGH_ADDRESS_POINT = 0x1F6D6B0
GAMEMODE_HIGH_WORD_BEFORE = 0x1F6D6A0

# An Itanium mangled type name is a length-prefixed source name ("8GameMode")
# or a mangled function name ("_ZN3EAE..."). Anything else in a __name slot
# is not RTTI.
MANGLED_NAME = re.compile(r"^(_Z[A-Za-z0-9_]*|[0-9]+[A-Za-z_][A-Za-z0-9_]*)$")


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
            (p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, p_memsz,
             _al) = struct.unpack_from("<IIQQQQQQ", b, off)
            self.progs.append((p_type, p_flags, p_offset, p_vaddr, p_filesz,
                               p_memsz))

    def loads(self):
        """The PT_LOAD segments, in the order the ELF lists them."""
        return [p for p in self.progs if p[0] == PT_LOAD]

    def segment_of(self, va):
        """The PT_LOAD segment holding va, or None."""
        for p in self.progs:
            if p[0] == PT_LOAD and p[3] <= va < p[3] + p[5]:
                return p
        return None

    def va_to_off(self, va):
        for (p_type, _fl, p_offset, p_vaddr, p_filesz, _ms) in self.progs:
            if p_type == PT_LOAD and p_vaddr <= va < p_vaddr + p_filesz:
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
            nm = b[strtab_off + sh_name:].split(b"\x00")[0].decode("ascii",
                                                                 "replace")
            if nm == name:
                return (sh_addr, sh_offset, sh_size)
        return None


def relocations(elf):
    """-> {r_offset: (r_type, r_addend)} for every .rela.dyn entry."""
    out = {}
    rela = elf.section(".rela.dyn")
    if rela is None:
        return out
    _addr, offset, size = rela
    blob = elf.blob
    for i in range(size // 24):
        off = offset + i * 24
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
        out[r_offset] = (r_info & 0xFFFFFFFF, r_addend)
    return out


def relative_relocs(elf):
    """-> {r_offset: r_addend} for every R_AARCH64_RELATIVE relocation."""
    return {off: addend
            for (off, (rtype, addend)) in relocations(elf).items()
            if rtype == R_AARCH64_RELATIVE}


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


def header_constant(text, name, value):
    """True when the header declares `name` with the hex value `value`."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if name not in line:
            continue
        # A declaration may wrap onto the next line, so look at both.
        window = " ".join(lines[i:i + 2])
        for m in re.finditer(r"0x([0-9a-fA-F]+)", window):
            if int(m.group(1), 16) == value:
                return True
    return False


def main() -> int:
    if not SO.exists():
        print(f"SKIP: the guest binary is not at {SO}")
        return 0

    elf = Elf(SO)
    all_relocs = relocations(elf)
    relocs = relative_relocs(elf)
    loads = elf.loads()

    # 1. THE SEGMENT RANGES COME FROM THE ELF.
    assert len(loads) == 3, (
        f"the guest has {len(loads)} PT_LOAD segments, expected 3")

    seg_data = loads[1]
    seg_bss = loads[2]
    data_lo, data_hi = seg_data[3], seg_data[3] + seg_data[5]
    bss_lo, bss_hi = seg_bss[3], seg_bss[3] + seg_bss[5]

    assert data_lo == 0x1E1AAE0, (
        f"PT_LOAD[1] starts at {data_lo:#x}, expected 0x1e1aae0")
    assert data_hi == 0x1F7F880, (
        f"PT_LOAD[1] ends at {data_hi:#x}, expected 0x1f7f880")
    assert bss_lo == 0x1F80880, (
        f"PT_LOAD[2] starts at {bss_lo:#x}, expected 0x1f80880")
    assert bss_hi == 0x1FC7A99, (
        f"PT_LOAD[2] ends at {bss_hi:#x}, expected 0x1fc7a99")
    assert seg_data[1] & PF_W, (
        f"PT_LOAD[1] ({data_lo:#x}-{data_hi:#x}) is not writable, expected RW")
    assert seg_bss[1] & PF_W, (
        f"PT_LOAD[2] ({bss_lo:#x}-{bss_hi:#x}) is not writable, expected RW")
    assert not (loads[0][1] & PF_W), (
        f"PT_LOAD[0] ({loads[0][3]:#x}) is writable, expected not writable")

    # 2. EVERY VTABLE'S ADDRESS POINT IS INSIDE THE VPTR REGION.
    vtables = enumerate_vtables(elf, relocs)
    vptr_lo, vptr_hi = 0x1E1AAE0, 0x1F7F880
    assert len(vtables) > 1000, (
        f"only {len(vtables)} vtables were enumerated, expected well over "
        "1000; the enumeration is broken or the guest binary changed")

    outside = sorted(v for v in vtables if not (vptr_lo <= v < vptr_hi))
    assert not outside, (
        f"{len(outside)} vtable address points fall outside the vptr region "
        f"[{vptr_lo:#x}, {vptr_hi:#x}), first ones: "
        + ", ".join(f"{v:#x}" for v in outside[:8]))

    # 2b. THE POSITIVE CONTROL: the GameMode entry past the old bound.
    game_mode_typeinfo = relocs.get(0x1E98168 - 8)
    assert game_mode_typeinfo is not None, (
        f"no R_AARCH64_RELATIVE relocation at the typeinfo slot "
        f"{0x1E98168 - 8:#x} of the GameMode vtable")

    high_typeinfo = relocs.get(GAMEMODE_HIGH_TYPEINFO_SLOT)
    assert high_typeinfo is not None, (
        f"no R_AARCH64_RELATIVE relocation at the typeinfo slot "
        f"{GAMEMODE_HIGH_TYPEINFO_SLOT:#x}")
    assert high_typeinfo == game_mode_typeinfo, (
        f"the typeinfo at {GAMEMODE_HIGH_TYPEINFO_SLOT:#x} is "
        f"{high_typeinfo:#x}, expected the GameMode typeinfo "
        f"{game_mode_typeinfo:#x}")

    assert GAMEMODE_HIGH_ADDRESS_POINT in vtables, (
        f"the GameMode address point {GAMEMODE_HIGH_ADDRESS_POINT:#x} is not "
        f"in the enumerated set of {len(vtables)} vtables")
    assert vtables[GAMEMODE_HIGH_ADDRESS_POINT][1] == "8GameMode", (
        f"the entry at {GAMEMODE_HIGH_ADDRESS_POINT:#x} is "
        f"{vtables[GAMEMODE_HIGH_ADDRESS_POINT][1]!r}, expected '8GameMode'")

    # The old bound must NOT have covered this entry: that is the bug.
    assert GAMEMODE_HIGH_ADDRESS_POINT > OLD_VPTR_BOUND, (
        f"the GameMode address point {GAMEMODE_HIGH_ADDRESS_POINT:#x} is not "
        f"past the old bound {OLD_VPTR_BOUND:#x}, so the control no longer "
        "proves anything")
    assert GAMEMODE_HIGH_WORD_BEFORE > OLD_VPTR_BOUND, (
        f"the word before the GameMode typeinfo slot "
        f"{GAMEMODE_HIGH_WORD_BEFORE:#x} is not past the old bound "
        f"{OLD_VPTR_BOUND:#x}")
    assert vptr_lo <= GAMEMODE_HIGH_ADDRESS_POINT < vptr_hi, (
        f"the GameMode address point {GAMEMODE_HIGH_ADDRESS_POINT:#x} is "
        f"outside the vptr region [{vptr_lo:#x}, {vptr_hi:#x})")

    # 3. THE RTTI CONVENTION, for known classes.
    expected_classes = {
        0x1E98168: "8GameMode",
        0x1E4B378: "19RuleSet_RaceMetrics",
        0x1E4B3F8: "27RuleSetModifier_RaceMetrics",
    }
    for vptr, expected in expected_classes.items():
        slot = vptr - 8
        entry = all_relocs.get(slot)
        assert entry is not None, (
            f"the typeinfo slot {slot:#x} of the vtable {vptr:#x} has no "
            f"relocation at all, expected an R_AARCH64_RELATIVE one")
        assert entry[0] == R_AARCH64_RELATIVE, (
            f"the typeinfo slot {slot:#x} of the vtable {vptr:#x} is "
            f"relocation type {entry[0]}, expected {R_AARCH64_RELATIVE} "
            "(R_AARCH64_RELATIVE)")

        typeinfo = relocs[slot]
        name_va = relocs.get(typeinfo + 8)
        assert name_va is not None, (
            f"the typeinfo {typeinfo:#x} of the vtable {vptr:#x} has no "
            f"relocation at its __name slot {typeinfo + 8:#x}")
        name = cstring(elf, name_va)
        assert name == expected, (
            f"the vtable {vptr:#x} resolves to {name!r}, expected "
            f"{expected!r}")

        seg = elf.segment_of(name_va)
        assert seg is not None, (
            f"the name string of the vtable {vptr:#x} lives at {name_va:#x}, "
            "which is in no PT_LOAD segment")
        assert not (seg[1] & PF_W), (
            f"the name string of the vtable {vptr:#x} lives at {name_va:#x} "
            f"in the writable segment {seg[3]:#x}-{seg[3] + seg[5]:#x}, "
            "expected a read-only segment")

    # 4. THE GAME MODE FAMILY.
    game_mode = sorted(v for (v, (_ti, name)) in vtables.items()
                       if name == "8GameMode")
    assert len(game_mode) == 13, (
        f"{len(game_mode)} entries carry the '8GameMode' typeinfo, expected "
        "13; the guest binary changed or the enumeration is broken")

    # 5. THE HEADER CARRIES THE CONSTANTS.
    assert HEADER.exists(), (
        f"the guest RTTI header is missing: {HEADER} does not exist, so the "
        "module cannot ship the constants this test checks")
    header = HEADER.read_text(encoding="utf-8")
    for name, value in EXPECTED_CONSTANTS:
        assert name in header, (
            f"the guest RTTI header does not declare {name}")
        assert header_constant(header, name, value), (
            f"the guest RTTI header declares {name} with a value other than "
            f"{value:#x}")

    # 6. THE BUILD PICKS UP EVERY source/*.cpp.
    assert MAKEFILE.exists(), f"the Makefile is missing: {MAKEFILE}"
    makefile = MAKEFILE.read_text(encoding="utf-8")
    wildcard_lines = [l for l in makefile.splitlines()
                      if "wildcard" in l and "*.cpp" in l]
    assert wildcard_lines, (
        "the Makefile does not glob *.cpp, so a new source/*.cpp could be "
        "silently left out of the build")
    assert any("SOURCES" in l for l in wildcard_lines), (
        "the Makefile's *.cpp wildcard is not driven by SOURCES: "
        + "; ".join(wildcard_lines))
    m = re.search(r"^\s*SOURCES\s*:?=\s*(.*)$", makefile, re.M)
    assert m is not None, "the Makefile has no SOURCES definition"
    source_dirs = m.group(1).split()
    assert "source" in source_dirs, (
        f"the Makefile's SOURCES does not include 'source': {source_dirs}")

    game_mode_list = ", ".join(f"{v:#x}" for v in game_mode)
    print("PASS: the guest RTTI ranges and conventions agree with the guest "
          f"binary (data {data_lo:#x}-{data_hi:#x}, bss {bss_lo:#x}-"
          f"{bss_hi:#x}, vptr region {vptr_lo:#x}-{vptr_hi:#x}, "
          f"{len(vtables)} vtables, {len(game_mode)} '8GameMode' entries at "
          f"{game_mode_list})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
