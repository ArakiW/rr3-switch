import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

old = b'''            } else {
                op = svcSetMemoryPermission(
                    mapped.processBacking,
                    image.image.size(), Perm_Rx);
                if (R_FAILED(op)) {
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "mapProcCode: svcSetMemoryPermission failed", 208, op);
                }
            }'''

new = b'''            } else {
                // Memory is already executable after svcMapProcessCodeMemory
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory ok", 208, 0);
            }'''

if old in content:
    content = content.replace(old, new)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: removed svcSetMemoryPermission call')
else:
    print('Old text not found')
