import subprocess, re, os
BIN = r'E:\AI\RealRacing3-switch\.tools\devkitPro\msys64\opt\devkitpro\devkitA64\aarch64-none-elf\bin'
READELF = os.path.join(BIN, 'readelf.exe')

def dynsyms(path):
    out = subprocess.run([READELF, '--dyn-syms', '-W', path],
                         capture_output=True, text=True, errors='replace').stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r'\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\S+)\s+(\S+)\s*$', line)
        if m:
            rows.append({
                'value': int(m.group(1), 16), 'size': int(m.group(2)),
                'type': m.group(3), 'bind': m.group(4), 'vis': m.group(5),
                'ndx': m.group(6), 'name': m.group(7),
            })
    return rows

ROM = r'E:\AI\RealRacing3-switch\native-switch\romfs'
fm = dynsyms(os.path.join(ROM, 'libfmodex.so'))
und = [r for r in fm if r['ndx'] == 'UND' and r['name']]
print('libfmodex dynsym rows:', len(fm), ' undefined:', len(und))

src = open(r'E:\AI\RealRacing3-switch\native-switch\source\symbol_resolver.cpp',
           encoding='utf-8', errors='replace').read()
host = set(re.findall(r'RR3_SHIM\(\s*"([^"]+)"', src))
print('port host symbol table entries:', len(host))

need = set()
for r in und:
    need.add(r['name'].split('@')[0])
missing = sorted(n for n in need if n not in host)
print('libfmodex needs', len(need), 'unique; NOT in port host table:', len(missing))
for n in missing:
    print('   NEEDS', n)

# which guest images define them
for img in ('libc++_shared.so', 'libRealRacing3.so', 'libNimble.so'):
    rows = dynsyms(os.path.join(ROM, img))
    defined = set(r['name'].split('@')[0] for r in rows if r['ndx'] != 'UND' and r['name'])
    hit = sorted(n for n in missing if n in defined)
    print(img, 'defines', len(defined), 'of the missing:', len(hit))
    for n in hit[:20]:
        print('     ', n)
