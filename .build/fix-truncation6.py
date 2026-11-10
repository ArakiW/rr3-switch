import sys, re
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
base = r"E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes"
for name, items in (("glyph_vector.cpp", [(344, 96)]), ("cc_http_request.cpp", [(3784, 51)])):
    p = base + "\\" + name
    text = open(p, encoding="utf-8", errors="surrogateescape").read()
    fixed = 0
    for lineno, limit in sorted(items, reverse=True):
        off = ft.self_offset(text, lineno)
        # widen the search window: the call may start well above the line
        window = text[max(0, off-3000):off]
        starts = [m.start() for m in re.finditer(r"snprintf\(", window)]
        for s in reversed(starts):
            abs_start = max(0, off-3000) + s
            paren = text.find("(", abs_start)
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
            break
    if fixed:
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
    print(name, fixed, "fixed")