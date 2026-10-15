"""Who writes the frame FSM words in a guest function?

    python fsm-writers.py [elfva] [count]

render-head.py showed the render entry reaching `bl 0x12ae290` only when
(0x4a8!=0x4b0 || 0x4ac!=0x4b4) && 0x4d1!=0, and clearing 0x4d4 otherwise.
That pattern is a debounce for a RESIZE, not a per-frame draw, so the per-frame
work must be driven by a different word. This lists every store in the function
to 0x4a4..0x4e0 so the word that actually advances can be identified from the
disassembly rather than guessed.
"""
import re, sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

VA = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x12AEA7C
N = int(sys.argv[2]) if len(sys.argv) > 2 else 4000

guestfde._selfcheck()
lo, hi = guestfde._range(VA)
print("FDE [%s,%s) size=%d  scanning %d insns" % (hex(lo), hex(hi), hi - lo, N))

STORE = re.compile(r"\[x\d+, #0x(4[0-9a-f]{2})\]")
n = 0
for ins in guestfde.insns(VA, N):
    n += 1
    m = STORE.search(ins.op_str)
    if not m:
        continue
    off = int(m.group(1), 16)
    if not (0x4a4 <= off <= 0x4e0):
        continue
    # str/stur = write; ldr/ldrb = read. Classify by mnemonic, not by guessing.
    kind = "WRITE" if ins.mnemonic.startswith(("str", "stur", "stp")) else "read "
    print("  %s  %-7s %-30s  +0x%s  %s" % (hex(ins.address), ins.mnemonic,
                                           ins.op_str, m.group(1), kind))
print("\ntotal insns scanned: %d" % n)
