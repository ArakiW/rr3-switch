import re, statistics
def load(p):
    rows=[]; lines=0
    for line in open(p, encoding="utf-8", errors="replace"):
        lines+=1
        if "phase=guest-frame-census" not in line: continue
        d={}
        for k in ("frame","tt","tr","ts","tp","jni","fs"):
            m=re.search(r"\b%s=(\d+)" % k, line)
            if m: d[k]=int(m.group(1))
        if "tt" in d: rows.append(d)
    return rows, lines
base=r"E:\AI\RealRacing3-switch\.build\eden-runs"
for tag,d,lo in (("chunk3","20261110-074920-eden-rr3-chunk3",2500),
                  ("percall1","20261110-161612-eden-rr3-percall1",0),
                  ("yield2","20261110-164914-eden-rr3-yield2",1500)):
    p=base+"\\"+d+"\\rr3-native-rehost.guest-view.log"
    rows,lines=load(p)
    frames=rows[-1].get("frame",0) if rows else 0
    sel=[r for r in rows if r.get("frame",0)>lo]
    if not sel: print("%-9s (no race phase)" % tag); continue
    tts=sorted(r["tt"] for r in sel); j=sorted(r.get("jni",0) for r in sel); f=sorted(r.get("fs",0) for r in sel)
    print("%-9s RACE n=%-5d tt med=%6.1f p25=%6.1f  => %5.1f fps | jni med=%6.1f | fs med=%5.1f | log lines/frame=%4.1f" %
          (tag, len(sel), statistics.median(tts), tts[len(tts)//4], 1000.0/statistics.median(tts),
           statistics.median(j), statistics.median(f), lines/max(1,frames)))