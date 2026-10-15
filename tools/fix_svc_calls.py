import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Fix svcMapProcessCodeMemory call - needs (proc, dst, src, size)
old1 = b'Result op = svcMapProcessCodeMemory(\r\n                process,\r\n                reinterpret_cast<uint64_t>(mapped.processBacking),\r\n                image.image.size());'
new1 = b'Result op = svcMapProcessCodeMemory(\r\n                process,\r\n                reinterpret_cast<uint64_t>(mapped.targetBase),\r\n                reinterpret_cast<uint64_t>(mapped.processBacking),\r\n                image.image.size());'

# Fix svcSetMemoryPermission call - first arg is void*
old2 = b'op = svcSetMemoryPermission(\r\n                    reinterpret_cast<uint64_t>(mapped.processBacking),\r\n                    image.image.size(), Perm_Rx);'
new2 = b'op = svcSetMemoryPermission(\r\n                    mapped.processBacking,\r\n                    image.image.size(), Perm_Rx);'

changed = False
if old1 in content:
    content = content.replace(old1, new1)
    changed = True
    print('Fixed svcMapProcessCodeMemory call')
else:
    print('old1 not found')

if old2 in content:
    content = content.replace(old2, new2)
    changed = True
    print('Fixed svcSetMemoryPermission call')
else:
    print('old2 not found')

if changed:
    with open(path, 'wb') as f:
        f.write(content)
    print('File updated')
