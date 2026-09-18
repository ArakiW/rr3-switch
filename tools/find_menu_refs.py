import sys
sys.stdout.reconfigure(encoding='utf-8')
with open(r'E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so','rb') as f:
    data = f.read()

for needle in [b'MainMenu.gui', b'MainMenu.xml', b'main_menu.gui', b'SpriteAtlas', b'sprites_0']:
    idx = data.find(needle)
    if idx >= 0:
        print(f'{needle.decode()}: found at 0x{idx:x}')
    else:
        print(f'{needle.decode()}: not found')

print()
print('--- gui/processed/ files ---')
idx = 0
count = 0
while count < 30:
    idx = data.find(b'gui/processed/', idx)
    if idx < 0: break
    end = data.find(b'\x00', idx)
    s = data[idx:end].decode('ascii','replace')
    if len(s) < 80:
        print(f'0x{idx:x}: {s}')
        count += 1
    idx = end if end > idx else idx + 14
