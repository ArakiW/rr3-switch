#pragma once
//
// FIND THE CONTROL THE GUEST WANTS PRESSED -- AND FIND THE BUTTONS TOO.
//
// WHY THIS EXISTS
// ---------------
// The port's first UI driver found the guest's RED GLOVE and pressed it.
// The glove is a good signal -- the guest draws it on the control it wants
// pressed -- but it is NOT the only signal, and trusting it alone is what
// left the guest sitting on the main menu for 1500 seconds.
//
// MEASURED 2026-10-30, on the q25 archive's last frame (1280x720):
//
//   the red glove sat on the season-promo CAR IMAGE at (465, 355) -- a
//   decoration, not a control, and pressing it changed nothing;
//   the actual entry point was the ORANGE button at x 1090..1223,
//   y 611..641 whose label is the guest's own "ENTER THE RACE" -- and the
//   guest does NOT draw a glove on it.
//
// So the glove answers "which control does the guest point at", and the
// accent-coloured button answers "which control is the way forward", and
// the port needs BOTH.  The guest's primary actions are drawn in a saturated
// accent colour (orange for "COLLECT" and "ENTER THE RACE", yellow for the
// "CONTINUE" button), which is a convention the guest keeps across screens.
//
// THE FEEDBACK LOOP IS WHAT MAKES THIS SAFE.  Every target is a CANDIDATE,
// not a decision: the port presses one, watches whether the screen actually
// changed, and moves on to the next candidate when it did not.  That is why a
// decorative orange tab costs one wasted press instead of a wasted run.
//
// THE COLOUR BANDS
// ----------------
// The orange band is the one the project already measured (r>230,
// 150<g<215, b<60) off the daily-rewards calendar's COLLECT word and the
// main menu's ENTER THE RACE label.  The yellow band (r>225, g>215, b<90)
// is the one measured off the track-load panel's CONTINUE button.
//
// A COLOUR MASK ALONE IS NOT A BUTTON.  Every candidate also has to be
// button-SIZED (wide enough to hold a label, not a single glyph) and
// SOLID (a label's glyphs are separate blobs with gaps between them, and a
// solid slab of accent colour is what a button is).
// ---------------------------------------------------------------------------
namespace rr3 {
namespace ui {

// What a candidate is.
enum class TargetKind {
    Glove,          // the guest's own red hint hand
    OrangeButton,   // a saturated orange control
    YellowButton,   // a saturated yellow control
};

// One candidate control to press.
struct Target {
    TargetKind kind{TargetKind::Glove};
    int x{0};          // the press point, in VIEW pixels
    int y{0};
    int width{0};      // the bounding box, for the log
    int height{0};
    int area{0};       // how many pixels the control's colour covered
};

// The candidates, best first: the glove, then the buttons by size.
struct TargetList {
    int count{0};
    Target items[8];
};

// Finds the candidates in a frame.
//
// `bottomUpRgba` is the buffer glReadPixels returned: the FIRST row is the
// BOTTOM row of the image.  The coordinates reported are in VIEW pixels
// (top-down), because that is the space the guest's input uses.
//
// `previous` is the previous frame's buffer in the same layout, or null.
// The glove cannot be told from a red car without it.
//
// `changedTotal` receives the number of pixels that differ between the two
// frames, so the caller can tell "the screen changed" from "the glove moved".
TargetList findTargets(const unsigned char* bottomUpRgba,
                        const unsigned char* previous,
                        int width,
                        int height,
                        int* changedTotal);

// The accent colour bands, as measured.
constexpr int kOrangeRedMin = 230;
constexpr int kOrangeGreenMin = 150;
constexpr int kOrangeGreenMax = 215;
constexpr int kOrangeBlueMax = 60;

constexpr int kYellowRedMin = 225;
constexpr int kYellowGreenMin = 215;
constexpr int kYellowBlueMax = 90;

// A button has to be big enough to hold a label and small enough to be a
// control rather than a picture.
constexpr int kMinButtonPixels = 200;
constexpr int kMinButtonWidth = 60;
constexpr int kMaxButtonWidth = 600;
constexpr int kMinButtonHeight = 16;
constexpr int kMaxButtonHeight = 160;
// A button's colour is a solid slab; a label's glyphs are separate blobs.
constexpr int kMinButtonFillPercent = 35;

}  // namespace ui
}  // namespace rr3
