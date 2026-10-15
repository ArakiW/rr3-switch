path = r'E:\AI\RealRacing3-switch\native-switch\source\main.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

old = '''dashboard.guestInitialization =
                rr3::initializeGuestDependencies();'''
new = '''dashboard.guestInitialization =
                rr3::initializeGuestDependencies();
            {
                char d[64];
                __builtin_snprintf(d, sizeof(d), "initDeps-result: done=%d active=%d",
                    dashboard.guestInitialization.completed ? 1 : 0,
                    dashboard.guestInitialization.active ? 1 : 0);
                rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", d, 222, 0);
            }'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Done')
else:
    print('Pattern not found')
