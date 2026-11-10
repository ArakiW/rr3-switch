import re
sites = [("guest_filesystem.cpp",1979),("guest_filesystem.cpp",9465),("guest_filesystem.cpp",9746),
         ("guest_view_trace.cpp",1956),("jni_bridge.cpp",502),("guest_filesystem.cpp",3493),
         ("guest_filesystem.cpp",9107),("guest_filesystem.cpp",9310)]
base = r"E:\AI\RealRacing3-switch\native-switch\source"
for name, ln in sites:
    path = base + "\\" + name
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    fn = "?"
    for i in range(ln-1, 0, -1):
        l = lines[i]
        if re.match(r"^(static\s+|extern\s+\"C\"\s+)?[A-Za-z_][\w \*&:<>]*\s+\**\w+\s*\(", l) and not l.strip().startswith("//"):
            fn = l.strip()[:90]; break
    print("%-24s %5d  in  %s" % (name, ln, fn))
    print("      > " + lines[ln-1].strip()[:100])