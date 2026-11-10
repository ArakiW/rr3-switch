import re, sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
path = r"E:\AI\RealRacing3-switch\native-switch\source\guest_filesystem.cpp"
# (line in the CURRENT file, the compiler's region lower bound)
sites = [(1630, 1280), (1769, 1266), (1573, 1267), (4753, 1002), (9921, 219), (9995, 1024)]
text = open(path, encoding="utf-8", errors="surrogateescape").read()
spec = re.compile(r"%(?:(\d+)\.)?(\d*)s")
fixed = 0
for lineno, limit in sorted(sites, reverse=True):
    off = ft.self_offset(text, lineno)
    window = text[max(0, off-4000):off]
    starts = [m.start() for m in re.finditer(r"snprintf\(", window)]
    for s in reversed(starts):
        abs_start = max(0, off-4000) + s
        paren = text.find("(", abs_start)
        args,_ = ft.split_args(text, paren)
        if len(args) < 3: continue
        fmt = args[2]
        n = len(spec.findall(fmt))
        if n == 0: continue
        prec = max(1, (limit-1)//n - 1)
        new_fmt = spec.sub(lambda m: "%."+str(prec)+"s", fmt)
        if new_fmt == fmt: continue
        pos = text.index(fmt, paren)
        text = text[:pos]+new_fmt+text[pos+len(fmt):]
        fixed += 1
        break
if fixed:
    open(path,"w",encoding="utf-8",errors="surrogateescape",newline="").write(text)
print("fixed", fixed)