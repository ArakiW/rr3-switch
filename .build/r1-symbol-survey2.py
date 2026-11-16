# -*- coding: utf-8 -*-
"""R1 feasibility, take two: the mangled C++ names must be kept this time."""
import io, os, re, subprocess

REPO = r"E:\AI\RealRacing3-switch"
READELF = os.path.join(REPO, ".tools", "devkitPro", "msys64", "opt", "devkitpro",
                        "devkitA64", "bin", "aarch64-none-elf-readelf.exe")
ROMSFS = os.path.join(REPO, "native-switch", "romfs")


def dynsyms(path):
    out = subprocess.run([READELF, "--dyn-syms", "-W", path],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    undef, defined = set(), set()
    for line in out.stdout.split("\n"):
        m = re.match(r"\s*\d+:\s+[0-9a-f]+\s+\d+\s+\S+\s+\S+\s+\S+\s+(\S+)\s+(\S+)", line)
        if not m:
            continue
        ndx, name = m.group(1), m.group(2)
        if ndx == "UND":
            undef.add(name)
        else:
            defined.add(name)
    return undef, defined


game_undef, game_def = dynsyms(os.path.join(ROMSFS, "libRealRacing3.so"))
fmod_undef, fmod_def = dynsyms(os.path.join(ROMSFS, "libfmodex.so"))

resolver = io.open(os.path.join(REPO, "native-switch", "source", "symbol_resolver.cpp"),
                   encoding="utf-8", errors="replace").read()
intercepted = set(re.findall(r'RR3_SHIM\("([^"]+)"', resolver))

game_fmod = sorted(s for s in game_undef if s.startswith("_ZN4FMOD") or s.startswith("FMOD_"))
covered = [s for s in game_fmod if s in intercepted]
missing = [s for s in game_fmod if s not in intercepted]

print("=== the game imports %d FMOD symbols ===" % len(game_fmod))
print("intercepted by the port : %d" % len(covered))
print("NOT intercepted          : %d" % len(missing))
for s in missing[:30]:
    print("   %-72s exported_by_libfmodex=%s" % (s[:72], s in fmod_def))
print()
print("=== libfmodex.so: %d exports, %d imports ===" % (len(fmod_def), len(fmod_undef)))
print("exports the game's FMOD API: %d of %d"
      % (len([s for s in game_fmod if s in fmod_def]), len(game_fmod)))
print()
print("=== libfmodex.so's own imports (all) ===")
print(", ".join(sorted(fmod_undef)[:120]))
print()
print("=== dlopen targets inside libfmodex.so ===")
blob = open(os.path.join(ROMSFS, "libfmodex.so"), "rb").read()
for needle in (b"libOpenSLES.so", b"libaaudio.so", b"libfmod.so", b"libdl.so", b"OpenSL"):
    print("   %-16s present=%s" % (needle.decode(), needle in blob))
