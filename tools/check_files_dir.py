import os

base = r'E:\AI\RealRacing3-switch\.tools\emulators\ryubing-mcp-0.2.0\portable\sdcard\switch\rr3-native-rehost\data\loose\files'
print(f'files/ directory: {base}')
if os.path.exists(base):
    items = os.listdir(base)
    print(f'Entries: {len(items)}')
    for item in sorted(items):
        full = os.path.join(base, item)
        if os.path.isfile(full):
            size = os.path.getsize(full)
            print(f'  {size:>12} {item}')
        else:
            print(f'  {"<dir>":>12} {item}')
else:
    print('Directory not found')
    parent = os.path.dirname(base)
    print(f'Parent: {parent}')
    if os.path.exists(parent):
        items = os.listdir(parent)
        print(f'Parent entries: {len(items)}')
        for item in sorted(items)[:20]:
            print(f'  {item}')
