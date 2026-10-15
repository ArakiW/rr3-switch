import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Find and replace the svcSetMemoryPermission section
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
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory ok", 208, 0);
            }'''

# Try with \r\n
old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed with CRLF')
elif old in content:
    content = content.replace(old, new)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed with LF')
else:
    print('Not found - trying manual replace')
    # Find the exact position and replace
    idx = content.find(b'op = svcSetMemoryPermission(')
    if idx >= 0:
        # Find the end of this block
        end_marker = b'            }'
        end_idx = content.find(end_marker, idx)
        if end_idx >= 0:
            # Find the closing of the else block
            # The block is: } else { ... }
            # Find the matching closing brace
            brace_count = 0
            search_start = idx
            # Go back to find "else {"
            else_idx = content.rfind(b'} else {', 0, idx)
            if else_idx >= 0:
                # Find the matching closing brace for the else block
                brace_count = 1
                i = else_idx + len(b'} else {')
                while i < len(content) and brace_count > 0:
                    if content[i:i+1] == b'{':
                        brace_count += 1
                    elif content[i:i+1] == b'}':
                        brace_count -= 1
                    i += 1
                # i now points past the closing brace
                old_block = content[else_idx:i]
                new_block = b'''} else {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory ok", 208, 0);
            }'''
                new_block_crlf = new_block.replace(b'\n', b'\r\n')
                content = content.replace(old_block, new_block_crlf)
                with open(path, 'wb') as f:
                    f.write(content)
                print('Fixed with manual replace')
            else:
                print('Could not find else block')
        else:
            print('Could not find end marker')
    else:
        print('svcSetMemoryPermission not found')
