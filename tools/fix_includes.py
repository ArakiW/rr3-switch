import sys

# Fix platform_adapter.h
path = r'E:\AI\RealRacing3-switch\native-switch\source\eap_port\platform_adapter.h'
with open(path, 'r') as f:
    content = f.read()

old = '''// Game directory - we'll set this to our data path
void io_set_game_dir(const char *dir);
const char *io_game_dir(void);
const char *io_writable_dir(void);

// Path helper - matches EapRules fix_path.h signature
// Returns the fixed path (may be the same pointer or a temp buffer)
const char *fix_path(const char *path, char *buf, size_t buf_len);'''

new = '''#ifdef __cplusplus
extern "C" {
#endif

// Game directory - we'll set this to our data path
void io_set_game_dir(const char *dir);
const char *io_game_dir(void);
const char *io_writable_dir(void);

// Path helper - matches EapRules fix_path.h signature
// Returns the fixed path (may be the same pointer or a temp buffer)
const char *fix_path(const char *path, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Fixed platform_adapter.h')

# Fix xt_Play.cpp
path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes\xt_Play.cpp'
with open(path, 'r') as f:
    content = f.read()
if 'trace_adapter' not in content:
    content = content.replace('#include "../jni_internals.h"', '#include "../jni_internals.h"\n#include "../../eap_port/trace_adapter.h"')
    with open(path, 'w') as f:
        f.write(content)
    print('Fixed xt_Play.cpp')
else:
    print('xt_Play.cpp already has trace_adapter')
