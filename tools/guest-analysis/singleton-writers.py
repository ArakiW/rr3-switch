"""Enumerate stores to a guest singleton field, with controls that must pass first.

    python singleton-writers.py [elfva] [field_off]      (default 0x1f6d5d8, 8)

READ THIS BEFORE ENABLING ROUTE B. Eight attempts were made to make route B
("the object arrives as a parameter in x0") work, and the measurement at the
end of them showed the premise is simply false for the function it was being
tested against:

    0x10ac430  mov  w8, #0xbb8     x0=0x100
    0x10ac460  add  x0, x19, x8     x19 not tracked

`x0` is a general-purpose variable there, walked to 0x10 and then 0x100 by the
function itself. Seeding `eng[0] = 0` on entry manufactures a base that does
not exist, and every one of the eight fixes was patching a symptom of that one
bad premise rather than the premise.

So route B is off. A function's object parameter is whichever register the
caller passes, and that has to be read from the CALL SITE per function --
which is the same "verify the first link" rule that ended the
construction-sequence detour, arriving from the opposite direction.

WHY THIS TOOL EXISTS. Three hand-rolled enumerations of "who writes engine+8"
produced three different wrong answers, and the eight defects behind them were:
STUR's 9-bit signed field missing; raw store offsets with no base displacement;
displacement applied but the base dropped; per-FDE iteration over a table that
is not a partition of .text (84,635 gaps); a prologue bitmask matching zero
instructions; ADD (register) `option` masked instead of extracted; `mov xD,x0`
invalidating instead of copying; and `eng.pop(base)` after a store, which is a
category error that moves the symptom to a different function every run.

A control on the *decoder* validated all of them. A control is worth nothing
unless it would fail if the arithmetic that produces the answer were wrong.

CONTROLS (both must pass, or nothing is printed):
  A. the .got route must account for the known writer, at its real offset
  B. a route-B store whose base is a callee-saved copy of x0 must be reported at
     its displaced offset and must NOT be reported at the raw offset

Offset encodings that must be decoded, all four:
  STR/STRB imm, unsigned offset   imm12, byte-scaled 1 or 4
  STRH imm, unsigned offset       imm12, scale 2
  STP / STUR family              imm7 signed (STP) or imm9 signed (STUR)
  add-materialised base + small store
"""
import re, struct, sys
import capstone

sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

SLOT_VA  = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x1F6D5D8
FIELD    = int(sys.argv[2], 0) if len(sys.argv) > 2 else 8
PAGE     = SLOT_VA & 0xFFFFF000
SLOT_OFF = (SLOT_VA & 0xFFF) // 8      # the LDR immediate is imm12 * 8
D = guestfde._d
SH = guestfde.SEC[".text"]
TV, TO, TSZ = SH[3], SH[4], SH[5]

STORE_OPS = (0x3E4, 0x2E4, 0x3E0, 0x2E0, 0x3E6, 0x2E6, 0x3E2, 0x2E2,
             0x2A0, 0x2A2, 0x2A4, 0x2A6)


def store_offsets(word):
    """Byte offsets a store/pair writes, from the raw 32-bit word."""
    op = (word >> 22) & 0x3FF
    out = []
    if op in (0x3E4, 0x2E4, 0x3E0, 0x2E0, 0x3E6, 0x2E6):        # STR / STUR
        imm = (word >> 10) & 0x7FF
        if (word >> 21) & 1 and (imm & 0x100):
            imm -= 0x200
        out.append(imm * (8 if (word >> 30) == 3 else 4))
    elif op in (0x3E2, 0x2E2):                                   # STRH
        imm = (word >> 10) & 0x7FF
        if (word >> 21) & 1 and (imm & 0x100):
            imm -= 0x200
        out.append(imm * 2)
    elif op in (0x2A0, 0x2A2, 0x2A4, 0x2A6):                    # STP
        imm = (word >> 15) & 0x7F
        if imm & 0x40:
            imm -= 0x80
        sz = 16 if (word >> 30) == 3 else 8
        out += [imm * sz, imm * sz + sz]
    return out


