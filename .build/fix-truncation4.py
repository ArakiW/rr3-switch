import sys
sys.path.insert(0, r"E:\AI\RealRacing3-switch\.build")
import fix_truncation as ft
base = r"E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes"
for name, items in (("glyph_vector.cpp", [(344, 96)]), ("cc_http_request.cpp", [(1340, 148), (3784, 188)])):
    p = base + "\\" + name
    print(name, ft.fix_file(p, items), "fixed")