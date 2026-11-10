import sys, re
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
base = r"E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes"
for name, items in (("glyph_vector.cpp", [(344, 96)]), ("cc_http_request.cpp", [(1340, 148), (3784, 188)])):
    p = base + "\\" + name
    text = open(p, encoding="utf-8", errors="surrogateescape").read()
    fixed = 0
    for lineno, limit in sorted(items, reverse=True):
        off = ft.self_offset(text, lineno)
        start = text.rfind("snprintf", 0, off)
        if start < 0: continue
        paren = text.find("(", start)
        args, _ = ft.split_args(text, paren)
        if len(args) < 3: continue
        fmt = args[2]
        n = fmt.count("%s")
        if n == 0: continue
        prec = max(1, (limit - 1) // n - 1)
        new_fmt = fmt.replace("%s", "%." + str(prec) + "s")
        if new_fmt == fmt: continue
        pos = text.index(fmt, paren)
        text = text[:pos] + new_fmt + text[pos+len(fmt):]
        fixed += 1
    if fixed:
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    print(name, fixed, "fixed")