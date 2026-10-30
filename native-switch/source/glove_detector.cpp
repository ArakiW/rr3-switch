#include "rr3/glove_detector.hpp"

#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace glove {

namespace {

// One red blob, found by a flood fill over the mask.
struct Blob {
    int x0{0};
    int y0{0};
    int x1{0};
    int y1{0};
    int area{0};
    int changed{0};
};

// The mask: a saturated red, the glove's own colour band.  A pixel is in
// the mask when its red channel is at least kRedMin and its green and
// blue channels are at most kRedMin - kRedSpread below it, which is what
// tools/follow_glove.py uses and what its positive control (the archived
// frames) confirmed.
inline bool isGloveRed(const unsigned char* p) {
    const int r = p[0];
    const int g = p[1];
    const int b = p[2];
    return r >= kRedMin && g <= r - kRedSpread && b <= r - kRedSpread;
}

// The blob has to be big enough to be the glove and not noise, and it has
// to have MOVED: the glove animates, a red car does not.
constexpr int kMinRedPixels = 150;
// How many of the blob's pixels have to differ from the previous frame
// for the blob to count as the glove.  A moving hand changes a lot of its
// own pixels; a car that is merely on screen changes none of them.
constexpr int kMinChangedPixels = 40;

// AND IT HAS TO BE HAND-SIZED.
//
// MEASURED 2026-10-30 over the q21 archive's captured frames: the
// guest's glove on the calendar's COLLECT word occupies a bounding box of
// 44x41, 48x40 and 83x51 pixels, while the RED CAR on the race screen
// occupies 227x172, 219x103 and 208x90.  The two are not separable by
// area (a car seen from the front is small) and not by shape (the fill
// ratios overlap: 0.3..0.7 for both).  They ARE separable by SIZE: a
// hand never exceeds about 90 pixels across, and a car always does.
//
// Without this bound the detector fires on 6 of the 16 race-screen frame
// pairs in that archive, and a click on a red car at 46 km/h throws the
// car off the track -- the same failure the port already has once per
// race.  The test that found it is
// native-switch/tests/test_glove_detector.py.
//
// The bound is on the WIDTH AND THE HEIGHT separately, not on the area:
// a car seen head-on is narrow but tall, and a hand is small in both.
// kMaxGloveWidth / kMaxGloveHeight live in the header, so the
// test and the implementation cannot drift apart.

}  // namespace

Detection find(const unsigned char* bottomUpRgba,
               const unsigned char* previous,
               int width,
               int height,
               int* previousChangedPixels) {
    Detection result;
    if (bottomUpRgba == nullptr || width <= 0 || height <= 0) {
        return result;
    }

    const int total = width * height;

    // How much of the screen changed at all.  A screen transition changes
    // a lot; a still screen with an animating glove changes a little.
    int changedTotal = 0;
    if (previous != nullptr) {
        for (int i = 0; i < total; ++i) {
            const unsigned char* a = bottomUpRgba + i * 4;
            const unsigned char* b = previous + i * 4;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
                ++changedTotal;
            }
        }
    }
    if (previousChangedPixels != nullptr) {
        *previousChangedPixels = changedTotal;
    }
    if (previous == nullptr) {
        return result;
    }

    // The mask, and the per-pixel "did it change" flag.
    unsigned char* mask = static_cast<unsigned char*>(std::malloc(total));
    if (mask == nullptr) {
        return result;
    }
    unsigned char* moved = static_cast<unsigned char*>(std::malloc(total));
    if (moved == nullptr) {
        std::free(mask);
        return result;
    }
    for (int i = 0; i < total; ++i) {
        const unsigned char* a = bottomUpRgba + i * 4;
        const unsigned char* b = previous + i * 4;
        const bool red = isGloveRed(a);
        mask[i] = red ? 1 : 0;
        moved[i] = (red && (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])) ? 1 : 0;
    }

    // The largest red blob with enough moved pixels in it.
    Blob best;
    bool haveBest = false;
    unsigned char* seen = static_cast<unsigned char*>(std::calloc(total, 1));
    if (seen != nullptr) {
        for (int i = 0; i < total; ++i) {
            if (!mask[i] || seen[i]) {
                continue;
            }
            // A flood fill over the mask.
            int* stack = static_cast<int*>(std::malloc(sizeof(int) * 4096));
            if (stack == nullptr) {
                break;
            }
            int top = 0;
            stack[top++] = i;
            seen[i] = 1;
            Blob blob;
            blob.x0 = blob.x1 = i % width;
            blob.y0 = blob.y1 = i / width;
            while (top > 0) {
                const int j = stack[--top];
                const int jx = j % width;
                const int jy = j / width;
                ++blob.area;
                if (moved[j]) {
                    ++blob.changed;
                }
                if (jx < blob.x0) { blob.x0 = jx; }
                if (jx > blob.x1) { blob.x1 = jx; }
                if (jy < blob.y0) { blob.y0 = jy; }
                if (jy > blob.y1) { blob.y1 = jy; }
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = jx + dx;
                        const int ny = jy + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const int k = ny * width + nx;
                        if (mask[k] && !seen[k]) {
                            seen[k] = 1;
                            if (top < 4096) {
                                stack[top++] = k;
                            }
                        }
                    }
                }
            }
            std::free(stack);
            const int blobWidth = blob.x1 - blob.x0 + 1;
            const int blobHeight = blob.y1 - blob.y0 + 1;
            if (blob.area >= kMinRedPixels && blob.changed >= kMinChangedPixels &&
                blobWidth <= kMaxGloveWidth && blobHeight <= kMaxGloveHeight) {
                if (!haveBest || blob.area > best.area) {
                    best = blob;
                    haveBest = true;
                }
            }
        }
        std::free(seen);
    }
    std::free(mask);
    std::free(moved);

    if (!haveBest) {
        return result;
    }
    result.valid = true;
    result.redPixels = best.area;
    result.changed = best.changed;
    // The mask is in the buffer's order: the FIRST row is the BOTTOM of
    // the image.  The touch file and the guest's input use top-down view
    // pixels, so the row has to be flipped here.
    const int centreX = (best.x0 + best.x1) / 2;
    const int centreY = (best.y0 + best.y1) / 2;
    result.x = centreX;
    result.y = height - 1 - centreY;
    return result;
}

}  // namespace glove
}  // namespace rr3
