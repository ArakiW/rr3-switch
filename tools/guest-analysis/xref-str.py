"""Find the code that references a .rodata/.data string address.

    python xref-str.py 0x47e29c [radius=0x300]

Why: the guest raised its own fatal dialog, "加载游戏数据时出错…", and that
string sits at 0x47e29c. Locating the string told us the failure is SaveManager
(SaveManager.cpp:699 is its neighbour). This answers the next question -- WHICH
CONDITION raises it -- by finding the ADRP/ADD pair that materialises the
address and printing the surrounding basic block.

The search is literal and bounded: ARM64 materialises a 4 KiB-aligned page with
`adrp xN, page` and the intra-page offset with `add xN, xN, #imm`, so a 4-byte
encode of the ADRP page delta plus a match on the offset in the next few
instructions is a complete search, not a heuristic one. Every hit is re-checked
by disassembling, so a coincidental page match cannot be reported as an xref.
"""
import re, struct, sys
import capstone

sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

TARGET = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x47E29C
RADIUS = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x300

page = TARGET & ~0xFFF
off = TARGET & 0xFFF
d = guestfde._d
text_va, text_off, text_size = None, None, None
for name, sh in guestfde.SEC.items():
    if name == ".text":
        text_va, text_off, text_size = sh[3], sh[4], sh[5]
assert text_va is not None, "no .text"
print("target = %s   page = %s   off = #0x%x" % (hex(TARGET), hex(page), off))
print(".text  = %s size = %d" % (hex(text_va), text_size))

# ADRP: immhi:immlo = imm21, encoded as bits[30:29]=immlo, bits[23:5]=immhi
# page delta in 4 KiB pages, PC-relative to the ADRP itself.
hits = []
for i in range(0, text_size - 4, 4):
    w = struct.unpack_from("<I", d, text_off + i)[0]
    if (w & 0x9F000000) != 0x90000000:      # op=1, fixed 10000
        continue
    immlo = (w >> 29) & 0x3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):                      # sign-extend 21 bits
        imm -= 1 << 21
    pc = text_va + i
    # ADRP is Align(PC, 4096) + imm*4096 -- the BASE is page-aligned, the raw
    # PC is not. Using `pc + (imm << 12)` produces non-page-aligned results
    # that a later `& ~0xFFF` quietly rounds into plausible-looking page
    # numbers, so the mistake hides instead of failing loudly.
    if ((pc & ~0xFFF) + (imm << 12)) != page:
        continue
    hits.append((pc, w & 0x1F))               # (addr, dest register)

print("ADRP candidates pointing at page: %d" % len(hits))

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True

real = []
for pc, reg in hits:
    o = guestfde._va2off(pc)
    for k in range(1, 5):                      # look at the next 4 insns
        code = d[o + k * 4: o + k * 4 + 4]
        if len(code) < 4:
            break
        w2 = struct.unpack_from("<I", code)[0]
        if (w2 & 0xFF000000) == 0x91000000:    # ADD (immediate)
            imm12 = (w2 >> 10) & 0xFFF
            sh = (w2 >> 22) & 1
            if imm12 == off and not sh:
                real.append(pc + k * 4)
                break

print("confirmed xrefs: %s" % [hex(a) for a in real])
print()

for x in real:
    lo, hi = guestfde._range(x)
    if lo is None:
        lo = x - 0x80
        hi = x + 0x80
    print("=== xref at %s   enclosing FDE [%s,%s) %s ===" %
          (hex(x), hex(lo), hex(hi), guestfde.name(lo) or ""))
    base = max(lo, x - 0xC0)
    o = guestfde._va2off(base)
    span = min(hi, x + RADIUS) - base
    for ins in md.disasm(d[o:o + span], base):
        mark = "  <== xref" if ins.address == x else ""
        if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
            try:
                t = int(ins.op_str[1:], 0)
                nm = guestfde.sym(t)
                mark += ("   ; %s" % nm) if nm else ("   ; %s" % hex(t))
            except Exception:
                pass
        print("  %s  %-8s %-34s%s" % (hex(ins.address), ins.mnemonic,
                                       ins.op_str, mark))
    print()
