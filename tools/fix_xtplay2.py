import os

path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes\xt_Play.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Check for corrupted bytes
print(f'File size: {len(content)}')
print(f'Contains backtick-n: {b"`n" in content}')
print(f'Contains literal backslash-n: {b"\\n" in content}')

# Replace backtick-n with actual newline
content = content.replace(b'"`n"', b'"')

with open(path, 'wb') as f:
    f.write(content)
print('Fixed xt_Play.cpp binary')

# Verify
with open(path, 'r') as f:
    for i, line in enumerate(f):
        if i < 10:
            print(f'{i}: {repr(line)}')
