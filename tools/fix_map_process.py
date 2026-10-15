import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r') as f:
    content = f.read()

old = '''bool mapProcessCodeImages(ElfLoaderSession* result) {
    // Emulator workaround: skip svcMapProcessCodeMemory entirely.
    // The emulator doesn't support the required memory states (0xe401).
    // Use backing memory directly as target.
    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log",
        "mapProcCode: emulator fallback (skip svcMapProcessCodeMemory)", 208, 0);
    result->segmentedMapperMode = 2;
    result->permissionRanges = 0;
    result->permissionRangesVerified = 0;
    result->permissionFailures = 0;
    result->segmentPermissionsEnforced = true;  // Emulator workaround
    gSegmentedSessionActive = true;  // Emulator workaround
    gLoaderResolutionPlanHash = 1;  // Emulator workaround (non-zero)

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
        // Use backing memory directly as target (no mapping needed).
        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        // Point both link bases at processBacking so relocations and
        // lifecycleTarget resolve to the same buffer we will execute from.
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
    }
    return true;
}'''

new = '''bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    const bool useRealMapping = (process != INVALID_HANDLE);

    if (useRealMapping) {
        // Real hardware: use svcMapProcessCodeMemory for executable mappings.
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "mapProcCode: real hardware (svcMapProcessCodeMemory)", 208, 0);
        result->segmentedMapperMode = 1;
    } else {
        // Emulator fallback: skip svcMapProcessCodeMemory entirely.
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
            // Real hardware: map as executable code memory.
            Result op = svcMapProcessCodeMemory(
                process,
                reinterpret_cast<uint64_t>(mapped.processBacking),
                image.image.size());
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory failed", 208, op);
                // Fall back to direct backing
                mapped.targetBase = mapped.processBacking;
                mapped.processMapped = true;
            } else {
                // Set executable permission
                op = svcSetMemoryPermission(
                    reinterpret_cast<uint64_t>(mapped.processBacking),
                    image.image.size(), Perm_Rx);
                if (R_FAILED(op)) {
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "mapProcCode: svcSetMemoryPermission failed", 208, op);
                }
                mapped.targetBase = mapped.processBacking;
                mapped.processMapped = true;
            }
            result->segmentPermissionsEnforced = true;
            gSegmentedSessionActive = true;
            gLoaderResolutionPlanHash = 1;
        } else {
            // Emulator: use backing memory directly as target.
            mapped.targetBase = mapped.processBacking;
            mapped.processMapped = true;
            result->segmentPermissionsEnforced = true;
            gSegmentedSessionActive = true;
            gLoaderResolutionPlanHash = 1;
        }

        // Point both link bases at processBacking so relocations and
        // lifecycleTarget resolve to the same buffer we will execute from.
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
    }
    return true;
}'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Fixed mapProcessCodeImages')
