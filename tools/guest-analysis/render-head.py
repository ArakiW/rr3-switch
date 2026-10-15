"""Disassemble the head of the guest render entry and annotate the frame FSM.

    python render-head.py [elfva] [count]

Why this exists: guest_view_probe.cpp's comment claims the render body opens
with `if ((w==w2 && h==h2) || activity[0x4d1]==0) activity[0x4d4]=0; else
activity[0x4d4]++`. If that is right, the host never advances the frame FSM
because it passes the same dimensions on every call. A comment is not evidence,
so this re-derives it from the bytes and marks every store to +0x4d1/+0x4d4/
+0x4d8 so the FSM can be read off the disassembly rather than believed.
"""
import struct, sys
import capstone

sys.path.insert(0, r"E:\AI\RealRacing3-switch\tools\guest-analysis")
import guestfde

VA = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x12AEA7C
N = int(sys.argv[2]) if len(sys.argv) > 2 else 64

lo, hi = guestfde._range(VA)
print("FDE for %s = [%s, %s)  size=%d" % (hex(VA), hex(lo), hex(hi), hi - lo))
if lo is None:
    sys.exit("no FDE covers %s" % hex(VA))
guestfde._selfcheck()

FSM = {"#0x4d1": "hasFocus", "#0x4d4": "frameCounter",
       "#0x4d8": "frameFsm", "#0x4d0": "focusA", "#0x4d2": "focusB",
       "#0x4fc": "gpuId", "#0x500": "gpuModel"}

print()
for i, ins in enumerate(guestfde.insns(VA, N)):
    if i >= N:
        break
    note = ""
    for k, v in FSM.items():
        if k in ins.op_str:
            note = "   <== %s" % v
    print("  %s  %-8s %-34s%s" % (hex(ins.address), ins.mnemonic, ins.op_str, note))
