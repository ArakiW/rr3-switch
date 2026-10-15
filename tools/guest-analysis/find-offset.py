"""Find code that touches a struct field at a register-built large offset.

    python find-offset.py 0x10518 [--file libRealRacing3.so]

WHY THIS EXISTS
    Scanning .text for `str/ldr [Xn, #0x10518]` returns ZERO hits, and it looks
    conclusive: nobody touches that field. It is false. A field far into a large
    object is addressed with the offset materialised into a register:

        mov  w9, #0x518
        movk w9, #1, lsl #16        ; w9 = 0x10518
        ldr  x8, [x8, x9]

    A 12-bit scaled immediate covers 0..32760 only, so anything past that goes
    through a register. Every false negative below came from assuming the
    immediate form.

THE CONTROL IS NOT OPTIONAL
    The caller passes a known-good site via --control; the scan aborts if it is
    not found. Without that assertion a zero result is indistinguishable from a
    broken predicate, and this file exists because of exactly that confusion
    twice in one session.

ENCODING (verified against the guest, not from a header)
    MOVZ wD, #imm16            0x52800000 | imm16 << 5 | D
    MOVK wD, #imm16, LSL #16   0x72800000 | imm16 << 5 | (hw=1) << 21 | D
    hw encoding: 0 = LSL #0, 1 = LSL #16, 2 = LSL #32, 3 = LSL #48
"""
import argparse
import re
import struct
import sys

DEFAULT_IMAGE = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"

MOVZ32 = 0x52800000
MOVK32 = 0x72800000
MASK = 0xFF800000


def load_sections(data):
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    raw = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        v = struct.unpack_from("<IIQQQQIIQQ", data, o)
        raw.append(dict(name_off=v[0], type=v[1], addr=v[3], off=v[4], size=v[5]))
    strtab = raw[e_shstrndx]

    def name_at(x):
        e = data.index(b"\0", strtab["off"] + x)
        return data[strtab["off"] + x:e].decode("utf-8", "replace")

    return {name_at(s["name_off"]): s for s in raw if name_at(s["name_off"])}


def find_pairs(data, text, offset_want):
    low = offset_want & 0xFFFF
    high = (offset_want >> 16) & 0xFFFF
    hw = (offset_want >> 16) & 3
    if (offset_want & 0xFFFF) != low or ((offset_want >> 16) & 0xFFFF) != high:
        raise SystemExit("offset %#x needs more than movz+movk; not handled" % offset_want)
    base, off, size = text["addr"], text["off"], text["size"]
    pairs = []
    for a in range(base, base + size - 8, 4):
        w = struct.unpack_from("<I", data, off + (a - base))[0]
        if (w & MASK) != MOVZ32:
            continue
        if ((w >> 5) & 0xFFFF) != low:
            continue
        rd = w & 31
        w2 = struct.unpack_from("<I", data, off + (a + 4 - base))[0]
        if (w2 & MASK) != MOVK32:
            continue
        if ((w2 >> 21) & 3) != hw:
            continue
        if (w2 & 31) != rd:
            continue
        if ((w2 >> 5) & 0xFFFF) != high:
            continue
        pairs.append(a)
    return pairs


def classify_sites(data, text, pairs, window=24):
    """For each movz/movk pair, report whether the offset register is used by a
    STORE in the following instructions. That store is the writer: the question
    "who creates this field" is exactly "which of these sites stores to it".

    Two things this must not get wrong, both of which produced a false "nothing
    writes it" earlier in the session:
      * the window. 24 instructions is far too short -- compilers hoist the
        offset register far above its use. The caller passes the enclosing
        function's real size.
      * the addressing form. A register-offset store is `[base, index]`, not
        `[base]`, so comparing only the base finds none of them.
    """
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    base, off, size = text["addr"], text["off"], text["size"]
    rows = []
    for a in pairs:
        w = struct.unpack_from("<I", data, off + (a - base))[0]
        reg = "w%d" % (w & 31)
        chunk = data[off + (a - base):off + (a - base) + window * 4]
        stores = []
        for ins in md.disasm(chunk, a):
            if not ins.mnemonic.startswith("st"):
                continue
            for op in ins.operands:
                if op.type != capstone.x86_const.X86_OP_MEM:
                    continue
                used = ins.reg_name(op.mem.base) or ""
                used_idx = ins.reg_name(op.mem.index) or ""
                if reg.lower() in (used.lower(), used_idx.lower()):
                    stores.append((ins.address, ins.mnemonic, ins.op_str))
                break
        rows.append((a, stores))
    return rows


