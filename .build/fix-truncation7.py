import re, sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
p = re.compile(r"^(.*?):(\d+):(\d+): warning: .*?-Wformat-truncation=.*?writing up to (\d+) bytes into a region of size(?: between (\d+) and (\d+)| (\d+))")
wanted=[]
for line in open(r"E:\AI\RealRacing3-switch\.build\build-slim8.log", encoding="utf-8", errors="replace"):
    m=p.match(line.strip())
    if m:
        lo,hi,single = m.group(5), m.group(6), m.group(7)
        limit = int(lo) if lo else (int(single) if single else int(hi))
        wanted.append((m.group(1), int(m.group(2)), limit))
print("warnings parsed:", len(wanted))
by={}
for path,ln,lim in wanted: by.setdefault(path,[]).append((ln,lim))
for path, items in by.items():
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    fixed=0
    for lineno, limit in sorted(items, reverse=True):
        off = ft.self_offset(text, lineno)
        start = text.rfind("snprintf", 0, off)
        if start < 0: continue
        paren = text.find("(", start)
        args,_ = ft.split_args(text, paren)
        if len(args) < 3: continue
        fmt = args[2]; n = fmt.count("%s")
        if n == 0: continue
        prec = max(1, (limit-1)//n - 1)
        new_fmt = fmt.replace("%s", "%."+str(prec)+"s")
        if new_fmt == fmt: continue
        pos = text.index(fmt, paren)
        text = text[:pos]+new_fmt+text[pos+len(fmt):]
        fixed += 1
    if fixed:
        open(path,"w",encoding="utf-8",errors="surrogateescape",newline="").write(text)
    print(path, fixed, "fixed")