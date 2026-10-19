r"""Check every Cloudcell JNI symbol in the tree against the guest image.

Adjacent string literals are concatenated the way the compiler does, so a name
split across two lines -- "Java_..._CC_1FacebookWorker_1Class_" /
"PermissionCheckCallback" -- is checked as the single symbol it actually
becomes. A plain line-based grep cannot see that, and the whole reason 23 of 25
names went missing is that nobody could see the joined form.

Every name here was checked back against the image before being written into
the tree. Run this after touching any Java_* literal under
native-switch/source/jni_compat/classes/.

Files are read as latin-1, not utf-8: several headers in that directory carry
bytes that are not valid utf-8, and a strict decoder would abort the whole scan
on the files this exists to check. Encoding health is reported separately.

Usage:  python tools\check-cc-symbols.py
Exit 0 when every symbol is present in the image, 1 otherwise.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUEST = os.path.join(ROOT, "reverse", "native", "arm64", "libRealRacing3.so")
CLASSES = os.path.join(ROOT, "native-switch", "source", "jni_compat", "classes")

# A Java_ name starts here and runs to the closing quote. If a second quote
# follows on the next non-space line, the literal continues and the two halves
# form one symbol.
#
# The continuation group is OPTIONAL on purpose. Making it mandatory once made
# this checker match only the 9 continued literals, report "9 resolved, 0
# unexpected" and exit 0 -- a green run that had checked a quarter of the file.
SYMBOL = re.compile(r'"(Java_[A-Za-z0-9_]+)("\s*\n\s*"(?=[A-Za-z0-9_]*"))?')

# The image exports no GoogleStore or GoogleAd symbols at all. These facades
# cannot work against this guest; leaving them visible is the point.
KNOWN_ABSENT = ("GoogleStoreServiceV3", "GoogleAdManager", "EAIO_EAIO_Startup")


def main():
    with io.open(GUEST, "rb") as handle:
        blob = handle.read().decode("latin-1")

    missing, present, known_absent, nonutf8 = [], [], [], []
    for name in sorted(os.listdir(CLASSES)):
        if not name.endswith((".cpp", ".h")):
            continue
        path = os.path.join(CLASSES, name)
        raw = io.open(path, "rb").read()
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            nonutf8.append((name, exc.start))
        text = raw.decode("latin-1")
        for match in SYMBOL.finditer(text):
            symbol = match.group(1)
            if match.group(2):          # continued on the following line
                tail = re.match(r'"([^"]*)"', match.group(2)[1:])
                symbol += tail.group(1) if tail else ""
            line = text.count("\n", 0, match.start()) + 1
            entry = (name, line, symbol)
            if symbol in blob:
                present.append(entry)
            elif any(tag in symbol for tag in KNOWN_ABSENT):
                known_absent.append(entry)
            else:
                missing.append(entry)

    print("=== resolved against the image: %d ===" % len(present))
    for name, line, symbol in present:
        print("  ok    %-26s:%-5d %s" % (name, line, symbol.split("cloudcellapi_")[-1]))
    print("")
    print("=== deliberately absent (no such export in this guest): %d ===" % len(known_absent))
    for name, line, symbol in known_absent:
        print("  n/a   %-26s:%-5d %s" % (name, line, symbol.split("cloudcellapi_")[-1]))
    print("")
    print("=== UNEXPECTED misses: %d ===" % len(missing))
    for name, line, symbol in missing:
        print("  MISS  %-26s:%-5d %s" % (name, line, symbol))
    print("")
    print("=== files that are not valid utf-8: %d ===" % len(nonutf8))
    for name, offset in nonutf8:
        print("  %-26s first bad byte at offset %d" % (name, offset))
    print("")
    print("%d resolved, %d known-absent, %d unexpected" %
          (len(present), len(known_absent), len(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
