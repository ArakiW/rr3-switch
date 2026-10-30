#pragma once
//
// FIND THE GUEST'S OWN RED GLOVE, AND CLICK IT.
//
// WHY THIS EXISTS.
//
// The touch script used to name PIXEL COORDINATES, and the guest moved
// them.  MEASURED:
//
//   2026-10-27  the daily-rewards calendar's orange COLLECT word sat at
//               x 373..533, y 202..242, and the script clicked (453, 222)
//   2026-10-29  the same word sat at x 565..725, y 202..242, so (453, 222)
//               was 112 pixels of blank background to the LEFT of the
//               button, and the guest sat in that modal for the whole run
//   2026-10-30  the same word sat at x 720..940 and pointed at the FOURTH
//               day circle instead of the third
//
// The calendar lays its word out over the middle column of a day grid
// whose width depends on the month, so the coordinate moves with the date.
// A hard-coded point cannot survive that: it worked on 2026-10-28 (the
// reward had already been collected, so no calendar appeared at all) and
// broke on every day after.
//
// THE GUEST ALREADY SAYS WHERE TO PRESS.  It draws a RED GLOVE CURSOR on
// the control it wants pressed -- the very hand that says "hold the brake
// to reverse" points at the brake.  So the coordinate does not have to be
// known: it can be READ, every frame, from the picture the guest just
// drew.
//
// WHY HERE AND NOT ON THE HOST.  tools/follow_glove.py already does this
// on the host, by differencing two framebuffer captures the port writes to
// the card.  That works, but it is a round trip through the card and it
// only sees the frames the capture schedule happens to produce.  The port
// already has the pixels in hand -- it reads them back for the PNG capture
// -- so the glove can be found there, on the frame-loop thread, on the
// frame the guest just drew.
//
// A COLOUR MASK ALONE CANNOT FIND THE GLOVE: the guest's cars are red
// too.  What separates the glove is that it ANIMATES, so it is the thing
// that MOVES between two frames of the same screen.  So the detector
// differences two consecutive readbacks and looks for a saturated red
// cluster that CHANGED.
//
// The mask and the threshold come from tools/follow_glove.py, which was
// calibrated on real captures; the difference test is the same one that
// tool uses.
//
namespace rr3 {
namespace glove {

// What one detection found.
struct Detection {
    bool valid{false};
    int x{0};          // the glove's centre, in VIEW pixels
    int y{0};
    int redPixels{0};   // how many saturated-red pixels the cluster had
    int changed{0};     // how many of them moved since the previous frame
};

// Finds the glove in a frame.
//
// `bottomUpRgba` is the buffer glReadPixels returned: the FIRST row is the
// BOTTOM row of the image, and the glove's coordinates are reported in
// VIEW pixels (top-down), because that is the coordinate space the touch
// file and the guest's own input use.
//
// `previous` is the previous frame's buffer in the same layout, or null on
// the first call.  Without it there is no way to tell the glove from a red
// car, and the detector reports nothing rather than a guess.
//
// `previousChangedPixels` counts the pixels that differ between the two
// frames; it is returned so the caller can tell "the screen changed" from
// "the glove moved", and skip the detection on a screen transition.
Detection find(const unsigned char* bottomUpRgba,
               const unsigned char* previous,
               int width,
               int height,
               int* previousChangedPixels);

// The glove's colours, as calibrated by tools/follow_glove.py:
// a saturated red with white stripes next to it.
constexpr int kRedMin = 140;
constexpr int kRedSpread = 70;

// A GLOVE IS NOT A WARM ORANGE.
//
// MEASURED 2026-10-30 (q29): the mask alone also takes the guest's own
// orange back button, because an orange pixel (240, 170, 30) sits exactly
// on the mask's edge (g = 170 = r - 70).  Pressing it sent the guest BACK
// to the main menu, and the run oscillated between two screens for ever.
//
//     the false positive   g - b = 93 .. 147 (mean 139)   -- warm
//     the real glove       g - b = -31         (mean)       -- cool
//
// "g <= b + kRedWarmLimit" keeps 1071 of 1071 real glove pixels and
// rejects 943 of 943 false ones.
constexpr int kRedWarmLimit = 40;

// The glove is HAND-SIZED.  MEASURED 2026-10-30 over the q21 archive:
// the glove's bounding box is 44x41, 48x40 and 83x51 pixels, while the
// red car's is 227x172, 219x103 and 208x90.  The bound is on the width
// and the height separately, because a car seen head-on is narrow.
constexpr int kMaxGloveWidth = 110;
constexpr int kMaxGloveHeight = 110;

}  // namespace glove
}  // namespace rr3
