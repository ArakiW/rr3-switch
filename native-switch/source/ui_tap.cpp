#include "rr3/ui_tap.hpp"

#include "rr3/guest_touch.hpp"

namespace rr3 {
namespace ui {

namespace {

// The guest's touch object ships with "a finger is down" and x = y = -1.
// That is not a real finger -- a real one has a position -- and because the
// guest's engine stores a new point only when no finger is down, that stale
// state swallows the first press of a run.  Releasing it first is what the
// touch contract asks for, and it is harmless: the guest is told that the
// pointer at (-1,-1) ended.
bool stalePointerIsDown() {
    const GuestTouchState state = readGuestTouchState();
    if (!state.valid || !state.down) {
        return false;
    }
    return state.x == -1 && state.y == -1;
}

}  // namespace

bool Tapper::press(void* jniEnvironment, void* activityObject,
                    unsigned long long frame, int x, int y, const char* phase) {
    if (s_pending) {
        return false;
    }
    if (!s_staleCleared) {
        s_staleCleared = true;
        if (stalePointerIsDown()) {
            injectGuestTouchEvent(jniEnvironment, activityObject,
                                  GuestTouchAction::Release, 0, -1, -1, false,
                                  "tap-stale-release");
        }
    }
    injectGuestTouchEvent(jniEnvironment, activityObject, GuestTouchAction::Begin,
                          0, x, y, false, phase);
    s_pending = true;
    s_x = x;
    s_y = y;
    s_pressFrame = frame;
    s_releaseFrame = frame + kTapHoldFrames;
    return true;
}

bool Tapper::service(void* jniEnvironment, void* activityObject,
                      unsigned long long frame) {
    if (!s_pending || frame < s_releaseFrame) {
        return false;
    }
    injectGuestTouchEvent(jniEnvironment, activityObject, GuestTouchAction::End,
                          0, s_x, s_y, true, "tap-end");
    s_pending = false;
    return true;
}

}  // namespace ui
}  // namespace rr3
