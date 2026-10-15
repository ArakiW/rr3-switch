import sys

path = r'E:\AI\RealRacing3-switch\native-switch\source\multi_dso_linker.cpp'
with open(path, 'rb') as f:
    content = f.read()

# 1. Replace mapProcessCodeImages call with mapSegmentedImages
old1 = b'''    // Map backing memory and set link bases BEFORE relocations so that
    // applyLoaderTable writes resolved addresses into the executable buffer.
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

new1 = b'''    // Map segments with per-segment permissions (RX for code, RW for data).
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

# 2. Remove finalizeCodeMappings call (mapSegmentedImages handles everything)
old2 = b'''    // Finalize code mappings: copy relocated image and map as executable.
    finalizeCodeMappings(&result);

    // Flush D-cache and invalidate I-cache on the executable buffer so'''

new2 = b'''    // Flush D-cache and invalidate I-cache on the executable buffer so'''

old1_crlf = old1.replace(b'\n', b'\r\n')
new1_crlf = new1.replace(b'\n', b'\r\n')
old2_crlf = old2.replace(b'\n', b'\r\n')
new2_crlf = new2.replace(b'\n', b'\r\n')

changed = False
if old1_crlf in content:
    content = content.replace(old1_crlf, new1_crlf)
    changed = True
    print('Replaced mapProcessCodeImages with mapSegmentedImages')
else:
    print('old1 not found')

if old2_crlf in content:
    content = content.replace(old2_crlf, new2_crlf)
    changed = True
    print('Removed finalizeCodeMappings call')
else:
    print('old2 not found')

if changed:
    with open(path, 'wb') as f:
        f.write(content)
    print('File updated')
