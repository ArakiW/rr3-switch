#include "rr3/ui_target.hpp"

#include "rr3/glove_detector.hpp"

#include <cstdlib>
#include <vector>

namespace rr3 {
namespace ui {

namespace {

// One accent colour band, as comparisons on the three channels.
struct Band {
    int redMin;
    int greenMin;
    int greenMax;
    int blueMax;
};

constexpr Band kOrangeBand{kOrangeRedMin, kOrangeGreenMin, kOrangeGreenMax,
                            kOrangeBlueMax};
constexpr Band kYellowBand{kYellowRedMin, kYellowGreenMin, 255,
                            kYellowBlueMax};

inline bool inBand(const unsigned char* p, const Band& band) {
    const int r = p[0];
    const int g = p[1];
    const int b = p[2];
    return r >= band.redMin && g >= band.greenMin && g <= band.greenMax &&
           b <= band.blueMax;
}

// A run of accent-coloured pixels.
struct Blob {
    int x0{0};
    int y0{0};
    int x1{0};
    int y1{0};
    int area{0};
    long long sumX{0};
    long long sumY{0};
};

// How close two accent runs have to be to count as ONE control.
//
// A button's label is a row of glyphs with a few pixels of background
// between them, and the calendar's "COLLECT" word is nothing but glyphs:
// measured 2026-10-27 at x 373..533, y 202..242, which is a WORD, not a
// slab.  So the runs have to be merged before they can be called a control.
constexpr int kMergeGap = 8;

// The smallest run worth looking at.  Below this it is dust.
constexpr int kMinRunPixels = 16;

// Every accent run in one band, before merging.
void collectRuns(const unsigned char* bottomUpRgba, int width, int height,
                 const Band& band, std::vector<Blob>* runs) {
    const size_t total = static_cast<size_t>(width) * height;
    unsigned char* mask = static_cast<unsigned char*>(std::malloc(total));
    if (mask == nullptr) {
        return;
    }
    for (size_t i = 0; i < total; ++i) {
        mask[i] = inBand(bottomUpRgba + i * 4, band) ? 1 : 0;
    }

    // The flood fill uses a heap stack that grows: a big accent area is
    // thousands of pixels and a fixed stack would silently truncate it.
    std::vector<int> stack;
    for (size_t i = 0; i < total; ++i) {
        if (!mask[i]) {
            continue;
        }
        stack.clear();
        stack.push_back(static_cast<int>(i));
        mask[i] = 0;  // reused as the visited mark
        Blob blob;
        blob.x0 = blob.x1 = static_cast<int>(i) % width;
        blob.y0 = blob.y1 = static_cast<int>(i) / width;
        while (!stack.empty()) {
            const int j = stack.back();
            stack.pop_back();
            const int jx = j % width;
            const int jy = j / width;
            ++blob.area;
            blob.sumX += jx;
            blob.sumY += jy;
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
                    const size_t k = static_cast<size_t>(ny) * width + nx;
                    if (mask[k]) {
                        mask[k] = 0;
                        stack.push_back(static_cast<int>(k));
                    }
                }
            }
        }
        if (blob.area >= kMinRunPixels) {
            runs->push_back(blob);
        }
    }

    std::free(mask);
}

// Merges runs whose boxes are within kMergeGap of each other.
std::vector<Blob> mergeRuns(const std::vector<Blob>& runs) {
    std::vector<Blob> groups = runs;
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < groups.size() && !merged; ++i) {
            for (size_t j = i + 1; j < groups.size(); ++j) {
                const Blob& a = groups[i];
                const Blob& b = groups[j];
                const int gapX = (a.x0 > b.x1) ? (a.x0 - b.x1 - 1)
                                               : ((b.x0 > a.x1) ? (b.x0 - a.x1 - 1) : 0);
                const int gapY = (a.y0 > b.y1) ? (a.y0 - b.y1 - 1)
                                               : ((b.y0 > a.y1) ? (b.y0 - a.y1 - 1) : 0);
                if (gapX > kMergeGap || gapY > kMergeGap) {
                    continue;
                }
                Blob m;
                m.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
                m.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
                m.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
                m.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
                m.area = a.area + b.area;
                m.sumX = a.sumX + b.sumX;
                m.sumY = a.sumY + b.sumY;
                groups[i] = m;
                groups.erase(groups.begin() + static_cast<long>(j));
                merged = true;
                break;
            }
        }
    }
    return groups;
}

