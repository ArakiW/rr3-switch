import re, sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
def warnings_from(log_path):
    p = re.compile(r"^(.*?):(\d+):(\d+): warning: .*?-Wformat-truncation=.*?writing up to (\d+) bytes into a region of size(?: between (\d+) and (\d+)| (\d+))")
    out=[]
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m=p.match(line.strip())
        if m:
            lo, hi, single = m.group(5), m.group(6), m.group(7)
            limit = int(lo) if lo else (int(single) if single else int(hi))
            out.append((m.group(1), int(m.group(2)), limit))
    return out
by={}
for path,ln,lim in warnings_from(r"E:\AI\RealRacing3-switch\.build\build-slim2.log"):
    by.setdefault(path,[]).append((ln,lim))
total=0
for p,items in by.items():
    total += ft.fix_file(p, items)
    print(p, len(items))
print("total", total)