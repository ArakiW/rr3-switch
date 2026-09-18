#pragma once

// Minimal PNG decoder for the JNI loadTextureFromMemory path.
// Guest-TLS-safe: no malloc/stdio; all working memory is static or stack.

#include <cstddef>
#include <cstdint>

namespace rr3 {

// Upper bound for width/height. Working buffers are sized from this.
constexpr uint32_t kPngMaxDimension = 2048;

// Session produced by pngDecodeBegin. Contents are an implementation detail
// except for width/height, which callers read after a successful begin.
struct PngDecodeSession {
    uint32_t width;
    uint32_t height;
    uint32_t channels;   // source samples per pixel: 1, 2, 3, or 4
    uint32_t stride;     // bytes per filtered scanline including filter byte
    uint32_t nextRow;
    uint8_t colorType;   // PNG IHDR color type (0,2,3,4,6)
    const uint8_t* raw;  // inflated + unfiltered scanlines (filter bytes remain)
    bool valid;
};

// Parses the PNG signature and IHDR. Returns false on malformed input.
bool pngProbeHeader(const uint8_t* data, size_t size, uint32_t* outWidth,
                    uint32_t* outHeight, uint8_t* outColorType,
                    uint8_t* outBitDepth);

// Inflates and unfilters the full image into an internal static buffer.
// On success *session is ready for pngDecodeNextRow.
bool pngDecodeBegin(PngDecodeSession* session, const uint8_t* data,
                    size_t size);

// Converts the next scanline to RGBA8888 in rgbaRow (width*4 bytes).
// Returns false on error. When the image is exhausted *outDone is set true
// and rgbaRow is left untouched.
bool pngDecodeNextRow(PngDecodeSession* session, uint8_t* rgbaRow,
                      bool* outDone);

}  // namespace rr3