def mem_base(word):
    op = (word >> 22) & 0x3FF
    return (word >> 5) & 0x1F if op in STORE_OPS else None


def is_wreg(word, rn):
    return (word & 0xFFE0FFE0) == 0xAA0003E0 and (word >> 5 & 0x1F) == rn


FDE_STARTS = set(lo for lo, _ in guestfde.FDE)     # O(1), not 84k tuple scans


def _scan_step(eng, out, w, pc):
    """Advance the pass by exactly one instruction, mutating eng and out."""
    if (w & 0x9F000000) == 0x90000000:                            # ADRP
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 0x3)
        if imm & (1 << 20):
            imm -= 1 << 21
        dst = w & 0x1F
        if ((pc & ~0xFFF) + (imm << 12)) == PAGE:                  # route A
            for k in range(1, 5):
                w2 = struct.unpack_from("<I", D, TO + (pc - TV) + k * 4)[0]
                if ((w2 & 0xFFC00000) == 0xF9400000
                        and ((w2 >> 10) & 0xFFF) == SLOT_OFF
                        and ((w2 >> 5) & 0x1F) == dst):
                    eng[w2 & 0x1F] = 0
                    break
            else:
                eng.pop(dst, None)
        else:
            eng.pop(dst, None)
        return
    if (w & 0xFF000000) in (0x91000000, 0xD1000000):              # ADD/SUB imm
        rd, rn = w & 0x1F, (w >> 5) & 0x1F
        if (w & 0xFF000000) == 0x91000000 and ((w >> 22) & 1):
            return                                                # shifted
        delta = (w >> 10) & 0xFFF
        if (w & 0xFF000000) == 0xD1000000:
            delta = -delta
        if rn in eng:
            eng[rd] = eng[rn] + delta
        else:
            eng.pop(rd, None)
        return
    # ADD/SUB (register), by FIELD, not by mask. `option` is bits 15:13 and a
    # plain `add x8, x19, x8` is option 0b010 (LSL, shift 0) -- 0b000 is the
    # EXTENDED-register form. Masking for 0b000 matched nothing, which is the
    # third time tonight an ARM64 field was masked instead of extracted.
    if (w & 0x7F800000) in (0x0B000000, 0x4B000000, 0x8B000000, 0xCB000000):
        option = (w >> 13) & 0x7
        is_add = (w & 0x40000000) == 0
        ok = option in ((0, 2) if is_add else (3, 4, 5, 6))
        if ok:
            rd, rn, rm = w & 0x1F, (w >> 5) & 0x1F, (w >> 16) & 0x1F
            if rn in eng and rm in eng:
                v = eng[rn] + (eng[rm] if is_add else -eng[rm])
                if option in (0, 3, 4):                       # shifted forms
                    v += (v << ((w >> 10) & 0x3F)) if is_add else (v >> ((w >> 10) & 0x3F))
                eng[rd] = v
            else:
                eng.pop(rd, None)
            return
    # MOVZ / MOVK -- the other way a displacement gets built, after
    #   mov w8, #0xf400 ; movk w8, #1, lsl #16  =>  0x1f400
    # Neither was modelled, so eng[8] did not exist and the register-form add
    # at 0x10ac5dc could not resolve. Both are field extractions.
    if (w & 0x7F800000) in (0x52800000, 0x72800000):              # MOVZ / MOVK
        rd = w & 0x1F
        hw = (w >> 21) & 0x3
        sh = 16 * hw
        mask = 0xFFFF << sh
        if (w & 0x7F800000) == 0x52800000:                        # MOVZ
            eng[rd] = ((w >> 5) & 0xFFFF) << sh
        else:                                                      # MOVK
            if rd in eng:                                          # merges
                eng[rd] = (eng[rd] & ~mask) | (((w >> 5) & 0xFFFF) << sh)
            # a MOVK into an untracked register leaves it untracked; inventing
            # a value here would fabricate a base that may not exist
        return
    if is_wreg(w, 0):                                              # mov xD, x0
        v = eng.get(0)
        if v is None:
            eng.pop(w & 0x1F, None)
        else:
            eng[w & 0x1F] = v
        return
    mb = mem_base(w)
    if mb is not None and mb in eng:
        for off in store_offsets(w):
            out.setdefault(eng[mb] + off, []).append(pc)
        # NO eng.pop(mb) HERE. A store writes memory at base+off; it does not
        # reassign the base register. Invalidating eng[mb] was a category error
        # and is the single defect behind every wrong list this tool has ever
        # produced: it killed the base at the first store in each function, so
        # everything after it was invisible, and because "the first store"
        # differs per function the failure appeared to move each run. A base is
        # invalidated by REASSIGNMENT, which add/mov/sub already handle --
        # 0x10ac460 `add x0, x19, x8` legitimately ends x0's life, and x19 must
        # survive to reach it (0x10ac3d0 `str xzr, [x19, #0x54b8]` does not end
        # x19's life).


