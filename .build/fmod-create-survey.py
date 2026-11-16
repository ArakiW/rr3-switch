# -*- coding: utf-8 -*-
"""Where is FMOD_System_Create in the guest's own audio library, and what does
it call?"""
import io, os, re, subprocess, sys

RE = sys.argv[1]
SO = sys.argv[2]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace").stdout


syms = run([RE + "-readelf", "--dyn-syms", "-W", SO])
for line in syms.split("\n"):
    if re.search(r"\b(FMOD_System_Create|_ZN4FMOD6System4initEijPv|_ZN4FMOD6System10createSound)", line):
        print("SYM:", " ".join(line.split())[:200])
print()
# the static symbol table (if any) and the section layout
secs = run([RE + "-readelf", "-S", "-W", SO])
for line in secs.split("\n"):
    if ".text" in line or ".dynsym" in line or ".rela" in line:
        print("SEC:", " ".join(line.split())[:160])
print()
# the undefined symbols the library needs
undef = set()
for line in syms.split("\n"):
    m = re.match(r"\s*\d+:\s+[0-9a-f]+\s+\d+\s+\S+\s+\S+\s+\S+\s+UND\s+(\S+)", line)
    if m:
        undef.add(m.group(1))
interesting = sorted(s for s in undef if re.search(r"(?i)new|delete|malloc|pthread_once|atexit|JNI|dlopen|dlsym|mmap", s))
print("undefined, allocator/thread/JNI-ish:", len(interesting))
for s in interesting:
    print("   ", s)
