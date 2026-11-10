import re, io
p = r"E:\AI\RealRacing3-switch\native-switch\source\guest_view_probe.cpp"
lines = io.open(p, encoding="utf-8", errors="replace").read().split("\n")
# find the enclosing function of line 7297 (1-indexed)
target = 7296
for i in range(target, 0, -1):
    l = lines[i]
    if re.match(r"^[A-Za-z_][\w \*&:]*\([^;]*\)\s*\{?\s*$", l) and not l.strip().startswith("//") and "(" in l:
        print("enclosing function candidate at line", i+1, ":", l.strip()[:160])
        break
# all top-level function definitions
names = []
for i, l in enumerate(lines):
    m = re.match(r"^(?:static\s+)?(?:extern \"C\"\s+)?[A-Za-z_][\w \*&:<>]*\b(\w+)\s*\(", l)
    if m and not l.strip().startswith("//") and "=" not in l.split("(")[0]:
        names.append((i+1, m.group(1), l.strip()[:100]))
for n in names:
    if 6000 <= n[0] <= 7400:
        print(n)