import os

path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes\xt_Play.cpp'
with open(path, 'rb') as f:
    content = f.read()

# The file has: #include "../jni_internals.h"`n#include "../../eap_port/trace_adapter.h"
# We need to replace `"n` with newline + quote
content = content.replace(b'jni_internals.h"`n#include', b'jni_internals.h"\n#include')

with open(path, 'wb') as f:
    f.write(content)
print('Fixed xt_Play.cpp')

# Verify
with open(path, 'r') as f:
    for i, line in enumerate(f):
        if i < 10:
            print(f'{i}: {repr(line)}')
