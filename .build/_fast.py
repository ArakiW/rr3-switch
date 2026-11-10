import re
def load(p):
    rows=[]
    for line in open(p, encoding="utf-8", errors="replace"):
        if "phase=guest-frame-census" not in line: continue
        d={}
        for k in ("frame","tt","tr"):
            m=re.search(r"\b%s=(\d+)" % k, line)
            if m: d[k]=int(m.group(1))
        if "tt" in d: rows.append(d)
    return rows
for name,p in (("chunk3", r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-074920-eden-rr3-chunk3\rr3-native-rehost.guest-view.log"),
                ("fps1  ", r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-081622-eden-rr3-fps1\rr3-native-rehost.guest-view.log")):
    r=load(p)
    tts=sorted(x["tt"] for x in r)
    n=len(tts)
    fast=[t for t in tts if t < 16666]   # 16.666 ms in ticks? tt is in ms, so < 16.67 ms
    fast=[t for t in tts if t < 17]
    print("%s frames=%-5d  min tt=%6.1f ms  frames faster than 17 ms: %d (%.2f%%)  median=%7.1f ms" %
          (name, n, tts[0], len(fast), 100.0*len(fast)/n, tts[n//2]))