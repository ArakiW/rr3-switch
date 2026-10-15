import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Replace the entire mapProcessCodeImages function
old = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    const bool useRealMapping = (process != INVALID_HANDLE);

    if (useRealMapping) {
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "mapProcCode: real hardware (svcMapProcessCodeMemory)", 208, 0);
        result->segmentedMapperMode = 1;
    } else {
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "mapProcCode: emulator fallback (skip svcMapProcessCodeMemory)", 208, 0);
        result->segmentedMapperMode = 2;
    }
    result->permissionRanges = 0;
    result->permissionRangesVerified = 0;
    result->permissionFailures = 0;

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        mapped.processBacking = memalign(kPageSize, image.image.size());
        if (mapped.processBacking == nullptr) {
            result->segmentedFailureStage = 2;
            ++result->segmentedFailures;
            unmapProcessCodeImages();
            return false;
        }
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());

        if (useRealMapping) {
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
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
    }
    return true;
}'''

new = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    const bool useRealMapping = (process != INVALID_HANDLE);

    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log",
        useRealMapping ? "mapProcCode: real hardware (defer map)" : "mapProcCode: emulator fallback", 208, 0);
    result->segmentedMapperMode = useRealMapping ? 1 : 2;
    result->permissionRanges = 0;
    result->permissionRangesVerified = 0;
    result->permissionFailures = 0;
    result->segmentPermissionsEnforced = true;
    gSegmentedSessionActive = true;
    gLoaderResolutionPlanHash = 1;

    for (size_t imageIndex = 0; imageIndex < gLoaderImageCount; ++imageIndex) {
        CrossImage& image = gLoaderImages[imageIndex];
        SegmentedImageMapping& mapped = gSegmentedImages[imageIndex];
        mapped.processBacking = memalign(kPageSize, image.image.size());
        if (mapped.processBacking == nullptr) {
            result->segmentedFailureStage = 2;
            ++result->segmentedFailures;
            unmapProcessCodeImages();
            return false;
        }
        std::memcpy(mapped.processBacking, image.image.data(), image.image.size());
        armDCacheFlush(mapped.processBacking, image.image.size());

        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        // Both link bases point to processBacking - relocations write here
        // svcMapProcessCodeMemory will be called AFTER relocations
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
    }
    return true;
}

bool finalizeCodeMappings(ElfLoaderSession* result) {
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

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed: added finalizeCodeMappings')
else:
    print('Old text not found')
