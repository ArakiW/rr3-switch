#!/usr/bin/env python3
"""Find the code that WRITES a .bss string slot, by tracking ADRP + STR.

The guest's joystick manager holds its own device-name/description strings in
.bss. The profile matcher reads them, so knowing who fills them says whether
the port can influence the profile choice at all.

Usage: bss_writer.py <page-hex> <offset-hex> [context]
"""

import re
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

from so_xref import Elf

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"


def main() -> int:
    page = int(sys.argv[1], 0)
    offset = int(sys.argv[2], 0)
    context = int(sys.argv[3]) if len(sys.argv) > 3 else 14

    elf = Elf(SO)
    _, _, _, tva, toff, tsize = elf.text()
    code = elf.blob[toff:toff + tsize]
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    instrs = list(md.disasm(code, tva))

    hits = []
    for idx, ins in enumerate(instrs):
        if ins.mnemonic != "adrp":
            continue
        mm = re.match(r"(x\d+), #(0x[0-9a-f]+|\d+)", ins.op_str)
        if not mm:
            continue
        if int(mm.group(2), 0) != page:
            continue
        reg = mm.group(1)
        for j in range(idx + 1, min(idx + 12, len(instrs))):
            nxt = instrs[j]
            if nxt.mnemonic not in ("str", "stp", "strb"):
                continue
            om = re.search(r"\[(\w+), #(0x[0-9a-f]+|\d+)\]", nxt.op_str)
            if not om:
                continue
            if om.group(1) != reg:
                continue
            if int(om.group(2), 0) != offset:
                continue
            hits.append((ins.address, nxt.address))
            break

    print(f"# writers of {page:#x}+{offset:#x}: {len(hits)}")
    for adrp_addr, str_addr in hits[:20]:
        print(f"\n--- adrp {adrp_addr:#x} -> str {str_addr:#x} ---")
        idx = next(k for k, i in enumerate(instrs) if i.address == adrp_addr)
        lo = max(0, idx - context)
        hi = min(len(instrs), idx + context + 6)
        for i in instrs[lo:hi]:
            mark = ">>" if i.address in (adrp_addr, str_addr) else "  "
            print(f"{mark} {i.address:#x}  {i.mnemonic:<8} {i.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
