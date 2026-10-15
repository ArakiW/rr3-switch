path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

old = '''GuestStartupPlan captureGuestStartupPlan() {
    GuestStartupPlan plan{};
    if (!gSegmentedSessionActive || gLoaderImageCount == 0 ||
        gLifecycleOrderCount != gLoaderImageCount) {
        return plan;
    }'''

new = '''GuestStartupPlan captureGuestStartupPlan() {
    GuestStartupPlan plan{};
    // Diagnostic: log startup plan conditions.
    {
        char diag[128];
        __builtin_snprintf(diag, sizeof(diag),
            "startupPlan: active=%d imgCount=%d lifeCount=%d",
            gSegmentedSessionActive ? 1 : 0,
            (int)gLoaderImageCount, (int)gLifecycleOrderCount);
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            diag, 209, 0);
    }
    if (!gSegmentedSessionActive || gLoaderImageCount == 0 ||
        gLifecycleOrderCount != gLoaderImageCount) {
        return plan;
    }'''

if old in content:
    content = content.replace(old, new, 1)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Done')
else:
    print('Pattern not found')
