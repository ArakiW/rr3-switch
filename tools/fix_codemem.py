import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

old = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
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

new = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
    // Use svcCreateCodeMemory + MapOwner with R|W|X to get a single
    // writable+executable mapping. JIT RX is read-only (faults on .data
    // writes); svcSetMemoryPermission rejects Perm_X.
    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processBacking == nullptr) continue;

        Handle codeHandle = INVALID_HANDLE;
        Result op = svcCreateCodeMemory(&codeHandle, mapped.processBacking,
                                        image.image.size());
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: CreateCodeMemory failed", 208, op);
            continue;
        }

        // Map as owner with R|W|X (value 7) at the same address.
        constexpr u32 kRwx = Perm_R | Perm_W | Perm_X;
        op = svcControlCodeMemory(codeHandle, CodeMapOperation_MapOwner,
                                  mapped.processBacking, image.image.size(), kRwx);
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: MapOwner RWX failed", 208, op);
            // Fallback: try RX only (code runs but .data writes will fault)
            op = svcControlCodeMemory(codeHandle, CodeMapOperation_MapOwner,
                                      mapped.processBacking, image.image.size(), Perm_Rx);
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "finalizeMap: MapOwner RX failed", 208, op);
                svcCloseHandle(codeHandle);
                continue;
            }
        }

        armDCacheFlush(mapped.processBacking, image.image.size());
        armICacheInvalidate(mapped.processBacking, image.image.size());

        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        mapped.targetBase = mapped.processBacking;

        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "finalizeMap: code-map ok", 208, 0);
    }
    return true;
}'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: using svcCreateCodeMemory + MapOwner RWX')
else:
    print('Old text not found')
