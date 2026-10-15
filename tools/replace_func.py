import re

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# Find function boundaries
start = content.find('bool mapProcessCodeImages(ElfLoaderSession* result) {')
if start < 0:
    print('Function not found')
    exit(1)

# Find matching closing brace
depth = 0
end = start
for i in range(start, len(content)):
    if content[i] == '{':
        depth += 1
    elif content[i] == '}':
        depth -= 1
        if depth == 0:
            end = i + 1
            break

old_func = content[start:end]
print(f'Found function: {len(old_func)} chars')

new_func = '''bool mapProcessCodeImages(ElfLoaderSession* result) {
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
    }
    return true;
}'''

content = content[:start] + new_func + content[end:]
with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('Function replaced')
