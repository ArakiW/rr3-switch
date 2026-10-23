//
// The framebuffer capture, in its own translation unit.
//
// See include/rr3/guest_frame_capture.hpp for why this is not inline in
// guest_view_probe.cpp.
//
// The encoder lives in rr3_png_write.hpp: one IDAT, filter type 0 on
// every scanline, zlib deflate at Z_BEST_SPEED. It is not trying to be
// small; it is trying to be impossible to get wrong.
//
#include "rr3/guest_frame_capture.hpp"

#include "rr3_png_write.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {

bool writeFramePng(const char* path,
                   const unsigned char* bottomUpRgba,
                   int width,
                   int height) {
    if (path == nullptr || bottomUpRgba == nullptr ||
        width <= 0 || height <= 0) {
        return false;
    }
    const size_t stride = static_cast<size_t>(width) * 4u;

    // glReadPixels hands back rows bottom-up and PNG stores them top-down,
    // so the image is flipped here rather than saved upside down. One row
    // of scratch is enough; the whole buffer does not need a second copy.
    unsigned char* row = static_cast<unsigned char*>(std::malloc(stride));
    if (row == nullptr) {
        return false;
    }
    unsigned char* pixels = const_cast<unsigned char*>(bottomUpRgba);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top = pixels + static_cast<size_t>(y) * stride;
        unsigned char* bot =
            pixels + static_cast<size_t>(height - 1 - y) * stride;
        std::memcpy(row, top, stride);
        std::memcpy(top, bot, stride);
        std::memcpy(bot, row, stride);
    }
    std::free(row);

    return writePngRgba(path, width, height, pixels);
}

}  // namespace rr3
