path = r'E:\AI\RealRacing3-switch\native-switch\source\main.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

old = 'rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "guest-boot-start", 211, 0);'
new = '''rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "guest-boot-start", 211, 0);
        {
            char diag[128];
            __builtin_snprintf(diag, sizeof(diag),
                "guest-boot: recovery=%d initActive=%d initDone=%d extData=%d fsTest=%x entryRet=%d viewAtt=%d",
                dashboard.recoveryMode ? 1 : 0,
                dashboard.guestInitialization.active ? 1 : 0,
                dashboard.guestInitialization.completed ? 1 : 0,
                dashboard.externalData.ready ? 1 : 0,
                dashboard.filesystemTest,
                dashboard.entryProbe.resumeReturned ? 1 : 0,
                dashboard.viewProbe.attempted ? 1 : 0);
            rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", diag, 212, 0);
        }'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Done')
else:
    print('Pattern not found')
