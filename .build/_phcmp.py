import re
from collections import Counter
def phases(p):
    c=Counter()
    for line in open(p, encoding="utf-8", errors="replace"):
        m=re.search(r"phase=([a-z0-9-]+)", line)
        if m: c[m.group(1)]+=1
    return c
a=phases(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-074920-eden-rr3-chunk3\rr3-native-rehost.guest-view.log")
b=phases(r"E:\AI\RealRacing3-switch\.build\eden-runs\20261110-153412-eden-rr3-yield1\rr3-native-rehost.guest-view.log")
print("%-28s %10s %10s %8s" % ("phase","chunk3","yield1","delta"))
keys = set(a)|set(b)
rows=[]
for k in keys:
    rows.append((b.get(k,0)-a.get(k,0), k, a.get(k,0), b.get(k,0)))
for d,k,x,y in sorted(rows, reverse=True)[:14]:
    print("%-28s %10d %10d %+8d" % (k,x,y,d))
print("---- vanished (chunk3 had them, yield1 does not) ----")
for d,k,x,y in sorted(rows):
    if x>0 and y==0:
        print("  %-28s chunk3=%d  yield1=0" % (k,x))