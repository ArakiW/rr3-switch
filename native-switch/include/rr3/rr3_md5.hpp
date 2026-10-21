#pragma once

// MD5 (RFC 1321), for the asset manifest's md5 column.
//
// WHY IT EXISTS. The guest's asset manifest (files/apk/res/
// asset_list_gui_assets_core.txt) is a tab-separated list of
//
//     <path>\t<md5>\t<size>\t<size>
//
// and the guest's ImageResManager builds its atlas registry from it. A file
// that the manifest does not name is not in the registry, so
// ImageResManager::loadImageFromAtlas reports "Failed to load image from
// atlas" WITHOUT ever touching the filesystem.
//
// So the port completes that manifest, and a completed line must carry the
// file's REAL digest. A digest that is copied from anywhere else, or
// invented, is a lie the guest can act on; this one is computed from the
// file's own bytes, every time, on the deployed data.
//
// This is deliberately a SEPARATE translation unit: the implementation is
// self-contained and can be compiled and checked on the host against a
// reference implementation, which the port cannot do for itself.

#include <cstddef>
#include <cstdint>

namespace rr3 {

class Md5 {
public:
    void begin();
    void update(const void* data, std::size_t length);
    // Writes 32 lowercase hex characters plus a NUL.
    void finish(char out[33]);

private:
    void transform(const std::uint8_t block[64]);

    std::uint32_t mState[4]{};
    std::uint64_t mBits{0};
    std::uint8_t mBuffer[64]{};
    std::size_t mBuffered{0};
};

// md5 of a file, read in 64 KiB chunks.
//
// Returns false and leaves out[] untouched when the file cannot be read.
// sizeOut, when not null, receives the file's size in bytes.
bool md5File(const char* path, char out[33], unsigned long long* sizeOut);

} // namespace rr3