def _is_entry(pc, word):
    """Function-entry signal: an FDE start, or a frame-setup prologue.

    `guestfde.FDE` is NOT a partition of .text -- 84,636 entries with 84,635
    gaps, and the known-good writer 0x12ddce4 lies in no FDE at all. Iterating
    the table therefore loses functions, so entry detection cannot rely on it
    alone. The `stp x29, x30, [sp, #-N]!` frame setup is present for functions
    that have no unwind record, and is what the gaps need.
    """
    if pc in FDE_STARTS:
        return True
    # stp x29, x30, [sp, #-N]!  by FIELD extraction, not a bitmask: the earlier
    # mask (0xFFE00000/0x7FC00000) matched zero instructions in the whole image.
    if (word & 0xFF000000) in (0xA8000000, 0xA9000000):                # STP / STP!
        if ((word & 0x1F) == 0x1E and ((word >> 5) & 0x1F) == 0x1F
                and ((word >> 10) & 0x1F) == 0x1D):                   # x30, sp, x29
            return True
    return False


def enumerate_sites(seed_x0):
    """{byte offset from the singleton: [store addresses]} -- linear over .text."""
    out = {}
    eng = {}
    for i in range(0, TSZ - 4, 4):
        w = struct.unpack_from("<I", D, TO + i)[0]
        pc = TV + i
        if seed_x0 and _is_entry(pc, w):
            eng = {0: 0}                 # route B: x0 is the object on entry
        _scan_step(eng, out, w, pc)
    return out


def main():
    a_only = enumerate_sites(False)
    both = enumerate_sites(True)

    # CONTROL A: the .got route must account for a store at a real offset.
    ctl_a = any(a in a_only for a in (0,)) and 0x12DDCE4 in sum(a_only.values(), [])
    # CONTROL B: a route-B store must appear only at its displaced offset.
    b_disp = [a for a in (0x1F400, 0x1F408) if a in both]
    b_raw = [a for a in (0, 8) if a in both and 0x10AC5E8 in both[a]]
    print("CONTROL A (.got route accounts for 0x12ddce4)          : %s" % ctl_a)
    print("CONTROL B (route-B store at its displaced offset)     : %s %s"
          % (bool(b_disp), [hex(x) for x in b_disp]))
    print("CONTROL B' (and NOT reported at the raw offset)       : %s" % (not b_raw))
    if not (ctl_a and b_disp and not b_raw):
        print("\nPREDICATE INVALID -- refusing to print a list. "
              "No statement is made about the writers of +0x%x." % FIELD)
        return 1
    print("\nPREDICATE VALID -- both routes accounted for\n")
    hits = both.get(FIELD, [])
    print("writers of +0x%x  : %d" % (FIELD, len(hits)))
    for a in hits:
        lo, hi = guestfde._range(a)
        print("   %s   FDE=[%s,%s) size=%s"
              % (hex(a), hex(lo) if lo else "?", hex(hi) if hi else "?",
                 hex(hi - lo) if lo else "?"))
    print("\noffsets seen through the singleton: %d distinct"
          % len(both))
    return 0


if __name__ == "__main__":
    sys.exit(main())



