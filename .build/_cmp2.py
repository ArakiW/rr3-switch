import re, statistics
def load(p):
    rows=[]
    for line in open(p, encoding="utf-8", errors="replace"):
        if "phase=guest-frame-census" not in line: continue
        d={}
        for k in ("frame","tt","tr","jni","fs"):
            m=re.search(r"\b%s=(\d+)" % k, line)
            if m: d[k]=int(m.group(1))
        if "tt" in d: rows.append(d)
    return rows
def show(label, rows, lo, hi):
    sel=[r for r in rows if lo <= r.get("frame",0) < hi]
    if not sel: print("%-28s (none)" % label); return
    tts=sorted(r["tt"] for r in sel)
    j=sorted(r.get("jni",0) for r in sel); f=sorted(r.get("fs",0) for r in sel)
    print("%-28s n=%-5d tt med=%7.1f p25=%7.1f | jni med=%6.1f | fs med=%5.1f" %
          (label, len(sel), statistics.median(tts), tts[len(tts)//4], statistics.median(j), statistics.median(f)))
base=r"E:\AI\RealRacing3-switch\.build\eden-runs"
for tag,d in (("chunk3","20261110-074920-eden-rr3-chunk3"), ("fps5","20261110-160507-eden-rr3-fps5")):
    rows=load(base+"\\"+d+"\\rr3-native-rehost.guest-view.log")
    for lo,hi in ((0,600),(600,1500),(1500,2600)):
        show("%s frames %d-%d" % (tag,lo,hi), rows, lo, hi)
    print()