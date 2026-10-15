path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Add logging after tlsScope check
old1 = '''if (!tlsScope.active()) {
        result.failureCode = 4;
        return result;
    }'''
new1 = '''if (!tlsScope.active()) {
        result.failureCode = 4;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-tls-fail", 219, 0);
        return result;
    }
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-tls-ok", 220, 0);'''

# Add logging in constructor loop
old2 = '''for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
            uintptr_t target = 0;'''
new2 = '''for (uint64_t ordinal = 0; ordinal < count; ++ordinal) {
            {
                char d[64];
                __builtin_snprintf(d, sizeof(d), "initDeps-ctor img=%d ord=%d", (int)orderIndex, (int)ordinal);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", d, 221, 0);
            }
            uintptr_t target = 0;'''

if old1 in content:
    content = content.replace(old1, new1, 1)
    print('TLS check logging added')
else:
    print('TLS check pattern not found')

if old2 in content:
    content = content.replace(old2, new2, 1)
    print('Constructor logging added')
else:
    print('Constructor pattern not found')

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
