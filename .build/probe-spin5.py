import io, re, collections, sys

RUN = r'E:\AI\RealRacing3-switch\.build\eden-runs\20261117-005310-eden-rr3-fmodreal-obs'
tail = RUN + r'\eden_log.txt.tail'

# bases from this round's guarded-action.pending
bases = []
for line in io.open(RUN + r'\rr3-native-rehost.guarded-action.pending', encoding='utf-8', errors='replace'):
    m = re.search(r'image(\d+)=([\w.\-]+\.so)@([0-9a-fA-F]+)\+([0-9a-fA-F]+)', line)
    if m:
        bases.append((int(m.group(3), 16), int(m.group(4), 16), m.group(2)))
bases.sort()
def attr(v):
    for base, size, name in bases:
        if base <= v < base + size:
            return '%s+0x%x' % (name, v - base)
    return 'unknown(0x%x)' % v

pat = re.compile(r'\[\s*([0-9.]+)\].*Unmapped (Read|Write)(\d+)\s*@\s*0x([0-9a-fA-F]+)(?:\s*=\s*0x([0-9a-fA-F]+))?')
ops = []
with io.open(tail, encoding='utf-8', errors='replace') as f:
    for line in f:
        m = pat.search(line)
        if not m:
            continue
        ops.append((float(m.group(1)), m.group(2) + m.group(3), int(m.group(4), 16),
                    int(m.group(5), 16) if m.group(5) else None))
print('total unmapped ops', len(ops))
print('t range %.6f .. %.6f' % (ops[0][0], ops[-1][0]))
print()
print('=== BASES ===')
for base, size, name in bases:
    print('  %-22s @ 0x%012x + 0x%x' % (name, base, size))
print()
print('=== first 70 ops in order ===')
for i, (t, op, addr, val) in enumerate(ops[:70]):
    extra = ''
    if val is not None:
        extra = ' = 0x%x  [%s]' % (val, attr(val))
    print('%5d t=%10.6f  %-8s @ 0x%-8x%s' % (i, t, op, addr, extra))
print()
c = collections.Counter((op, addr) for (_t, op, addr, _v) in ops)
print('=== top 15 (op, addr) ===')
for k, v in c.most_common(15):
    print('%9d  %-9s @ 0x%x' % (v, k[0], k[1]))
print()
writes = [(t, addr, val) for (t, op, addr, val) in ops if op.startswith('Write') and val is not None]
wc = collections.Counter(attr(v) for (_t, _a, v) in writes)
print('=== written VALUE attribution (top 20) ===')
for k, v in wc.most_common(20):
    print('%9d  %s' % (v, k))
print()
# the alternating loop signature
pairs = collections.Counter()
for i in range(1, len(ops)):
    pairs[(ops[i-1][1], ops[i][1], ops[i-1][2], ops[i][2])] += 1
print('=== top 10 consecutive (op,op,addr,addr) ===')
for k, v in pairs.most_common(10):
    print('%9d  %s@0x%x -> %s@0x%x' % (v, k[0], k[2], k[1], k[3]))
