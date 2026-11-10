import re
from collections import Counter
def phases(p):
    c=Counter(); lines=0
    for line in open(p, encoding="utf-8", errors="replace"):
        lines+=1
        m=re.search(r"phase=([a-z0-9-]+)", line)
        if m: c[m.group(1)]+=1
    return c, lines
for tag,d in (("chunk3","20261110-074920-eden-rr3-chunk3"), ("yield2","20261110-164914-eden-rr3-yield2")):
    p=r"E:\AI\RealRacing3-switch\.build\eden-runs\%s\rr3-native-rehost.guest-view.log" % d
    c,lines=phases(p)
    print("=== %s: %d lines ===" % (tag, lines))
    for k,v in c.most_common(10):
        print("   %-26s %7d" % (k,v))
    print()