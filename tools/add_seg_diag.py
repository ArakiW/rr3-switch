import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Add diagnostics to mapSegmentedRange
old1 = b'''bool mapSegmentedRange(size_t imageIndex, uint64_t pageOffset, uint64_t bytes,
                       u32 permission, ElfLoaderSession* result) {
    CrossImage& image = gLoaderImages[imageIndex];
    SegmentedImageMapping& mappedImage = gSegmentedImages[imageIndex];
    SegmentCodeMapping range{};
    range.size = static_cast<size_t>(bytes);
    range.permission = permission;
    range.targetAddress = static_cast<unsigned char*>(mappedImage.targetBase) + pageOffset;
    range.backing = memalign(kPageSize, range.size);
    result->segmentedFailureImage = imageIndex;
    result->segmentedFailureRegion = result->segmentedRegions;
    result->segmentedExpectedPermission = permission;
    if (range.backing == nullptr) {
        result->segmentedFailureStage = 2;
        ++result->segmentedFailures;
        return false;
    }
    Result operation = svcCreateCodeMemory(&range.handle, range.backing, range.size);
    if (R_FAILED(operation)) {
        result->segmentedFailureStage = 3;
        result->segmentedFailureResult = operation;
        std::free(range.backing);
        ++result->segmentedFailures;
        return false;
    }'''

new1 = b'''bool mapSegmentedRange(size_t imageIndex, uint64_t pageOffset, uint64_t bytes,
                       u32 permission, ElfLoaderSession* result) {
    CrossImage& image = gLoaderImages[imageIndex];
    SegmentedImageMapping& mappedImage = gSegmentedImages[imageIndex];
    SegmentCodeMapping range{};
    range.size = static_cast<size_t>(bytes);
    range.permission = permission;
    range.targetAddress = static_cast<unsigned char*>(mappedImage.targetBase) + pageOffset;
    range.backing = memalign(kPageSize, range.size);
    result->segmentedFailureImage = imageIndex;
    result->segmentedFailureRegion = result->segmentedRegions;
    result->segmentedExpectedPermission = permission;
    if (range.backing == nullptr) {
        result->segmentedFailureStage = 2;
        ++result->segmentedFailures;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "segRange: memalign failed", imageIndex, bytes);
        return false;
    }
    Result operation = svcCreateCodeMemory(&range.handle, range.backing, range.size);
    if (R_FAILED(operation)) {
        result->segmentedFailureStage = 3;
        result->segmentedFailureResult = operation;
        std::free(range.backing);
        ++result->segmentedFailures;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
            "segRange: CreateCodeMemory failed", imageIndex, operation);
        return false;
    }'''

# Add diagnostics to the MapOwner/MapSlave section
old2 = b'''        virtmemLock();
        range.ownerAddress = virtmemFindCodeMemory(range.size, kPageSize);
        virtmemUnlock();
        if (range.ownerAddress == nullptr) {
            result->segmentedFailureStage = 4;
            operation = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        else {
            operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapOwner,
                                             range.ownerAddress, range.size, Perm_Rw);
            range.ownerMapped = R_SUCCEEDED(operation);
            if (R_SUCCEEDED(operation)) {
                std::memcpy(range.ownerAddress, image.image.data() + pageOffset, range.size);
                armDCacheFlush(range.ownerAddress, range.size);
                operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapSlave,
                                                 range.targetAddress, range.size, permission);
                range.slaveMapped = R_SUCCEEDED(operation);
            }
        }'''

new2 = b'''        virtmemLock();
        range.ownerAddress = virtmemFindCodeMemory(range.size, kPageSize);
        virtmemUnlock();
        if (range.ownerAddress == nullptr) {
            result->segmentedFailureStage = 4;
            operation = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                "segRange: virtmemFindCodeMemory null", imageIndex, bytes);
        }
        else {
            operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapOwner,
                                             range.ownerAddress, range.size, Perm_Rw);
            range.ownerMapped = R_SUCCEEDED(operation);
            if (R_FAILED(operation)) {
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                    "segRange: MapOwner failed", imageIndex, operation);
            }
            if (R_SUCCEEDED(operation)) {
                std::memcpy(range.ownerAddress, image.image.data() + pageOffset, range.size);
                armDCacheFlush(range.ownerAddress, range.size);
                operation = svcControlCodeMemory(range.handle, CodeMapOperation_MapSlave,
                                                 range.targetAddress, range.size, permission);
                range.slaveMapped = R_SUCCEEDED(operation);
                if (R_FAILED(operation)) {
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "segRange: MapSlave failed", imageIndex, operation);
                }
            }
        }'''

old1_crlf = old1.replace(b'\n', b'\r\n')
new1_crlf = new1.replace(b'\n', b'\r\n')
old2_crlf = old2.replace(b'\n', b'\r\n')
new2_crlf = new2.replace(b'\n', b'\r\n')

changed = False
if old1_crlf in content:
    content = content.replace(old1_crlf, new1_crlf)
    changed = True
    print('Added diagnostics to mapSegmentedRange (memalign/CreateCodeMemory)')
else:
    print('old1 not found')

if old2_crlf in content:
    content = content.replace(old2_crlf, new2_crlf)
    changed = True
    print('Added diagnostics to mapSegmentedRange (MapOwner/MapSlave)')
else:
    print('old2 not found')

if changed:
    with open(path, 'wb') as f:
        f.write(content)
    print('File updated')
