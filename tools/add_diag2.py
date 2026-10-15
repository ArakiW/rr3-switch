path = r'E:\AI\RealRacing3-switch\native-switch\source\main.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

old1 = 'const auto runGuardedDependencyInitialization = [&]() {'
new1 = '''const auto runGuardedDependencyInitialization = [&]() {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "dep-init-start", 210, 0);'''

old2 = 'const auto runGuardedGuestBootProbe = [&]() {'
new2 = '''const auto runGuardedGuestBootProbe = [&]() {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "guest-boot-start", 211, 0);'''

content = content.replace(old1, new1, 1)
content = content.replace(old2, new2, 1)

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Done')
