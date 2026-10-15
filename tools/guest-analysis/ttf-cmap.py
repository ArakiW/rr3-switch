import struct

blob = open(r'E:\AI\RealRacing3-switch\.build\r3_symbols.ttf', 'rb').read()
num = struct.unpack('>H', blob[4:6])[0]
tabs = {}
for i in range(num):
    o = 12 + i * 16
    tag = blob[o:o + 4].decode('latin1')
    chk, off, ln = struct.unpack('>III', blob[o + 4:o + 16])
    tabs[tag] = (off, ln)

co = tabs['cmap'][0]
n = struct.unpack('>H', blob[co + 2:co + 4])[0]
print("cmap subtables: %d" % n)

for i in range(n):
    pid, eid, so = struct.unpack('>HHI', blob[co + 4 + i * 8:co + 12 + i * 8])
    base = co + so
    fmt = struct.unpack('>H', blob[base:base + 2])[0]
    print("\n--- platform=%d encoding=%d format=%d ---" % (pid, eid, fmt))
    if fmt != 4:
        continue
    segX2 = struct.unpack('>H', blob[base + 6:base + 8])[0]
    seg = segX2 // 2
    ends = [struct.unpack('>H', blob[base + 14 + 2 * j:base + 16 + 2 * j])[0] for j in range(seg)]
    sp = base + 16 + segX2
    starts = [struct.unpack('>H', blob[sp + 2 * j:sp + 2 + 2 * j])[0] for j in range(seg)]
    dp = sp + segX2
    deltas = [struct.unpack('>h', blob[dp + 2 * j:dp + 2 + 2 * j])[0] for j in range(seg)]
    rp = dp + segX2
    ranges = [struct.unpack('>H', blob[rp + 2 * j:rp + 2 + 2 * j])[0] for j in range(seg)]
    total = 0
    for j in range(seg):
        if starts[j] == 0xFFFF:
            continue
        c = ends[j] - starts[j] + 1
        total += max(0, c)
        print("   U+%04X..U+%04X  (%d codes)" % (starts[j], ends[j], c))
    print("   total codes mapped = %d" % total)

    def lookup(ch):
        c = ord(ch)
        for j in range(seg):
            if starts[j] <= c <= ends[j]:
                if ranges[j] == 0:
                    return (c + deltas[j]) & 0xFFFF
                addr = rp + 2 * j + ranges[j] + 2 * (c - starts[j])
                g = struct.unpack('>H', blob[addr:addr + 2])[0]
                return (g + deltas[j]) & 0xFFFF if g else 0
        return 0

    print("   ASCII probe: " + " ".join("%r->%d" % (ch, lookup(ch)) for ch in "AB01"))
    if pid == 3 and eid == 1:
        print("   full BMP probe: " + " ".join(
            "%r->%d" % (ch, lookup(ch)) for ch in ".,:!?()%-/+*#@&"))
        syms = [c for c in range(0x20, 0x7F) if lookup(chr(c))]
        print("   printable ASCII covered: %d / 95" % len(syms))
        print("   covered: %r" % "".join(chr(c) for c in syms))
    break
