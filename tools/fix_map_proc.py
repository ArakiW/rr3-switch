import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# 1. Add diagnostics to mapSegmentedImages permission computation
old1 = b'''                if (page >= pagePermissions.size() ||
                    (pagePermissions[page] != Perm_None &&
                     pagePermissions[page] != permission)) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 12;
                    ++result->segmentedFailures;
                    return false;
                }'''

new1 = b'''                if (page >= pagePermissions.size() ||
                    (pagePermissions[page] != Perm_None &&
                     pagePermissions[page] != permission)) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 12;
                    ++result->segmentedFailures;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "segImg: perm conflict", imageIndex, page);
                    return false;
                }'''

# 2. Replace mapSegmentedImages call with mapProcessCodeImages
old2 = b'''    // Map segments with per-segment permissions (RX for code, RW for data).
    // This is the original mapSegmentedImages path that passed 11/11
    // permission ranges on real hardware.
    if (!mapSegmentedImages(&result)) {
        setError(&result, "segmented image mapping failed");
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "loader-early2: mapSegmentedImages failed", 203, 0);
        result.executableAliases = false;
        result.retained = false;
        result.completed = false;
        closeSegmentedMappings();
        gLoaderImageCount = 0;
        return result;
    }'''

new2 = b'''    // Use mapProcessCodeImages (one-pass ModuleCode mapping) - the original
    // approach that passed on real hardware.
    if (!mapProcessCodeImages(&result)) {
        setError(&result, "process-code loader mapping failed");
        appendGuestBootLine(
            "sdmc:/switch/rr3-native-rehost.guest-view.log",
            "loader-early2: mapProcessCodeImages failed", 203, 0);
        result.executableAliases = false;
        result.retained = false;
        result.completed = false;
        closeSegmentedMappings();
        gLoaderImageCount = 0;
        return result;
    }'''

# 3. Replace mapProcessCodeImages to use svcMapProcessCodeMemory on real HW
old3 = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
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
}'''

new3 = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    const bool useRealMapping = (process != INVALID_HANDLE);

    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log",
        useRealMapping ? "mapProcCode: real hw (svcMapProcessCodeMemory)" : "mapProcCode: emulator fallback", 208, 0);
    result->segmentedMapperMode = useRealMapping ? 1 : 2;
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

        if (useRealMapping && mapped.targetBase != nullptr) {
            // One-pass ModuleCode mapping: map processBacking at targetBase.
            Result op = svcMapProcessCodeMemory(
                process,
                reinterpret_cast<uint64_t>(mapped.targetBase),
                reinterpret_cast<uint64_t>(mapped.processBacking),
                image.image.size());
            if (R_FAILED(op)) {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory failed", imageIndex, op);
                // Fallback: use backing directly
                mapped.targetBase = mapped.processBacking;
            } else {
                appendGuestBootLine(
                    "sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "mapProcCode: svcMapProcessCodeMemory ok", imageIndex, 0);
            }
        } else {
            mapped.targetBase = mapped.processBacking;
        }

        mapped.processMapped = true;
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.targetBase);
    }

    result->segmentPermissionsEnforced = true;
    gSegmentedSessionActive = true;
    gLoaderResolutionPlanHash = 1;
    return true;
}'''

old1_crlf = old1.replace(b'\n', b'\r\n')
new1_crlf = new1.replace(b'\n', b'\r\n')
old2_crlf = old2.replace(b'\n', b'\r\n')
new2_crlf = new2.replace(b'\n', b'\r\n')
old3_crlf = old3.replace(b'\n', b'\r\n')
new3_crlf = new3.replace(b'\n', b'\r\n')

changed = False
if old1_crlf in content:
    content = content.replace(old1_crlf, new1_crlf)
    changed = True
    print('Added perm conflict diagnostic')
else:
    print('old1 not found')

if old2_crlf in content:
    content = content.replace(old2_crlf, new2_crlf)
    changed = True
    print('Switched to mapProcessCodeImages')
else:
    print('old2 not found')

if old3_crlf in content:
    content = content.replace(old3_crlf, new3_crlf)
    changed = True
    print('Fixed mapProcessCodeImages with svcMapProcessCodeMemory')
else:
    print('old3 not found')

if changed:
    with open(path, 'wb') as f:
        f.write(content)
    print('File updated')
