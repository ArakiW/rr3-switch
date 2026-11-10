import re, statistics, os
def stats(path, label):
    lines=0; rows=[]; phases={}
    with open(path, encoding="utf-8", errors="replace") as h:
        for line in h:
            lines+=1
            m=re.search(r"phase=guest-frame-census\b", line)
            if m:
                d={}
                for k in ("frame","tt","tr","jni","fs"):
                    mm=re.search(r"\b%s=(\d+)" % k, line)
                    if mm: d[k]=int(mm.group(1))
                if "tt" in d: rows.append(d)
            p=re.search(r"phase=([a-z0-9-]+)", line)
            if p: phases[p.group(1)]=phases.get(p.group(1),0)+1
    frames = rows[-1].get("frame",0) if rows else 0
    print("%s: total log lines=%d  frames=%d  => %.1f lines/frame" % (label, lines, frames, lines/max(1,frames)))
    race=[r for r in rows if r.get("frame",0)>2500]
    if race:
        tts=sorted(r["tt"] for r in race)
        jnis=sorted(r.get("jni",0) for r in race)
        fss=sorted(r.get("fs",0) for r in race)
        print("   RACE n=%d  tt median=%.1f ms  p25=%.1f  jni median=%.1f ms  fs median=%.1f ms" %
              (len(race), statistics.median(tts), tts[len(tts)//4], statistics.median(jnis), statistics.median(fss)))
    top=sorted(phases.items(), key=lambda kv:-kv[1])[:6]
    print("   top phases: " + ", ".join("%s=%d" % (k,v) for k,v in top))
    print()
stats(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-074920-eden-rr3-chunk3\rr3-native-rehost.guest-view.log", "chunk3 (percall ON,  no cache)")
stats(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-084334-eden-rr3-fps2\rr3-native-rehost.guest-view.log",   "fps2   (percall OFF, cache ON) ")