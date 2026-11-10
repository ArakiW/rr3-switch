import re, sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft

def warnings_from(log_path):
    pattern = re.compile(r"^(.*?):(\d+):(\d+): warning: .*?-Wformat-truncation=.*?writing up to (\d+) bytes into a region of size(?: between (\d+) and (\d+)| (\d+))")
    out = []
    for line in open(log_path, encoding="utf-8", errors="replace"):
        m = pattern.match(line.strip())
        if m:
            path = m.group(1)
            line_no = int(m.group(2))
            up_to = int(m.group(4))
            lo = m.group(5); hi = m.group(6); single = m.group(7)
            limit = int(lo) if lo else (int(single) if single else int(hi))
            out.append((path, line_no, limit))
    return out

def fix_file(path, wanted):
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    fixed = 0
    for lineno, limit in sorted(set(wanted), reverse=True):
        off = ft.self_offset(text, lineno)
        start = text.rfind("snprintf", 0, off)
        if start < 0: continue
        paren = text.find("(", start)
        if paren < 0: continue
        args, _ = ft.split_args(text, paren)
        if len(args) < 3: continue
        fmt = args[2]
        count = fmt.count("%s")
        if count == 0: continue
        precision = max(1, (limit - 1) // count - 1)
        new_fmt = fmt.replace("%s", "%." + str(precision) + "s")
        if new_fmt == fmt: continue
        pos = text.index(fmt, paren)
        text = text[:pos] + new_fmt + text[pos+len(fmt):]
        fixed += 1
    if fixed:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    return fixed

wanted = warnings_from(r"E:\AI\RealRacing3-switch\.build\build-fps6.log")
by = {}
for p, l, lim in wanted:
    by.setdefault(p, []).append((l, lim))
total = 0
for p, items in by.items():
    c = fix_file(p, items)
    print("%s: %d fixed" % (p, c)); total += c
print("total", total)