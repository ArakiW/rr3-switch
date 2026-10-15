import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Fix: Don't map as executable until after relocations
# writableLinkBase should stay as image.data() for relocations
# executableLinkBase should be processBacking for execution

old = b'''        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);'''

new = b'''        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        // writableLinkBase stays as image.data() for relocations (writable)
        // executableLinkBase is processBacking for execution (RX after map)
        image.writableLinkBase = nullptr;  // use image.data() for writes
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: writableLinkBase stays as image.data()')
else:
    print('Old text not found')
    idx = content.find(b'image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking)')
    if idx >= 0:
        print(f'Found at {idx}')
        print(repr(content[idx-100:idx+200]))