def adrp_sites(data, text, target):
    """The OTHER addressing mode, for a global reached as ADRP page + 12-bit
    immediate. 0x1f6d5d8 is such a case (adrp x8,#0x1f6d000 ; ldr x8,[x8,#0x5d8])
    and a movz/movk search finds nothing there -- not because nothing accesses
    it, but because this form does not exist. Both modes are needed and they are
    not interchangeable."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    base, off, size = text["addr"], text["off"], text["size"]
    page = target & ~0xFFF
    low = target & 0xFFF
    hits = []
    prev_adrp = None          # (address, reg) of the immediately preceding adrp
    for ins in md.disasm(data[off:off + size], base):
        if ins.mnemonic == "adrp":
            m = re.search(r"#(0x[0-9a-f]+|\d+)", ins.op_str)
            if m and int(m.group(1), 0) == page:
                prev_adrp = (ins.address, ins.op_str.split(",")[0].strip())
            else:
                prev_adrp = None
            continue
        # The memory operand belongs to THIS instruction, and is only meaningful
        # when the instruction right before it was the matching ADRP. Reading
        # the ADRP's own operands is a silent always-zero: an ADRP has none.
        if prev_adrp is not None and prev_adrp[0] == ins.address - 4:
            for op in ins.operands:
                if op.type != capstone.x86_const.X86_OP_MEM:
                    continue
                if (op.mem.disp & ~3) == low and \
                        (ins.reg_name(op.mem.base) or "") == prev_adrp[1]:
                    hits.append((ins.address, ins.mnemonic, ins.op_str))
                break
        prev_adrp = None
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("offset", type=lambda s: int(s, 0))
    ap.add_argument("--file", default=DEFAULT_IMAGE)
    ap.add_argument("--control", type=lambda s: int(s, 0), default=None,
                    help="a known site the scan MUST return, or it aborts")
    ap.add_argument("--stores", action="store_true",
                    help="also report which sites STORE through that register")
    ap.add_argument("--window", type=int, default=24,
                    help="instructions to scan after each pair for a store; 24 is "
                         "known to be too small, use --window 600 to cover a "
                         "whole function")
    ap.add_argument("--global", dest="as_global", action="store_true",
                    help="treat the offset as a GLOBAL (adrp page + immediate) "
                         "instead of a field at a register offset")
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    secs = load_sections(data)
    text = secs[".text"]

    if args.as_global:
        hits = adrp_sites(data, text, args.offset)
        print("global %#x : %d adrp+imm site(s)" % (args.offset, len(hits)))
        for a, m, o in hits:
            print("   %#-12x %-5s %s" % (a, m, o))
        if args.control is not None and args.control not in [h[0] for h in hits]:
            print("CONTROL %#x MISSING -- result means nothing." % args.control)
            return 2
        return 0

    hits = find_pairs(data, text, args.offset)
    print("offset %#x  (%d movz/movk pair(s))" % (args.offset, len(hits)))
    for a in hits:
        print("   %#x" % a)

    if args.control is not None:
        ok = args.control in hits
        print()
        print("CONTROL %#x present = %s" % (args.control, ok))
        if not ok:
            print("PREDICATE INVALID -- the result above means nothing.")
            return 2

    if args.stores:
        rows = classify_sites(data, text, hits, window=args.window)
        print()
        print("=== sites that STORE through the offset register ===")
        n = 0
        for a, stores in rows:
            for addr, mn, ops in stores:
                n += 1
                print("   pair %#-12x  store at %#-12x %-5s %s" % (a, addr, mn, ops))
        print()
        print("%d store site(s) out of %d access site(s)" % (n, len(hits)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
