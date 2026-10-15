path = r'E:\AI\RealRacing3-switch\native-switch\source\main.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Replace the auto-boot block to skip dependency init entirely
old = '''// Skip dependency init - force complete
        dashboard.guestInitialization.completed = true;
        dashboard.guestInitialization.active = true;
        rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "force-init-complete", 224, 0);
        rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "dep-init-done", 215, 0);
        if (automaticGuestBoot) {
            runGuardedGuestBootProbe();
        }'''

new = '''// Force complete and go straight to guest boot
        dashboard.guestInitialization.completed = true;
        dashboard.guestInitialization.active = true;
        rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "force-init-complete", 224, 0);
        if (automaticGuestBoot) {
            rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "calling-guest-boot", 226, 0);
            runGuardedGuestBootProbe();
            rr3::appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "guest-boot-returned", 227, 0);
        }'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Done')
else:
    print('Pattern not found')