// The biggest button-sized accent group in one band.
bool largestButton(const unsigned char* bottomUpRgba, int width, int height,
                   const Band& band, Blob* best) {
    std::vector<Blob> runs;
    collectRuns(bottomUpRgba, width, height, band, &runs);
    if (runs.empty()) {
        return false;
    }
    const std::vector<Blob> groups = mergeRuns(runs);

    bool have = false;
    for (size_t i = 0; i < groups.size(); ++i) {
        const Blob& g = groups[i];
        const int bw = g.x1 - g.x0 + 1;
        const int bh = g.y1 - g.y0 + 1;
        if (g.area < kMinButtonPixels) {
            continue;
        }
        if (bw < kMinButtonWidth || bw > kMaxButtonWidth) {
            continue;
        }
        if (bh < kMinButtonHeight || bh > kMaxButtonHeight) {
            continue;
        }
        if (!have || g.area > best->area) {
            *best = g;
            have = true;
        }
    }
    return have;
}

void addButton(TargetList* list, const Blob& blob, TargetKind kind, int height) {
    if (list->count >=
        static_cast<int>(sizeof(list->items) / sizeof(list->items[0]))) {
        return;
    }
    Target& t = list->items[list->count++];
    t.kind = kind;
    // The press point is the CENTROID of the accent pixels, not the middle
    // of the bounding box: a label's glyphs leave the box's middle empty
    // between the characters, and the box's middle can be outside a skewed
    // button entirely.
    //
    // The buffer's FIRST row is the BOTTOM of the image, and the guest's
    // input is in top-down view pixels.
    const int cx = static_cast<int>(blob.sumX / blob.area);
    const int cy = static_cast<int>(blob.sumY / blob.area);
    t.x = cx;
    t.y = height - 1 - cy;
    t.width = blob.x1 - blob.x0 + 1;
    t.height = blob.y1 - blob.y0 + 1;
    t.area = blob.area;
}

}  // namespace

TargetList findTargets(const unsigned char* bottomUpRgba,
                        const unsigned char* previous,
                        int width,
                        int height,
                        int* changedTotal) {
    TargetList list;
    if (bottomUpRgba == nullptr || width <= 0 || height <= 0) {
        return list;
    }

    // The glove first: it is the guest's own "press here".
    const glove::Detection det =
        glove::find(bottomUpRgba, previous, width, height, changedTotal);
    if (det.valid &&
        list.count <
            static_cast<int>(sizeof(list.items) / sizeof(list.items[0]))) {
        Target& t = list.items[list.count++];
        t.kind = TargetKind::Glove;
        t.x = det.x;
        t.y = det.y;
        t.width = 0;
        t.height = 0;
        t.area = det.redPixels;
    }

    // Then the accent-coloured controls: the guest's own convention for a
    // primary action.  The LARGEST one first, because the way forward is
    // the biggest thing the guest is offering.
    Blob orange{};
    if (largestButton(bottomUpRgba, width, height, kOrangeBand, &orange)) {
        addButton(&list, orange, TargetKind::OrangeButton, height);
    }
    Blob yellow{};
    if (largestButton(bottomUpRgba, width, height, kYellowBand, &yellow)) {
        addButton(&list, yellow, TargetKind::YellowButton, height);
    }

    return list;
}

}  // namespace ui
}  // namespace rr3
