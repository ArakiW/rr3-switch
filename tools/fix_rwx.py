import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Replace finalizeCodeMappings to use memalign + RWX instead of jitCreate
old = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
    // Use JIT memory for executable code - this is the standard libnx approach
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processBacking == nullptr) continue;

        // Create JIT memory with separate RW and RX views
        Jit jit{};
        Result op = jitCreate(&jit, image.image.size());
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: jitCreate failed", 208, op);
            continue;
        }

        // Get RW and RX addresses
        void* rwAddr = jitGetRwAddr(&jit);
        void* rxAddr = jitGetRxAddr(&jit);
        if (rwAddr == nullptr || rxAddr == nullptr) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: jitGetAddr failed", 208, 0);
            jitClose(&jit);
            continue;
        }

        // Copy relocated code to RW view
        std::memcpy(rwAddr, image.image.data(), image.image.size());
        armDCacheFlush(rwAddr, image.image.size());

        // Update link bases to point to executable view
        image.writableLinkBase = static_cast<unsigned char*>(rwAddr);
        image.executableLinkBase = static_cast<unsigned char*>(rxAddr);
        mapped.targetBase = rxAddr;

        // Flush and invalidate caches
        armICacheInvalidate(rxAddr, image.image.size());

        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "finalizeMap: jit ok", 208, 0);
    }
    return true;
}'''

new = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
    // Use memalign + RWX permission: single writable+executable mapping.
    // JIT's RX view is read-only, which faults when guest code writes to
    // .data (e.g. JNI_OnLoad storing JavaVM at libNimble+0x2f58).
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processBacking == nullptr) continue;

        // processBacking was allocated with memalign and relocations were
        // applied to it via writableLinkBase. Make it RWX so guest code can
        // both execute and write globals.
        Result op = svcSetMemoryPermission(
            mapped.processBacking, image.image.size(), Perm_Rwx);
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: SetPermission RWX failed", 208, op);
            // Try RW as fallback - at least relocations stay valid
            op = svcSetMemoryPermission(
                mapped.processBacking, image.image.size(), Perm_Rw);
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "finalizeMap: SetPermission RW failed", 208, op);
            }
        }

        armDCacheFlush(mapped.processBacking, image.image.size());
        armICacheInvalidate(mapped.processBacking, image.image.size());

        // Both link bases point to the same RWX buffer
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        mapped.targetBase = mapped.processBacking;

        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "finalizeMap: rwx ok", 208, 0);
    }
    return true;
}'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: using memalign + RWX instead of jitCreate')
else:
    print('Old text not found')
    idx = content.find(b'bool finalizeCodeMappings')
    if idx >= 0:
        print(f'Found at {idx}')
        print(repr(content[idx:idx+200]))
