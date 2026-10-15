import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

old = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
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

new = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
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
                reinterpret_cast<uint64_t>(mapped.processBacking),
                image.image.size());
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory failed", 208, op);
            } else {
                op = svcSetMemoryPermission(
                    reinterpret_cast<uint64_t>(mapped.processBacking),
                    image.image.size(), Perm_Rx);
                if (R_FAILED(op)) {
                    appendGuestBootLine(
                        "sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "mapProcCode: svcSetMemoryPermission failed", 208, op);
                }
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
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
    }
    return true;
}'''

if old in content:
    content = content.replace(old, new)
    with open(path, 'wb') as f:
        f.write(content)
    print('Fixed mapProcessCodeImages')
else:
    print('Old text not found - checking...')
    # Find the function start
    idx = content.find(b'bool mapProcessCodeImages')
    if idx >= 0:
        print(f'Found at position {idx}')
        print(content[idx:idx+200])
    else:
        print('Function not found')
