"""Summarise an Eden log's unmapped-memory faults.

    python eden-faults.py [path-to-eden_log.txt.tail] [tail_MiB]

The point of the per-page and per-run breakdown is that a LINEAR +1 byte walk
across the whole address space is a different failure from a fixed-address
null dereference, and the two are easy to confuse if you only look at the count.
"""
import collections, os, re, sys

P = (sys.argv[1] if len(sys.argv) > 1 else
     r"E:\AI\RealRacing3-switch\.build\eden-runs\20261004-045627-lr3\eden_log.txt.tail")
TAIL_DEFAULT = 24 * 1024 * 1024
print("source: %s" % P)
size = os.path.getsize(P)
TAIL = (int(sys.argv[2]) if len(sys.argv) > 2 else TAIL_DEFAULT) * 1024 * 1024
with open(P, "rb") as f:
    f.seek(max(0, size - TAIL))
    raw = f.read()
txt = raw.decode("utf-8", "replace")
lines = txt.splitlines()
print("tail %d bytes, %d lines\n" % (len(raw), len(lines)))

RE = re.compile(r"Unmapped (Read|Write)(\d+) @ (0x[0-9A-Fa-f]+)(?: = (0x[0-9A-Fa-f]+))?")

reads = collections.Counter()
writes = collections.Counter()
seq = []          # (addr, op, width) in file order
n = 0
for l in lines:
    m = RE.search(l)
    if not m:
        continue
    op, w, a, v = m.group(1), int(m.group(2)), int(m.group(3), 16), m.group(4)
    n += 1
    (reads if op == "Read" else writes)[a] += 1
    if len(seq) < 400000:
        seq.append((a, op, w, v))

print("parsed %d fault lines" % n)
print()
print("=== TOP 20 Read addresses ===")
for a, c in reads.most_common(20):
    print("  %-18s %6d   low12=0x%03x" % (hex(a), c, a & 0xFFF))
print()
print("=== TOP 20 Write addresses ===")
for a, c in writes.most_common(20):
    print("  %-18s %6d   low12=0x%03x" % (hex(a), c, a & 0xFFF))
print()

# how many distinct addresses vs total
print("distinct read addrs=%d  total reads=%d" % (len(reads), sum(reads.values())))
print("distinct write addrs=%d total writes=%d" % (len(writes), sum(writes.values())))
print()

# find the longest strictly-increasing-by-1 run (a byte-by-byte scan)
best = cur = 0
best_at = cur_at = None
prev = None
for a, op, w, v in seq:
    if op == "Read" and prev is not None and a == prev + 1:
        cur += 1
        if cur == 1:
            cur_at = a
    else:
        if cur > best:
            best, best_at = cur, cur_at
        cur, cur_at = 0, None
    prev = a if op == "Read" else None
if cur > best:
    best, best_at = cur, cur_at
print("=== longest +1 byte-scan run: %d bytes starting at %s ===" % (best, hex(best_at) if best_at else "n/a"))
if best_at:
    bs = best_at
    be = best_at + best - 1
    print("    span %s .. %s" % (hex(bs), hex(be)))
    b = bs.to_bytes(8, "little")
    print("    first 8 bytes LE = %r" % b)
    e = be.to_bytes(8, "little")
    print("    last  8 bytes LE = %r" % e)
    printable = sum(1 for c in b if 32 <= c < 127)
    print("    first-8 printable: %d/8" % printable)
    # dump the whole span as ascii where possible
    span = bytes(((bs + i) & 0xFF) for i in range(min(best, 96)))
    print("    span ascii: %r" % span)
print()

# ranges: cluster addresses by 64K
def cluster(cnt, label):
    cl = collections.Counter()
    for a, c in cnt.items():
        cl[a >> 16] += c
    print("=== %s by 64K cluster (top 12) ===" % label)
    for k, c in cl.most_common(12):
        print("  0x%08x..0x%08x  %7d  (%.1f%%)" % (k << 16, ((k + 1) << 16) - 1, c, 100.0 * c / max(1, sum(cl.values()))))
    print()

cluster(reads, "READ")
cluster(writes, "WRITE")

# low-12-bit distribution of reads (is it pointer-like?)
low12 = collections.Counter(a & 0xFFF for a in reads)
print("=== read low12 spread: distinct=%d ; top 8 ===" % len(low12))
for k, c in low12.most_common(8):
    print("  0x%03x : %d distinct addrs" % (k, c))

# time span
ts = re.findall(r"^\[\s*([0-9.]+)\]", lines[0]), re.findall(r"^\[\s*([0-9.]+)\]", lines[-1])
print()
print("time span of tail: %s .. %s" % (ts[0][0] if ts[0] else "?", ts[1][0] if ts[1] else "?"))
