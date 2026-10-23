#pragma once
//
// A minimal PNG WRITER for RGBA8 buffers, built on zlib.
//
// WHY THIS EXISTS. The port can already DECODE PNG (the guest hands us PNG
// payloads for loadTextureFromMemory), but there was no way to get an image
// OUT of a run. Every instrument we had about the guest's output was a
// counter or a colour triple, and those can say "the back buffer is not
// black" without ever saying what is on it. The whole question "is the main
// menu open, and does it LOOK like a main menu" was unanswerable with logs.
//
// The earlier full-buffer readback already answered the narrow question:
//   LATE frame=3000 full 1280x720 lit=921600 of 921600 px0=7f8a46ff
//       quads[r_bins=226703,285945,125911,283041]
//       pts=[818c48 7f8080 03438b 182c43 0c0d0f]
// The whole buffer is lit and the four red-channel bins are all populated,
// so the guest is drawing a real image -- but WHICH image is only visible.
//
// The encoder is deliberately trivial: one IDAT, filter type 0 on every
// scanline, zlib deflate at Z_BEST_SPEED. It is not trying to be small; it
// is trying to be impossible to get wrong.
//
// NOTE ON ROW ORDER. glReadPixels returns rows bottom-up, so the caller
// flips them before calling this (or accepts the flip); PNG stores rows
// top-down. The caller decides; this function writes what it is given, in
// the order it is given.
//
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rr3 {

inline void pngPutU32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xffu));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xffu));
    out.push_back(static_cast<unsigned char>(v & 0xffu));
}

inline void pngChunk(std::vector<unsigned char>& out,
                      const char* type,
                      const unsigned char* data,
                      size_t len) {
    pngPutU32(out, static_cast<uint32_t>(len));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    if (len != 0 && data != nullptr) {
        out.insert(out.end(), data, data + len);
    }
    const uLong c = crc32(crc32(0L, Z_NULL, 0),
                           out.data() + crcStart,
                           static_cast<uInt>(out.size() - crcStart));
    pngPutU32(out, static_cast<uint32_t>(c));
}

// Writes an 8-bit RGBA PNG. `rgba` must hold width*height*4 bytes.
// Returns true only if the whole file reached the disk.
inline bool writePngRgba(const char* path,
                         int width,
                         int height,
                         const unsigned char* rgba) {
    if (path == nullptr || rgba == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    const size_t stride = static_cast<size_t>(width) * 4u;

    // Raw scanlines, each prefixed with filter type 0 (None).
    std::vector<unsigned char> raw;
    raw.reserve((stride + 1u) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0u);
        const unsigned char* row =
            rgba + static_cast<size_t>(y) * stride;
        raw.insert(raw.end(), row, row + stride);
    }

    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> comp(bound);
    const int rc = compress2(comp.data(), &bound, raw.data(),
                              static_cast<uLong>(raw.size()), Z_BEST_SPEED);
    if (rc != Z_OK) {
        return false;
    }
    comp.resize(bound);

    std::vector<unsigned char> png;
    png.reserve(comp.size() + 128u);
    static const unsigned char kSig[8] = {
        0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au
    };
    png.insert(png.end(), kSig, kSig + 8);

    unsigned char ihdr[13];
    ihdr[0] = static_cast<unsigned char>((width >> 24) & 0xff);
    ihdr[1] = static_cast<unsigned char>((width >> 16) & 0xff);
    ihdr[2] = static_cast<unsigned char>((width >> 8) & 0xff);
    ihdr[3] = static_cast<unsigned char>(width & 0xff);
    ihdr[4] = static_cast<unsigned char>((height >> 24) & 0xff);
    ihdr[5] = static_cast<unsigned char>((height >> 16) & 0xff);
    ihdr[6] = static_cast<unsigned char>((height >> 8) & 0xff);
    ihdr[7] = static_cast<unsigned char>(height & 0xff);
    ihdr[8] = 8u;  // bit depth
    ihdr[9] = 6u;  // colour type: truecolour with alpha
    ihdr[10] = 0u; // compression: deflate
    ihdr[11] = 0u; // filter method: adaptive
    ihdr[12] = 0u; // interlace: none
    pngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    pngChunk(png, "IDAT", comp.data(), comp.size());
    pngChunk(png, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return written == png.size();
}

}  // namespace rr3
