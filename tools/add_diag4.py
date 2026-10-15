path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

old = '''GuestInitializationResult initializeGuestDependencies() {
    GuestInitializationResult result{};
    result.attempted = true;
    result.stage = 1;'''
new = '''GuestInitializationResult initializeGuestDependencies() {
    GuestInitializationResult result{};
    result.attempted = true;
    result.stage = 1;
    // Diagnostic: log init start.
    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log",
        "initDeps-start", 213, 0);'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Done')
else:
    print('Pattern not found')
