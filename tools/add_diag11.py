path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace(
    'result.completed = true;',
    'appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-completing", 223, 0);\n    result.completed = true;'
)
with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Done')
