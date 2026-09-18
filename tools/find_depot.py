import os

base = r'E:\AI\RealRacing3-switch'
for root, dirs, files in os.walk(base):
    if '.depot' in dirs:
        depot_path = os.path.join(root, '.depot')
        print(f'Found .depot at: {depot_path}')
        items = os.listdir(depot_path)
        print(f'Entries: {len(items)}')
        for item in sorted(items)[:30]:
            full = os.path.join(depot_path, item)
            if os.path.isfile(full):
                size = os.path.getsize(full)
                print(f'  {size:>12} {item}')
            else:
                print(f'  {"<dir>":>12} {item}')
        break
