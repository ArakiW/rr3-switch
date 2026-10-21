"""Static analysis of the guest's GUI-resolution list builder.

    python res-order-probe.py strings
    python res-order-probe.py func 0x10f8eb0
    python res-order-probe.py func 0x12c0fb0
    python res-order-probe.py xref 0x<rodata va of "Res order:">

Why: the port's own log shows the guest enumerating gui_assets (480x320,
960x640, 2048x1536, 1280x720) and then printing "Res order:" with entries
whose resolutions (960x640, 480x320, 1024x768, 768x576) do NOT match the
enumerated set. So the printed list is NOT the enumerated list, and the
question is where the enumerated vector goes and what prints what.

Everything here is read-only static analysis of the guest ELF.
"""
import struct
import sys

sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

SO = guestfde.SO
d = guestfde._d

# ---------------------------------------------------------------- strings ---
# Every rodata/data byte range, so a string search is COMPLETE over the
# mapped image and not a guess about which section holds it.
RODATA_VA, RODATA_OFF, RODATA_SIZE = guestfde.SEC[".rodata"][3:6]


def cstr(off, limit=512):
    end = d.find(b"\0", off, off + limit)
    if end < 0:
        return None
    return d[off:end].decode("utf-8", "replace")


def find_strings(needles):
    print("rodata: va=0x%x off=0x%x size=0x%x" % (RODATA_VA, RODATA_OFF, RODATA_SIZE))
    for needle in needles:
        b = needle.encode()
        print("\n=== %r ===" % needle)
        hits = 0
        pos = RODATA_OFF
        last = None
        while True:
            i = d.find(b, pos, RODATA_OFF + RODATA_SIZE)
            if i < 0:
                break
            va = RODATA_VA + (i - RODATA_OFF)
            print("  va=0x%x off=0x%x  %r" % (va, i, cstr(i)))
            hits += 1
            last = i
            pos = i + 1
        if hits == 0:
            # Positive control: the searcher itself must be able to FIND a
            # string that is known to exist. If it finds nothing at all, the
            # absence of a hit is a fact about the searcher, not the binary.
            ctl = b"Res base"
            j = d.find(ctl, RODATA_OFF, RODATA_OFF + RODATA_SIZE)
            print("  NO HIT (control: %r at va=0x%x)" %
                  (ctl, RODATA_VA + (j - RODATA_OFF) if j >= 0 else -1))
        else:
            # Positive control #2: the hit is a real C string.
            print("  hits=%d  (control: cstr reads %r)" %
                  (hits, (cstr(last) or "<unterminated>")[:60]))


# ------------------------------------------------------------------ funcs ---
def dump_func(va, label=""):
    r = guestfde._range(va)
    print("=== %s  0x%x ===" % (label, va))
    if r is None:
        print("  NOT inside a known FDE range")
        guestfde.dump(va, 24, "raw")
        return
    lo, hi = r
    print("  FDE range [0x%x, 0x%x)  size=0x%x  name=%s" %
          (lo, hi, hi - lo, guestfde.name(lo) or "-"))
    o = guestfde._va2off(lo)
    n = (hi - lo) // 4
    for ins in guestfde.MD.disasm(d[o:o + 4 * n], lo):
        mark = ""
        if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
            t = int(ins.op_str[1:].split(",")[0], 16)
            nm = guestfde.sym(t)
            mark = "   ; %s" % (nm if nm else hex(t))
        if ins.mnemonic == "adrp" or "add" == ins.mnemonic:
            pass
        print("  0x%09x: %-10s %-40s%s" % (ins.address, ins.mnemonic,
                                           ins.op_str, mark))


def dump_region(va, count, label=""):
    print("=== %s  0x%x (%d insns) ===" % (label, va, count))
    o = guestfde._va2off(va)
    if o is None:
        print("  unmapped")
        return
    for ins in guestfde.MD.disasm(d[o:o + 4 * count], va):
        mark = ""
        if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
            t = int(ins.op_str[1:].split(",")[0], 16)
            nm = guestfde.sym(t)
            mark = "   ; %s" % (nm if nm else hex(t))
        print("  0x%09x: %-10s %-40s%s" % (ins.address, ins.mnemonic,
                                           ins.op_str, mark))


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "strings"
    if mode == "strings":
        find_strings(sys.argv[2:] or [
            "Res order", "Res base", "original [", "actual [",
            "share) Current res", "%ux%u", "gui_assets"])
    elif mode == "at":
        for a in sys.argv[2:]:
            va = int(a, 16)
            off = guestfde._va2off(va)
            if off is None:
                print("0x%x: unmapped" % va)
                continue
            print("0x%x -> %r" % (va, cstr(off)))
    elif mode == "func":
        for a in sys.argv[2:]:
            dump_func(int(a, 16), "func")
    elif mode == "region":
        va = int(sys.argv[2], 16)
        n = int(sys.argv[3], 0)
        dump_region(va, n, "region")
