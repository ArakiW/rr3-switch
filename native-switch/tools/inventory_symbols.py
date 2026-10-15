from pathlib import Path
import re

src = Path(r"E:\AI\RealRacing3-switch\native-switch\source\symbol_resolver.cpp")
t = src.read_text(encoding="utf-8", errors="replace")
binds = re.findall(r"RR3_BIND\((\w+)\)", t)
shims = re.findall(r"RR3_SHIM\(\"([^\"]+)\"", t)
print("BIND", len(binds), "SHIM", len(shims))
gl = sorted(b for b in binds if b.lower().startswith(("gl", "egl")))
print("GL/EGL binds", len(gl))
for x in gl:
    print(" ", x)
print("egl-related shims", [s for s in shims if "egl" in s.lower()])

jni = Path(r"E:\AI\RealRacing3-switch\native-switch\source\jni_bridge.cpp")
j = jni.read_text(encoding="utf-8", errors="replace")
print("jni_bridge chars", len(j))
# method bindings
for pat in [
    "GetStaticMethodID",
    "GetMethodID",
    "CallObjectMethod",
    "CallVoidMethod",
    "CallIntMethod",
    "CallBooleanMethod",
    "FindClass",
    "NewStringUTF",
    "GetStringUTFChars",
    "RegisterNatives",
]:
    print(f"  {pat}: {j.count(pat)}")
