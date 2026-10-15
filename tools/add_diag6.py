path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace(
    'result.preflightPassed = true;',
    '''result.preflightPassed = true;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-preflight-ok", 216, 0);'''
)

content = content.replace(
    'result.stage = 3;',
    '''result.stage = 3;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-stage3", 217, 0);'''
)

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Done')
