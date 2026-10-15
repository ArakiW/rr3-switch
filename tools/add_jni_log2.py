import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\jni_bridge.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Add logging before and after JNI_OnLoad call - use \n line endings
old = b'''        // JNI_OnLoad receives the JavaVM handle (&g_vmVtable) and a reserved
        // arg (null on Android); it returns the required JNI version.
        const uintptr_t version =
            reinterpret_cast<uintptr_t (*)(void*, void*)>(targets[index])(
                &g_vmVtable, nullptr);
        probe.lastVersion = static_cast<uint32_t>(version);
        ++probe.imagesCalled;'''

new = b'''        // JNI_OnLoad receives the JavaVM handle (&g_vmVtable) and a reserved
        // arg (null on Android); it returns the required JNI version.
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "jniOnLoad-call", index, targets[index]);
        const uintptr_t version =
            reinterpret_cast<uintptr_t (*)(void*, void*)>(targets[index])(
                &g_vmVtable, nullptr);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "jniOnLoad-ret", index, version);
        probe.lastVersion = static_cast<uint32_t>(version);
        ++probe.imagesCalled;'''

if old in content:
    content = content.replace(old, new)
    with open(path, 'wb') as f:
        f.write(content)
    print('Added JNI_OnLoad call logging')
else:
    print('Old text not found')
