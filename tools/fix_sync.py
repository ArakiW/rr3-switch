import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Add a sync function after relocations to copy image.data() to mapped ranges
# Insert before the cache flush loop

old = b'''    // Flush D-cache and invalidate I-cache on the executable buffer so
    // relocated function pointers and patched code are coherent before call.
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        SegmentedImageMapping& mapped = gSegmentedImages[index];
        if (mapped.processBacking != nullptr) {
            armDCacheFlush(mapped.processBacking, gLoaderImages[index].image.size());
            armICacheInvalidate(mapped.processBacking, gLoaderImages[index].image.size());
        }
    }'''

new = b'''    // Sync relocated image.data() into the mapped ranges. mapSegmentedRange
    // copied the pre-relocation image during mapping; relocations wrote to
    // image.data() via pointer() (writableLinkBase=nullptr). Copy each range
    // so the executable mapping carries the resolved addresses.
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        CrossImage& image = gLoaderImages[index];
        SegmentedImageMapping& mapped = gSegmentedImages[index];
        for (auto& range : mapped.ranges) {
            if (range.ownerAddress != nullptr && range.size > 0) {
                const size_t pageOffset = static_cast<size_t>(
                    reinterpret_cast<unsigned char*>(range.targetAddress) -
                    static_cast<unsigned char*>(mapped.targetBase));
                std::memcpy(range.ownerAddress,
                            image.image.data() + pageOffset,
                            range.size);
            }
        }
    }

    // Flush D-cache and invalidate I-cache on the executable buffer so
    // relocated function pointers and patched code are coherent before call.
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        SegmentedImageMapping& mapped = gSegmentedImages[index];
        for (auto& range : mapped.ranges) {
            if (range.ownerAddress != nullptr) {
                armDCacheFlush(range.ownerAddress, range.size);
                if (range.permission == Perm_Rx) {
                    armICacheInvalidate(range.targetAddress, range.size);
                }
            }
        }
    }'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Added relocation sync to mapped ranges')
else:
    print('Old text not found')
    idx = content.find(b'Flush D-cache and invalidate I-cache')
    if idx >= 0:
        print(f'Found at {idx}')
        print(repr(content[idx-100:idx+300]))
