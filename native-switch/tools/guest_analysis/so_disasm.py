#!/usr/bin/env python3
"""Disassemble a range of the guest binary by virtual address.

Usage: so_disasm.py <start-va-hex> [end-va-hex]
"""

import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

from so_xref import Elf

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"


def main() -> int:
    elf = Elf(SO)
    _, _, _, tva, toff, tsize = elf.text()
    lo = int(sys.argv[1], 0)
    hi = int(sys.argv[2], 0) if len(sys.argv) > 2 else lo + 0x200
    code = elf.blob[toff:toff + tsize]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    n = int((hi - lo) / 4)
    for ins in md.disasm(code[lo - tva: lo - tva + n * 4], lo):
        print(f"{ins.address:#x}  {ins.mnemonic:<8} {ins.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
