#!/usr/bin/env python3
"""Extract a guest C++ class's member table (name -> offset) from the binary.

The guest registers every member of some of its classes with a NAME and an
OFFSET, in one function, in a very regular shape:

    adrp xN, #PAGE
    add  xN, xN, #IMM          ; the member's name, as a C string
    ...
    add  x0, x20, #OFF         ; the member's ADDRESS: base + OFF
    bl   <register member>     ; register (address, name, flags)
    ...
    ldr  w4, [x20, #OFF]       ; the member's VALUE, for an int
    blr  x8                    ; the writer's virtual "add member" call

x20 (or whatever --base names) is the object being described. The name
literal and the offset instruction appear in either order, and the literal
is sometimes split across two loads, so the tool associates them by
PROXIMITY around each call rather than by position.

The result is CHECKABLE: every offset the tool prints can be confirmed by
hand in the disassembly, and the tool prints the instruction it read the
offset from.

Usage:
    python member_table.py <so> <start-va> <end-va> [--base x20] [--near 20]
"""

import argparse
import re
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

from so_xref import Elf


def cstring(elf, va, limit=120):
    off = elf.va_to_off(va)
    if off is None:
        return None
    end = elf.blob.find(b"\x00", off, off + limit)
    if end < 0:
        return None
    raw = elf.blob[off:end]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("so")
    ap.add_argument("start")
    ap.add_argument("end")
    ap.add_argument("--base", default="x20")
    ap.add_argument("--near", type=int, default=22)
    args = ap.parse_args()

    elf = Elf(args.so)
    lo = int(args.start, 0)
    hi = int(args.end, 0)
    off = elf.va_to_off(lo)
    if off is None:
        print("start va not in the image")
        return 1
    code = elf.blob[off:off + (hi - lo)]

    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    insns = list(md.disasm(code, lo))
    base = args.base

    pending_va = {}   # register -> literal address from an adrp/add pair
    alias = {}        # register -> register it was copied from
    str_at = []       # (index, name, va)
    off_at = []       # (index, offset, kind, va)

    def is_base(reg):
        return reg == base or alias.get(reg) == base

    for idx, ins in enumerate(insns):
        mn = ins.mnemonic
        ops = ins.op_str

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
                    str_at.append((idx, s, va))
                pending_va[dst] = va
                continue
            if is_base(src):
                alias[dst] = base
                off_at.append((idx, imm, "ptr", ins.address))
                continue
            continue

        if mn.startswith("ldr"):
            m = re.search(r"\[(x\d+), #(0x[0-9a-f]+|\d+)\]!?", ops)
            if m and is_base(m.group(1)):
                off_at.append((idx, int(m.group(2), 0), "int", ins.address))
                continue
            continue

        if mn == "mov":
            m = re.match(r"(x\d+), (x\d+)$", ops)
            if m and is_base(m.group(2)):
                alias[m.group(1)] = base
            continue

    rows = []
    seen = set()
    for idx, ins in enumerate(insns):
        if ins.mnemonic != "bl":
            continue
        s_hit = None
        o_hit = None
        for j in range(idx - 1, max(-1, idx - args.near), -1):
            if s_hit is None:
                for (k, name, va) in str_at:
                    if k == j:
                        s_hit = (j, name, va)
                        break
            if o_hit is None:
                for (k, offset, kind, va) in off_at:
                    if k == j:
                        o_hit = (j, offset, kind, va)
                        break
            if s_hit and o_hit:
                break
        if not (s_hit and o_hit):
            continue
        # The name and the offset belong to the SAME member, so they must be
        # close to each other. Without this, an unrelated call between them
        # (operator new, say) pairs the previous member's name with this
        # member's offset.
        if abs(s_hit[0] - o_hit[0]) > 14:
            continue
        key = (s_hit[1], o_hit[1])
        if key in seen:
            continue
        seen.add(key)
        rows.append((o_hit[3], s_hit[1], o_hit[1], o_hit[2], ins.address))

    print(f"# member table  va {lo:#x}..{hi:#x}  base={base}")
    print(f"# {'offset@va':<22} {'offset':<8} {'kind':<5} name")
    for va, name, offset, kind, callva in rows:
        print(f"  {va:#010x}         +0x{offset:<5x} {kind:<5} {name}")
    print(f"== {len(rows)} members")
    return 0


if __name__ == "__main__":
    sys.exit(main())
