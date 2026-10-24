#!/usr/bin/env python3
"""Resolve a guest C++ class's vtable from its RTTI __name string.

The guest binary keeps Itanium C++ ABI RTTI: every polymorphic class has a
__class_type_info object whose __name slot (offset +8) points at the mangled
name, and every vtable has, at offset -8 from the first virtual function, a
pointer to that typeinfo.  Both of those pointers are R_AARCH64_RELATIVE
relocations in .rela.dyn, so the whole chain can be walked from the file
alone -- no runtime needed and no guesswork.

Usage:
    python rtti_vtable.py <so> <mangled-name-or-partial> [--slots N]

Example:
    python rtti_vtable.py libRealRacing3.so N10FeatSystem14CurrentLapFeatE
"""

import argparse
import re
import struct
import sys


class Elf:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.blob = fh.read()
        self.progs = []
        self.sections = []
        self._parse()

    def _parse(self):
        b = self.blob
        e_phoff, = struct.unpack_from("<Q", b, 0x20)
        e_phentsize, e_phnum = struct.unpack_from("<HH", b, 0x36)
        for i in range(e_phnum):
            off = e_phoff + i * e_phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = \
                struct.unpack_from("<IIQQQQQQ", b, off)
            self.progs.append((p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
        e_shoff, = struct.unpack_from("<Q", b, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", b, 0x3A)
        sh = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_addralign, sh_entsize) = \
                struct.unpack_from("<IIQQQQIIQQ", b, off)
            sh.append((sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size))
        strtab_off = sh[e_shstrndx][4]
        for (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size) in sh:
            name = b[strtab_off + sh_name:].split(b"\x00")[0].decode("ascii", "replace")
            self.sections.append((name, sh_type, sh_flags, sh_addr, sh_offset, sh_size))

    def off_to_va(self, off):
        for (p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz) in self.progs:
            if p_type == 1 and p_offset <= off < p_offset + p_filesz:
                return p_vaddr + (off - p_offset)
        return None

    def va_to_off(self, va):
        for (p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz) in self.progs:
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
        return None

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None


R_AARCH64_RELATIVE = 1027


def relative_relocs(elf):
    """-> list of (r_offset, r_addend) for every R_AARCH64_RELATIVE."""
    out = []
    rela = elf.section(".rela.dyn")
    if rela is None:
        return out
    _, _, _, addr, offset, size = rela
    blob = elf.blob
    for i in range(size // 24):
        off = offset + i * 24
        r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, off)
        rtype = r_info & 0xFFFFFFFF
        if rtype == R_AARCH64_RELATIVE:
            out.append((r_offset, r_addend))
    return out


def find_names(elf, pattern):
    rx = re.compile(pattern.encode())
    hits = []
    for m in rx.finditer(elf.blob):
        va = elf.off_to_va(m.start())
        if va is None:
            continue
        # The name must live in a read-only section (the typeinfo __name
        # always points into .rodata / .data.rel.ro).
        hits.append((va, m.group().decode("ascii", "replace")))
    return hits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("name")
    ap.add_argument("--slots", type=int, default=12)
    ap.add_argument("--vtable", type=lambda s: int(s, 0), default=None,
                    help="look the class name up FROM a vtable address")
    args = ap.parse_args()

    elf = Elf(args.so)
    relocs = relative_relocs(elf)
    by_addend = {}
    by_offset = {}
    for r_offset, r_addend in relocs:
        by_addend.setdefault(r_addend, []).append(r_offset)
        by_offset.setdefault(r_offset, []).append(r_addend)

    if args.vtable is not None:
        vt = args.vtable
        addends = by_offset.get(vt + 8)
        if not addends:
            print(f"no typeinfo slot at {vt + 8:#x}")
            return 1
        typeinfo = addends[0]
        name_addends = by_offset.get(typeinfo + 8)
        if not name_addends:
            print(f"no __name slot at {typeinfo + 8:#x}")
            return 1
        off = elf.va_to_off(name_addends[0])
        end = elf.blob.find(b"\x00", off) if off is not None else -1
        name = elf.blob[off:end].decode("ascii", "replace") if off is not None else "?"
        print(f"vtable {vt:#x} -> typeinfo {typeinfo:#x} -> {name}")
        return 0

    names = find_names(elf, args.name)
    if not names:
        print("no RTTI name matched")
        return 1

    for name_va, name_text in names:
        print(f"# name {name_text!r} at va {name_va:#x}")
        slots = by_addend.get(name_va, [])
        if not slots:
            print("  no relocation points at that name (not an RTTI __name)")
            continue
        for name_slot in slots:
            typeinfo = name_slot - 8
            print(f"  typeinfo va {typeinfo:#x} (__name slot {name_slot:#x})")
            vslots = by_addend.get(typeinfo, [])
            if not vslots:
                print("    no vtable points at that typeinfo")
                continue
            for vslot in vslots:
                vtable = vslot - 8
                print(f"    vtable va {vtable:#x} (typeinfo slot {vslot:#x})")
                # The virtual functions are the words after the typeinfo slot.
                # Each one is itself an R_AARCH64_RELATIVE relocation whose
                # ADDEND is the code address: the slot content is not
                # trustworthy (the linker may leave it zero), the addend is.
                for i in range(args.slots):
                    fn_slot = vslot + 8 + i * 8
                    addends = by_offset.get(fn_slot)
                    if not addends:
                        continue
                    fn_va = addends[0]
                    if fn_va == 0:
                        continue
                    print(f"      slot {i:2d} @ {fn_slot:#x} -> {fn_va:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
