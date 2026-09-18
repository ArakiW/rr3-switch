import re
with open(r'E:\AI\RealRacing3-switch\native-switch\source\jni_bridge.cpp','r',encoding='utf-8',errors='replace') as f:
    content = f.read()

matches = re.findall(r'rawEquals\(name,\s*"([^"]+)"\)', content)
print(f'Total JNI method stubs: {len(matches)}')
print()
for m in sorted(set(matches)):
    print(f'  {m}')
