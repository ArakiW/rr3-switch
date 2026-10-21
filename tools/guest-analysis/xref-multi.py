"""Find every code reference to a set of guest rodata string addresses, in one pass.

    python xref-multi.py 0x428c19 0x43aa64 0x4430ca 0x486080

Why: the port's log shows the guest printing, in this order,

    Res order:
    share) Current res: 1280, 720
    original [2] (960.000000, 640.000000) - 1.500000
    ...
    actual [1] (768.000000, 576.000000) - 1.333333

but NOT the format the ImageResManager node loop uses,
"(%f, %f) - %f - %s\\n". So "Res order:" and the "original/actual" tables are
printed by DIFFERENT functions, and only the xrefs can say which.

The search is exact, not heuristic: ARM64 materialises an address as
`adrp xN, <page>` + (`add xN, xN, #<off>` | `ldr xN, [xN, #<off>]`), and the
ADRP page delta is a complete enumeration of the candidates: for a given
target page, the set of ADRP instructions that can reach it is finite and is
enumerated here, with every hit re-disassembled to confirm the second
instruction carries the right intra-page offset.

POSITIVE CONTROL: the scanner prints how many ADRP candidates it found and
how many confirmed xrefs, and it is expected to find at least the known
"%ux%u" reference at 0x10f8eb8 (target 0x461d4b). A run that finds nothing
for a KNOWN-good target is a broken scanner, not an absence.
"""
import struct
import sys

sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

d = guestfde._d
TEXT_VA, TEXT_OFF, TEXT_SIZE = guestfde.SEC[".text"][3:6]

TARGETS = {}
for a in sys.argv[1:]:
    TARGETS[int(a, 16)] = None

page_of = {}      # target page -> {intra-page offsets}
for va in TARGETS:
    page_of.setdefault(va & ~0xFFF, set()).add(va & 0xFFF)

# ---- pass 1: every ADRP whose RESULT page is a target page ---------------
cand = {}          # pc -> (result_page, dest reg)
for i in range(0, TEXT_SIZE - 4, 4):
    w = struct.unpack_from("<I", d, TEXT_OFF + i)[0]
    if (w & 0x9F000000) != 0x90000000:      # ADRP
        continue
    immlo = (w >> 29) & 0x3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= 1 << 21
    pc = TEXT_VA + i
    result_page = (pc & ~0xFFF) + (imm << 12)
    if result_page in page_of:
        cand[pc] = (result_page, w & 0x1F)

print("ADRP candidates whose result page is a target page: %d" % len(cand))

# ---- pass 2: confirm with the ADD / LDR that carries the intra-page offset
md = guestfde.MD
found = {}
for pc, (result_page, reg) in cand.items():
    o = guestfde._va2off(pc)
    for k in range(1, 7):
        w2 = struct.unpack_from("<I", d, o + k * 4)[0]
        off = None
        if (w2 & 0x7F800000) == 0x11000000:     # ADD (immediate), 64-bit
            imm12 = (w2 >> 10) & 0xFFF
            if ((w2 >> 22) & 1) == 0:
                off = imm12
        elif (w2 & 0xFFC00000) == 0xF9400000:   # LDR (unsigned imm), 64-bit
            off = ((w2 >> 10) & 0xFFF) * 8
        if off is None:
            continue
        addr = result_page + off
        if addr in TARGETS:
            found.setdefault(addr, []).append(pc + k * 4)
            break

for va in sorted(TARGETS):
    hits = found.get(va, [])
    print("\n0x%x -> %d xref(s): %s" % (va, len(hits), [hex(h) for h in hits]))
    for h in hits:
        r = guestfde._range(h)
        print("   enclosing FDE: %s" % (("[0x%x,0x%x)" % r) if r else "NONE"))
        o = guestfde._va2off(h)
        for ins in md.disasm(d[o - 12:o + 8], h - 12):
            print("     0x%09x: %-10s %s" % (ins.address, ins.mnemonic,
                                             ins.op_str))
