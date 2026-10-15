import collections, io, os, re, sys

P = r"E:\AI\RealRacing3-switch\.build\eden-runs\20261004-045627-lr3\eden_log.txt.tail"
size = os.path.getsize(P)
TAIL = 8 * 1024 * 1024
with open(P, "rb") as f:
    f.seek(max(0, size - TAIL))
    f.seek(0, 1)
    raw = f.read()
txt = raw.decode("utf-8", "replace")
lines = txt.splitlines()
print("file=%d bytes, tail=%d bytes, %d lines" % (size, len(raw), len(lines)))
print("first tail line: %s" % (lines[0][:160] if lines else "(none)"))
print("last  tail line: %s" % (lines[-1][:160] if lines else "(none)"))
print()

def bucket(line):
    m = re.search(r"<(\w+)>\s+(\S+):(\d+):(.*)", line)
    if not m:
        return ("<no-tag>", "", "", line[:80])
    lvl, src, ln, rest = m.groups()
    return (lvl, src, ln, "")

c = collections.Counter()
for l in lines:
    lvl, src, ln, _ = bucket(l)
    if lvl.lower() in ("error", "critical", "fatal", "warning"):
        c[(lvl, src)] += 1
print("=== error/warning buckets in tail ===")
for (lvl, src), n in c.most_common(20):
    print("  %-9s %-60s %d" % (lvl, src, n))
print()

print("=== sample ERROR lines (up to 15) ===")
shown = 0
for l in lines:
    if re.search(r"<(Error|Critical|Fatal)>", l):
        print("  " + l[:300])
        shown += 1
        if shown >= 15:
            break
print()
print("=== most repeated full normalized lines ===")
norm = collections.Counter()
for l in lines:
    k = re.sub(r"\[\s*[0-9.]+\]", "[T]", l)
    k = re.sub(r"0x[0-9a-fA-F]+", "0xX", k)
    k = re.sub(r"\b\d+\b", "N", k)
    norm[k] += 1
for k, n in norm.most_common(15):
    print("  %6d  %s" % (n, k[:200]))
