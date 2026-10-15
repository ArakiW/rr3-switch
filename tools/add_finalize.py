import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# Add finalizeCodeMappings call before the cache flush loop
old = b'''    // Flush D-cache and invalidate I-cache on the executable buffer so
    // relocated function pointers and patched code are coherent before call.
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        SegmentedImageMapping& mapped = gSegmentedImages[index];
        if (mapped.processBacking != nullptr) {
            armDCacheFlush(mapped.processBacking, gLoaderImages[index].image.size());
            armICacheInvalidate(mapped.processBacking, gLoaderImages[index].image.size());
        }
    }'''

new = b'''    // Finalize code mappings: copy relocated image and map as executable.
    finalizeCodeMappings(&result);

    // Flush D-cache and invalidate I-cache on the executable buffer so
    // relocated function pointers and patched code are coherent before call.
    for (size_t index = 0; index < gLoaderImageCount; ++index) {
        SegmentedImageMapping& mapped = gSegmentedImages[index];
        if (mapped.processBacking != nullptr) {
            armDCacheFlush(mapped.processBacking, gLoaderImages[index].image.size());
            armICacheInvalidate(mapped.processBacking, gLoaderImages[index].image.size());
        }
    }'''

old_crlf = old.replace(b'\n', b'\r\n')
new_crlf = new.replace(b'\n', b'\r\n')

if old_crlf in content:
    content = content.replace(old_crlf, new_crlf)
    with open(path, 'wb') as f:
        f.write(content)
    print('Added finalizeCodeMappings call')
else:
    print('Old text not found')
