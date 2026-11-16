# -*- coding: utf-8 -*-
"""R1 feasibility: what does the guest need from FMOD, and what does the port
intercept?

READ ONLY.  Three sources are cross-referenced:
  1. the guest's libRealRacing3.so  -- the FMOD symbols it IMPORTS
  2. the guest's libfmodex.so       -- what it EXPORTS and what it IMPORTS
     (an OpenSL/AAudio import here is the difference between "the real FMOD
     could produce PCM" and "the real FMOD has nowhere to put it")
  3. symbol_resolver.cpp            -- what the port intercepts
"""
import io, os, re, subprocess, sys

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
        if name.startswith("_") or name.startswith("__"):
            continue
        if ndx == "UND":
            undef.add(name)
        else:
            defined.add(name)
    return undef, defined


game_undef, _ = dynsyms(os.path.join(ROMSFS, "libRealRacing3.so"))
fmod_undef, fmod_def = dynsyms(os.path.join(ROMSFS, "libfmodex.so"))

resolver = io.open(os.path.join(REPO, "native-switch", "source", "symbol_resolver.cpp"),
                   encoding="utf-8", errors="replace").read()
intercepted = set(re.findall(r'RR3_SHIM\("([^"]+)"', resolver))
fmod_intercepted = {s for s in intercepted if "FMOD" in s or s.startswith("_ZN4FMOD")}

# what the GAME imports that looks like FMOD
game_fmod_imports = {s for s in game_undef if "FMOD" in s}
# the game's FMOD imports that the real library actually exports
covered = {s for s in game_fmod_imports if s in intercepted}
not_covered = {s for s in game_fmod_imports if s not in intercepted}

print("=== the guest game imports %d FMOD symbols ===" % len(game_fmod_imports))
print("intercepted by the port : %d" % len(covered))
print("NOT intercepted          : %d" % len(not_covered))
for s in sorted(not_covered)[:25]:
    exported = s in fmod_def
    print("   %-70s exported_by_libfmodex=%s" % (s[:70], exported))
print()
print("=== the real libfmodex.so imports (its own dependencies) ===")
print("total undefined: %d" % len(fmod_undef))
audio = sorted(s for s in fmod_undef if re.search(r"(^sl[A-Z]|^SLES|AAudio|aaudio|OpenSL|MMDev|Audio)", s))
print("audio-backend imports: %d" % len(audio))
for s in audio[:40]:
    print("   ", s)
print()
print("=== does the real libfmodex export the FMOD API the game imports? ===")
missing = sorted(s for s in game_fmod_imports if s not in fmod_def)
print("game FMOD imports NOT exported by libfmodex.so: %d" % len(missing))
for s in missing[:20]:
    print("   ", s)
print()
print("=== the port's FMOD interception table: %d entries ===" % len(fmod_intercepted))
print("sample:", sorted(fmod_intercepted)[:6])
