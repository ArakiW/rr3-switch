"""Find every store to [Xn, #0x4fc] (and neighbours) across guest .text.

The render loop's exit test is [rend+0x4fc] == 4 and the render function never
writes that word, so the writer lives elsewhere. We enumerate candidate store
sites statically. False positives are expected (0x4fc is a common field offset),
so the output is a shortlist for manual CFG checking, not a conclusion.
"""
import sys, struct
sys.path.insert(0, r'E:\AI\RealRacing3-switch\.build')
import guestfde as G

TXT = G.SEC['.text']
START, SIZE = TXT[3], TXT[5]
NEIGH = ['#0x4f8', '#0x4f9', '#0x4fa', '#0x4fb', '#0x4fc', '#0x4fd', '#0x4fe', '#0x4ff',
         '#0x500', '#0x4a0', '#0x4a6', '#0x4a8', '#0x4ac', '#0x4b0', '#0x4b4',
         '#0x4d1', '#0x4d4', '#0x4d8']

o = G._va2off(START)
blob = G._d[o:o + SIZE]
insns = list(G.MD.disasm(blob, START))
print("decoded %d instructions over .text [0x%x,0x%x)" % (len(insns), START, START + SIZE))
print()

# group by imm
buckets = {}
for i in insns:
    for n in NEIGH:
        if n in i.op_str:
            buckets.setdefault(n, []).append(i)
            break

for n in ['#0x4fc', '#0x4a0']:
    rows = buckets.get(n, [])
    stores = [i for i in rows if i.mnemonic.startswith('st')]
    loads = [i for i in rows if i.mnemonic.startswith('ld')]
    print("=== %s : %d stores, %d loads ===" % (n, len(stores), len(loads)))
    for i in stores:
        r = G._range(i.address)
        tag = " [fn 0x%x sz=0x%x]" % (r[0], r[1] - r[0]) if r else " [no fn]"
        nm = G.name(i.address) or ""
        print("   0x%09x: %-8s %-28s%s %s" % (i.address, i.mnemonic, i.op_str, tag, nm))
    print("   loads:")
    for i in loads[:20]:
        r = G._range(i.address)
        tag = " [fn 0x%x]" % r[0] if r else ""
        print("   0x%09x: %-8s %-28s%s" % (i.address, i.mnemonic, i.op_str, tag))
    print()

# Also: which functions contain a store to #0x4fc, ranked
fs = {}
for i in buckets.get('#0x4fc', []):
    if i.mnemonic.startswith('st'):
        r = G._range(i.address)
        if r:
            fs.setdefault(r[0], []).append(i.address)
print("=== functions containing a store to #0x4fc ===")
for f, addrs in sorted(fs.items(), key=lambda kv: len(kv[1]), reverse=True):
    r = G._range(f)
    print("  fn 0x%09x sz=0x%-7x stores=%d at %s" % (f, (r[1] - r[0]) if r else 0,
                                                    len(addrs), ' '.join(hex(a) for a in addrs[:8])))
