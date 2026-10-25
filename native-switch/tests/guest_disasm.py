#!/usr/bin/env python3
"""Disassemble a window of the guest's own code.

Used by the glyph-layout tests to re-derive the guest's constants (the
initWithParagraph call site, the wrap width it passes, the promotion it
does) straight out of the guest binary, the same way the rest of this
project re-derives everything else it claims about the guest.

Usage:
    python guest_disasm.py <libRealRacing3.so> <vaddr_hex> [count]
"""

import struct
import sys
from pathlib import Path

try:
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
except ImportError:  # pragma: no cover - the caller checks for this
    Cs = None


def load_segments(blob: bytes):
    """Return [(vaddr, filesz, memsz, offset, flags)] for the PT_LOAD parts."""
    if blob[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file")
    e_phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", blob, 0x36)[0]
    e_phnum = struct.unpack_from("<H", blob, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", blob, off)
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<QQQQQ", blob, off + 8)
        segs.append((p_vaddr, p_filesz, p_memsz, p_offset, p_flags))
    return segs


def read_at(blob: bytes, segs, vaddr: int, size: int) -> bytes:
    for v, filesz, memsz, off, _flags in segs:
        if v <= vaddr < v + filesz:
            start = off + (vaddr - v)
            return blob[start:start + size]
    raise SystemExit(f"vaddr {vaddr:#x} is not in a PT_LOAD segment")


def main() -> int:
    if Cs is None:
        print("SKIP: capstone is not installed")
        return 0
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = Path(sys.argv[1])
    vaddr = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    blob = path.read_bytes()
    segs = load_segments(blob)
    code = read_at(blob, segs, vaddr, count * 4)
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    for insn in md.disasm(code, vaddr):
        print(f"{insn.address:#010x}  {insn.mnemonic:<8} {insn.op_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
