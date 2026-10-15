import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Replace finalizeCodeMappings to use svcCreateCodeMemory + svcControlCodeMemory
old = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    if (process == INVALID_HANDLE) {
        return true;  // emulator - nothing to do
    }

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processBacking == nullptr) continue;

        // Copy relocated image to processBacking
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());

        // Now map as executable code memory
        Result op = svcMapProcessCodeMemory(
            process,
            reinterpret_cast<uint64_t>(mapped.targetBase),
            reinterpret_cast<uint64_t>(mapped.processBacking),
            image.image.size());
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: svcMapProcessCodeMemory failed", 208, op);
        } else {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: ok", 208, 0);
            armICacheInvalidate(mapped.processBacking, image.image.size());
        }
    }
    return true;
}'''

new = b'''bool finalizeCodeMappings(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    if (process == INVALID_HANDLE) {
        return true;  // emulator - nothing to do
    }

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        if (mapped.processBacking == nullptr) continue;

        // Copy relocated image to processBacking
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());

        // Use svcCreateCodeMemory + svcControlCodeMemory for executable mapping
        Handle codeHandle = INVALID_HANDLE;
        Result op = svcCreateCodeMemory(&codeHandle, mapped.processBacking, image.image.size());
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: svcCreateCodeMemory failed", 208, op);
            continue;
        }

        // Map as owner with RW permissions first
        op = svcControlCodeMemory(codeHandle, CodeMapOperation_MapOwner,
                                  mapped.processBacking, image.image.size(), Perm_Rw);
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: MapOwner failed", 208, op);
            svcCloseHandle(codeHandle);
            continue;
        }

        // Copy code again (now in mapped region)
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());

        // Map as slave with RX permissions for execution
        op = svcControlCodeMemory(codeHandle, CodeMapOperation_MapSlave,
                                  mapped.processBacking, image.image.size(), Perm_Rx);
        if (R_FAILED(op)) {
            appendGuestBootLine(
                "sdmc:/switch/rr3-native-rehost.guest-view.log",
                "finalizeMap: MapSlave failed", 208, op);
            svcCloseHandle(codeHandle);
            continue;
        }

        armICacheInvalidate(mapped.processBacking, image.image.size());
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "finalizeMap: ok", 208, 0);
    }
    return true;
}'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: using svcCreateCodeMemory + svcControlCodeMemory')
else:
    print('Old text not found')
