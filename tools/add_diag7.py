path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace(
    'GuestTlsScope tlsScope;',
    '''GuestTlsScope tlsScope;
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log", "initDeps-tls-scope", 218, 0);'''
)

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Done')
