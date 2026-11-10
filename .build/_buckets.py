import re, statistics
def load(p):
    rows=[]
    for line in open(p, encoding="utf-8", errors="replace"):
        if "phase=guest-frame-census" not in line: continue
        d={}
        for k in ("frame","tt","tr","ts","tp","jni","jnitex","jnistr","jnifile","jnioth","fs"):
            m=re.search(r"\b%s=(\d+)" % k, line)
            if m: d[k]=int(m.group(1))
        if "tt" in d: rows.append(d)
    return rows
base=r"E:\AI\RealRacing3-switch\.build\eden-runs"
for tag,d,lo in (("percall1","20261110-161612-eden-rr3-percall1",600),
                  ("yield2","20261110-164914-eden-rr3-yield2",600)):
    p=base+"\\"+d+"\\rr3-native-rehost.guest-view.log"
    rows=load(p)
    sel=[r for r in rows if r.get("frame",0)>lo]
    if not sel: print("%-9s no data" % tag); continue
    print("=== %s (frames>%d, n=%d) ===" % (tag, lo, len(sel)))
    for k in ("tt","tr","ts","tp","jni","jnitex","jnistr","jnifile","jnioth","fs"):
        v=[r[k] for r in sel if k in r]
        if v: print("   %-8s median %8.1f ms   mean %8.1f" % (k, statistics.median(v), statistics.fmean(v)))
    print()