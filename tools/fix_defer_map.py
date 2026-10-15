import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Change mapProcessCodeImages to NOT call svcMapProcessCodeMemory
# Instead, just allocate and set up link bases
# The actual mapping will happen after relocations

old = b'''        if (useRealMapping) {
            Result op = svcMapProcessCodeMemory(
                process,
                reinterpret_cast<uint64_t>(mapped.targetBase),
                reinterpret_cast<uint64_t>(mapped.processBacking),
                image.image.size());
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory failed", 208, op);
            } else {
                // Memory is already executable after svcMapProcessCodeMemory
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory ok", 208, 0);
            }
            result->segmentPermissionsEnforced = true;
            gSegmentedSessionActive = true;
            gLoaderResolutionPlanHash = 1;
        } else {
            result->segmentPermissionsEnforced = true;
            gSegmentedSessionActive = true;
            gLoaderResolutionPlanHash = 1;
        }

        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        // writableLinkBase stays as image.data() for relocations (writable)
        // executableLinkBase is processBacking for execution (RX after map)
        image.writableLinkBase = nullptr;  // use image.data() for writes
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);'''

new = b'''        // Defer svcMapProcessCodeMemory until after relocations
        // For now, just set up link bases - relocations write to writableLinkBase
        result->segmentPermissionsEnforced = true;
        gSegmentedSessionActive = true;
        gLoaderResolutionPlanHash = 1;

        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        // writableLinkBase = processBacking so relocations write directly there
        // executableLinkBase = processBacking for execution
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: deferred svcMapProcessCodeMemory')
else:
    print('Old text not found')
    idx = content.find(b'if (useRealMapping) {')
    if idx >= 0:
        print(f'Found at {idx}')
        print(repr(content[idx:idx+500]))
