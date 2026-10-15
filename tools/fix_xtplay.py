import os

path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes\xt_Play.cpp'
with open(path, 'r') as f:
    content = f.read()

# Fix corrupted include
content = content.replace('"`n"', '"')

# Ensure trace_adapter is included
if 'trace_adapter' not in content:
    content = content.replace('#include "../jni_internals.h"', '#include "../jni_internals.h"\n#include "../../eap_port/trace_adapter.h"')

with open(path, 'w') as f:
    f.write(content)
print('Fixed xt_Play.cpp')
