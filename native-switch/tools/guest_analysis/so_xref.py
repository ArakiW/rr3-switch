#!/usr/bin/env python3
"""Find the guest's native joystick-config matcher and disassemble it.

The guest's own log says which profile it picked ("Cannot find description for
device: <desc> using default: <profile>"), but not HOW it picks, nor which of
a profile's several accel candidates it ends up using. That is native code, so
it is read out of the guest binary: find the string, find the code that
references it, and disassemble that function.

Usage: so_xref.py <string-literal> [max-functions]
"""

import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"


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

    def va_to_off(self, va):
        for (p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz) in self.progs:
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return p_offset + (va - p_vaddr)
        return None

    def off_to_va(self, off):
        for (p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz) in self.progs:
            if p_type == 1 and p_offset <= off < p_offset + p_filesz:
                return p_vaddr + (off - p_offset)
        return None

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        return None

    def text(self):
        s = self.section(".text")
        if s is None:
            # Fall back to the first executable segment.
            for (p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz) in self.progs:
                if p_type == 1 and (p_flags & 1):
                    return (".text", 1, p_flags, p_vaddr, p_offset, p_filesz)
            raise SystemExit("no .text")
        return s


def cstring_at(blob, off):
    end = blob.find(b"\x00", off)
    return blob[off:end]


def main() -> int:
    needle = sys.argv[1].encode("ascii")
    max_funcs = int(sys.argv[2]) if len(sys.argv) > 2 else 2

    elf = Elf(SO)
    blob = elf.blob

    hits = []
    start = 0
    while True:
        i = blob.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if not hits:
        print("literal not found")
        return 1
    for off in hits:
        va = elf.off_to_va(off)
        print(f"# literal at file {off:#x} va {va:#x} : "
              f"{cstring_at(blob, off)[:80]!r}")

    target_va = elf.off_to_va(hits[0])
    page = target_va & ~0xFFF
    print(f"# searching for references to page {page:#x}")

    _, _, _, tva, toff, tsize = elf.text()
    code = blob[toff:toff + tsize]

    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = False

    # Collect the ADRP targets that point at the string's page, then look for a
    # following ADD/LDR that forms the full address.
    refs = []
    instrs = list(md.disasm(code, tva))
    for idx, ins in enumerate(instrs):
        if ins.mnemonic != "adrp":
            continue
        try:
            imm = int(ins.op_str.split("#")[1].rstrip("]"), 16)
        except (ValueError, IndexError):
            continue
        if imm != page:
            continue
        # Look ahead for the ADD that completes the address.
        for j in range(idx + 1, min(idx + 8, len(instrs))):
            nxt = instrs[j]
            if nxt.mnemonic == "add" and "x" in nxt.op_str:
                mm = re.match(r"(x\d+), (x\d+), #(0x[0-9a-f]+|\d+)", nxt.op_str)
                if mm and mm.group(1) == ins.op_str.split(",")[0].strip():
                    val = int(mm.group(3), 0)
                    if (page + val) == target_va:
                        refs.append(ins.address)
                        break
            if nxt.mnemonic in ("b", "bl", "ret", "br"):
                break

    print(f"# adrp/add references: {[hex(r) for r in refs]}")

    if not refs:
        return 0

    # Print a window around each reference, and try to find the function start
    # by scanning backwards for a common prologue.
    for ref in refs[:max_funcs]:
        print(f"\n===== reference at {ref:#x} =====")
        idx = next(k for k, ins in enumerate(instrs) if ins.address == ref)
        lo = max(0, idx - 40)
        hi = min(len(instrs), idx + 60)
        for ins in instrs[lo:hi]:
            mark = ">>" if ins.address == ref else "  "
            print(f"{mark} {ins.address:#x}  {ins.mnemonic:<8} {ins.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
