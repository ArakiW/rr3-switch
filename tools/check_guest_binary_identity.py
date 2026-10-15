"""Decide, at runtime, whether the guest is executing the binary we have on disk.

Run after an Eden round that produced the extended `cc-slot:` line, i.e. one that
carries fp=, sfp=, slr= and at=<8 hex digits of the bytes AT the return address>.

The question this settles: 41 of 51 cc-slot return addresses mapped into
.eh_frame rather than .text, which is impossible for real code. Two explanations
were still open --

  (a) the loaded image is a different build from reverse/native/arm64, or
  (b) the address is real and the first PT_LOAD is R-X, so the guest really is
      executing where our copy of the file has unwind data.

Comparing the bytes the guest is sitting on against the same offset in our file
separates them with no inference in between.
"""

import os
import re
import sys
from elftools.elf.elffile import ELFFile

BIN = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
BIAS = 0x2106759000          # libRealRacing3.so load base, per the startup plan
CCSLOT = re.compile(
    r"cc-slot:\s*slot=(\d+)\s+self=(\S+)\s+caller=([0-9a-f]+)\s+"
    r"fp=([0-9a-f]+)\s+sfp=([0-9a-f]+)\s+slr=([0-9a-f]+)\s+at=([0-9a-f]{16})")


def main(guest_log):
    e = ELFFile(open(BIN, "rb"))
    text = e.get_section_by_name(".text")
    lo, hi = text["sh_addr"], text["sh_addr"] + text["sh_size"]
    data = open(BIN, "rb").read()
    p2v = [(s["p_vaddr"], s["p_offset"], s["p_filesz"])
           for s in e.iter_segments() if s["p_type"] == "PT_LOAD"]

    def at(off, n=8):
        for va, fo, fs in p2v:
            if va <= off < va + fs:
                return data[fo + (off - va): fo + (off - va) + n]
        return None

    rows = []
    for line in open(guest_log, "rb").read().split(b"\n"):
        m = CCSLOT.search(line.decode("utf-8", "replace"))
        if m:
            rows.append(dict(slot=int(m.group(1)),
                             caller=int(m.group(3), 16),
                             fp=int(m.group(4), 16),
                             sfp=int(m.group(5), 16),
                             slr=int(m.group(6), 16),
                             at=int(m.group(7), 16)))

    print("cc-slot rows with the extended probe: %d\n" % len(rows))
    if not rows:
        print("NO EXTENDED ROWS -- the run predates the probe, or the budget was "
              "spent before any slot was reached.")
        return 1

    caller_in_text = caller_match = slr_in_text = slr_match = 0
    for r in rows:
        coff, soff = r["caller"] - BIAS, r["slr"] - BIAS
        c_in = lo <= coff < hi
        s_in = lo <= soff < hi
        caller_in_text += c_in
        slr_in_text += s_in
        disk = at(coff)
        if disk is not None:
            want = int.from_bytes(disk[:4], "little")
            caller_match += (r["at"] >> 32) & 0xFFFFFFFF == want
        if s_in:
            d2 = at(soff)
            if d2 is not None:
                slr_match += (r["at"] >> 32) & 0xFFFFFFFF == int.from_bytes(d2[:4], "little")

    n = len(rows)
    print("caller offset inside .text : %2d/%d" % (caller_in_text, n))
    print("slr    offset inside .text : %2d/%d" % (slr_in_text, n))
    print("first word at caller == disk: %2d/%d" % (caller_match, n))
    print("first word at slr    == disk: %2d/%d" % (slr_match, n))

    print("\nper-row detail (disk bytes shown for comparison):")
    print("  slot  caller-ELF  in.text  at-runtime  on-disk         slr-ELF  in.text")
    for r in rows[:20]:
        coff, soff = r["caller"] - BIAS, r["slr"] - BIAS
        rt = "%016x" % r["at"]
        d = at(coff)
        ds = d.hex() if d else "<unmapped>"
        print("  %4d  0x%08x  %-7s  %s  %-16s 0x%08x  %s" % (
            r["slot"], coff, "yes" if lo <= coff < hi else "NO",
            rt, ds, soff, "yes" if lo <= soff < hi else "NO"))

    print("\nVERDICT")
    if caller_match == n and caller_in_text == n:
        print("  The loaded image IS the on-disk file, and every caller is code.")
    elif caller_match == 0:
        print("  NONE of the bytes the guest is executing match our file.")
        print("  => the loaded libRealRacing3.so is a DIFFERENT BUILD.")
    else:
        print("  MIXED: %d/%d match. Partial overlap; see per-row table." % (caller_match, n))
    e.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))