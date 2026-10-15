import sys

# Fix rr3_fmod_pump.cpp
path = r'E:\AI\RealRacing3-switch\native-switch\source\eap_port\rr3_fmod_pump.cpp'
with open(path, 'r') as f:
    content = f.read()
content = content.replace('../../eap_port/platform_adapter.h', 'platform_adapter.h')
content = content.replace('../../eap_port/trace_adapter.h', 'trace_adapter.h')
with open(path, 'w') as f:
    f.write(content)
print('Fixed rr3_fmod_pump.cpp')

# Fix xt_Play.cpp
path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes\xt_Play.cpp'
with open(path, 'r') as f:
    content = f.read()
# Fix the corrupted include
content = content.replace('"`n"', '"')
# Ensure trace_adapter is included
if 'trace_adapter' not in content:
    content = content.replace('#include "../jni_internals.h"', '#include "../jni_internals.h"\n#include "../../eap_port/trace_adapter.h"')
with open(path, 'w') as f:
    f.write(content)
print('Fixed xt_Play.cpp')
