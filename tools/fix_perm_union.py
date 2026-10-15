import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Fix mapSegmentedImages permission computation: use UNION when pages overlap
# instead of failing. Also fix mapProcessCodeImages to delegate to
# mapSegmentedImages on real hardware.

old1 = b'''                if (page >= pagePermissions.size() ||
                    (pagePermissions[page] != Perm_None &&
                     pagePermissions[page] != permission)) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 12;
                    ++result->segmentedFailures;
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "segImg: perm conflict", imageIndex, page);
                    return false;
                }
                pagePermissions[page] = permission;'''

new1 = b'''                if (page >= pagePermissions.size()) {
                    result->segmentedFailureImage = imageIndex;
                    result->segmentedFailureStage = 12;
                    ++result->segmentedFailures;
                    return false;
                }
                // Use permission union on overlap (code+data sharing a page
                // is valid in ELF); do not fail.
                pagePermissions[page] |= permission;'''

# Also fix RELRO to use union
old2 = b'''            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size()) {
                    ++result->segmentedFailures;
                    return false;
                }
                pagePermissions[page] = Perm_R;
            }'''

new2 = b'''            for (uint64_t offset = start; offset < end; offset += kPageSize) {
                const size_t page = static_cast<size_t>(offset / kPageSize);
                if (page >= pagePermissions.size()) {
                    ++result->segmentedFailures;
                    return false;
                }
                pagePermissions[page] |= Perm_R;
            }'''

# Fix mapProcessCodeImages to call mapSegmentedImages on real hardware
old3 = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
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

new3 = b'''bool mapProcessCodeImages(ElfLoaderSession* result) {
    const Handle process = envGetOwnProcessHandle();
    const bool useRealMapping = (process != INVALID_HANDLE);

    appendGuestBootLine(
        "sdmc:/switch/rr3-native-rehost.guest-view.log",
        useRealMapping ? "mapProcCode: real hw (segmented)" : "mapProcCode: emulator fallback", 208, 0);

    if (useRealMapping) {
        // Use the proven per-segment mapping path.
        return mapSegmentedImages(result);
    }

    // Emulator fallback: memalign backing as target.
    result->segmentedMapperMode = 2;
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
        mapped.targetBase = mapped.processBacking;
        mapped.processMapped = true;
        image.writableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
        image.executableLinkBase = static_cast<unsigned char*>(mapped.processBacking);
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
    print('Fixed perm union (overlap)')
else:
    print('old1 not found')

if old2_crlf in content:
    content = content.replace(old2_crlf, new2_crlf)
    changed = True
    print('Fixed RELRO perm union')
else:
    print('old2 not found')

if old3_crlf in content:
    content = content.replace(old3_crlf, new3_crlf)
    changed = True
    print('Fixed mapProcessCodeImages to use mapSegmentedImages on real HW')
else:
    print('old3 not found')

if changed:
    with open(path, 'wb') as f:
        f.write(content)
    print('File updated')
