#pragma once
//
// A TAP IS A TAP: DOWN, THEN UP, IN A LATER FRAME.
//
// WHY THIS EXISTS
// ---------------
// The port's first UI driver injected onTouchBeginJNI and onTouchEndJNI in
// the SAME frame, because that is what "press a button" looks like in code.
// The guest's UI does not agree.
//
// MEASURED 2026-10-30, over EVERY archive in .build/eden-runs (28 runs that
// injected touches at all):
//
//   q21  24 taps, all with a 40-frame down->up gap   -> reached a race
//   q25 144 taps, all with a     0-frame down->up gap -> never advanced
//
// The separation is PERFECT: every archive that reached a race has
// same-frame = 0, and q25 -- the only run with a same-frame tap -- is the
// only one that never moved.  The down->up gaps that appear in the archives
// are 1, 14..18, 20, 30, 36 and 40 frames; the ones that reach a race are
// 1, 20 and 40.
//
// So the guest needs the finger to be DOWN while it processes the press: the
// button's "pressed" state has to exist for at least one of the guest's own
// frames before the finger comes up.  A zero-duration tap never exists as a
// state, and the guest drops it.
//
// 40 FRAMES is the value every successful run uses, and it is counted in
// FRAMES, not seconds, so it means the same thing on the emulator and on
// real hardware: the guest's UI is updated once per frame, so a frame-count
// gap is a frame-count gap either way.
//
// THE STALE POINTER
// -----------------
// The guest's touch object SHIPS with "a finger is down" set and with
// x = y = -1, and its engine stores a new point only when no finger is down.
// So the FIRST press of a run is swallowed: q25's first press left the
// guest's own point at (-1,-1), while q21's first press (900 frames in, by
// which time the guest had settled) landed.  The fix is the one the touch
// contract already describes: read the guest's state before pressing, and
// release that stale pointer first.
//
// THIS IS A SHARED PRIMITIVE.  Every path that presses a control -- the
// glove, the orange button, the coordinate script -- goes through it, so a
// second path cannot grow its own idea of what a tap is.
// ---------------------------------------------------------------------------
namespace rr3 {
namespace ui {

// How long the finger stays down, in FRAMES.
//
// MEASURED: 40 frames is the gap every archive that reached a race used.
constexpr unsigned kTapHoldFrames = 40u;

// The port's tap state machine.
//
// Safe to call every frame: press() refuses to start a second tap while one
// is in flight, and service() releases the pending tap when its hold is over.
class Tapper {
public:
    // Releases a pending tap once its hold is over.
    // Returns true when it injected the release.
    bool service(void* jniEnvironment, void* activityObject,
                 unsigned long long frame);

    // Starts a tap at (x, y) in VIEW pixels.
    // Returns false when a tap is already in flight.
    bool press(void* jniEnvironment, void* activityObject,
               unsigned long long frame, int x, int y, const char* phase);

    bool pending() const { return s_pending; }
    int pendingX() const { return s_x; }
    int pendingY() const { return s_y; }
    // The frame the pending tap will be released on.
    unsigned long long releaseFrame() const { return s_releaseFrame; }

private:
    bool s_pending{false};
    int s_x{0};
    int s_y{0};
    unsigned long long s_pressFrame{0};
    unsigned long long s_releaseFrame{0};
    // Whether the guest's shipped "a finger is down at (-1,-1)" has been
    // released already.
    bool s_staleCleared{false};
};

}  // namespace ui
}  // namespace rr3
