import os

# Fix rr3_fmod_pump.h jni.h path
path = r'E:\AI\RealRacing3-switch\native-switch\source\eap_port\rr3_fmod_pump.h'
with open(path, 'r') as f:
    content = f.read()
content = content.replace('#include "jni.h"', '#include "../jni_compat/jni.h"')
with open(path, 'w') as f:
    f.write(content)
print('Fixed rr3_fmod_pump.h')

# Remove problematic JNI classes
classes_dir = r'E:\AI\RealRacing3-switch\native-switch\source\jni_compat\classes'
for f in ['rr3_Font.cpp', 'rr3_Font.h', 'rr3_font_face.cpp', 'rr3_font_face.h',
          'rr3_GlyphVector.cpp', 'rr3_GlyphVector.h', 'android_assets.cpp', 'android_assets.h']:
    path = os.path.join(classes_dir, f)
    if os.path.exists(path):
        os.remove(path)
        print(f'Removed {f}')
