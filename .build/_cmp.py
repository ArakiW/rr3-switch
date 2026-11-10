import re, statistics, sys
def load(p):
    rows=[]
    for line in open(p, encoding="utf-8", errors="replace"):
        if "phase=guest-frame-census" not in line: continue
        d = {}
        for k in ("frame","tt","tr","ts","tp"):
            m = re.search(r"\b%s=(\d+)" % k, line)
            if m: d[k]=int(m.group(1))
        if "tt" in d: rows.append(d)
    return rows
a = load(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-074920-eden-rr3-chunk3\rr3-native-rehost.guest-view.log")
b = load(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-081622-eden-rr3-fps1\rr3-native-rehost.guest-view.log")
def report(name, rows, lo, hi):
    sel=[r for r in rows if lo <= r.get("frame",0) < hi]
    if not sel: 
        print("%-28s (none)" % name); return
    out=[]
    for k in ("tt","tr","ts","tp"):
        v=[r[k] for r in sel if k in r]
        if v: out.append("%s=%6.1fms(p90 %6.1f)" % (k, statistics.fmean(v), sorted(v)[int(len(v)*0.9)]))
    print("%-28s n=%-5d %s" % (name, len(sel), "  ".join(out)))
print("=== THE SAME FRAME RANGE, both rounds ===")
for lo,hi in ((0,600),(600,1200),(1200,1855),(1855,2500),(2500,99999)):
    report("chunk3 frames %d-%d" % (lo,hi), a, lo, hi)
    report("fps1   frames %d-%d" % (lo,hi), b, lo, hi)
    print()