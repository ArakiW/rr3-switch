#pragma once
//
// The pure conversion from a digitizer frame to the guest's touch events.
//
// WHY A SEPARATE, HEADER-ONLY, LIBNX-FREE HEADER. The conversion is the part
// of the hardware touch path with the interesting behaviour -- the pointer id,
// the stale-pointer release, the coordinate mapping -- and it is the part that
// can be PROVED without a touchscreen. It is kept free of libnx types and of
// JNI so a host test can compile it unchanged and drive it with synthetic
// samples; see native-switch/tests/test_guest_touch_hw.py.
//
// THE GUEST SIDE IS AN ORACLE, NOT A GUESS. The guest's own Java
// (reverse/java/sources/com/firemint/realracing/GLView.java) turns an Android
// MotionEvent into the four JNI calls:
//
//   getActionMasked() == 0 or 5   -> onTouchBeginJNI(id, x, y)
//   getActionMasked() == 2        -> onTouchMoveJNI(id, x, y)
//   getActionMasked() == 1 or 6   -> onTouchEndJNI(id, x, y, last)
//   getActionMasked() == 3        -> onTouchCancelJNI()
//
// with last == (getActionMasked() == 1), i.e. last is true only for the last
// pointer's ACTION_UP. The id is motionEvent.getPointerId(actionIndex): a
// small per-gesture integer.
//
// The guest's engine keeps ONE pointer id (the touch object's +0x14) and
// MATCHES it on move and end, so a gesture must use one id from the down to
// the up. It also stores a new point ONLY when no finger is down (the engine's
// touch handler, ELF 0x12b4624: ldrb w8, [x20, #0x20] ; cbz w8, <store>), and
// the guest's touch object SHIPS with down=1 and x=y=-1. A press that arrives
// while the guest still believes a finger is down is therefore swallowed, and
// the stale pointer has to be released first.
//
// THE POINTER ID. The guest never requires a particular value, only that the
// down, the moves and the up agree. The digitizer's own finger id is used
// unchanged: it is stable for the life of a finger, it is 0 for the first
// finger -- which is exactly the id Android's MotionEvent gives the first
// pointer, so the single-finger case is bit-identical to what the guest sees
// on a real phone -- and two fingers get two distinct ids the way Android
// does. The guest's engine is single-pointer either way: it stores one id and
// refuses a second press, so the port drives ONE finger and ignores the rest
// until the driven finger lifts.

namespace rr3 {

// One finger as the digitizer reported it, in the digitizer's own pixels.
struct TouchSample {
    int rawX;
    int rawY;
    unsigned int finger;  // the digitizer's finger id (0..15)
    bool start;           // the finger landed on this frame
    bool end;             // the finger lifted on this frame
};

// The guest's own touch state, read back from the guest's touch object.
struct TouchGuestState {
    bool down;  // the guest believes a finger is down
    int pointerId;
    int x;
    int y;
};

// What the guest has to be told.
enum class TouchAction {
    Begin,    // onTouchBeginJNI(id, x, y)
    Move,     // onTouchMoveJNI(id, x, y)
    End,      // onTouchEndJNI(id, x, y, last=true)
    Release,  // onTouchEndJNI(id, x, y, last=false): let go of a stale pointer
};

struct TouchEvent {
    TouchAction action;
    int pointerId;
    int x;
    int y;
    bool last;  // onTouchEndJNI's "last" flag
};

// The port's view of the finger it is currently driving.
struct TouchTracker {
    bool down;
    int pointerId;
    int x;
    int y;
    unsigned int finger;
};

// The coordinate mapping.
//
// The digitizer's native size and the guest's view size are BOTH named here
// so the 1:1 assumption is a value a test can change rather than a constant
// buried in the arithmetic. On a Switch the digitizer and the screen are both
// 1280x720 and the guest's view is 1280x720 (the port reports it through
// onViewChangedJNI(env, self, 1280, 720, 0, 0)), so the mapping is the
// identity -- but it is the identity BY CONSTRUCTION here, not by luck.
struct TouchMapping {
    int sourceWidth;   // the digitizer's native width
    int sourceHeight;  // the digitizer's native height
    int viewWidth;     // the guest's view width
    int viewHeight;    // the guest's view height
    // The rectangle of the DIGITIZER that actually shows the guest's view.
    //
    // The present path fits the view into the output and leaves bars when the
    // two aspect ratios differ, so the digitizer's pixels are not all the
    // guest's screen.  A tap on a bar is a tap on nothing, and a tap that
    // lands inside the rectangle has to land on the button the player sees.
    //
    // A zero extent means "the whole digitizer", which is the identity and
    // the historical behaviour.
    int sourceX = 0;
    int sourceY = 0;
    int sourceRectWidth = 0;
    int sourceRectHeight = 0;
};

// The rectangle of the digitizer that shows the guest's view, with the
// "whole digitizer" default applied.
inline int rr3TouchRectX(const TouchMapping& mapping) {
    return mapping.sourceX;
}

inline int rr3TouchRectY(const TouchMapping& mapping) {
    return mapping.sourceY;
}

inline int rr3TouchRectWidth(const TouchMapping& mapping) {
    return mapping.sourceRectWidth > 0 ? mapping.sourceRectWidth
                                       : mapping.sourceWidth;
}

inline int rr3TouchRectHeight(const TouchMapping& mapping) {
    return mapping.sourceRectHeight > 0 ? mapping.sourceRectHeight
                                        : mapping.sourceHeight;
}

// The maximum number of events one frame can produce: an End for the finger
// that lifted, a Release for the guest's stale pointer, a Begin for the next
// finger and an End if that finger lifted in the same frame.
constexpr int kTouchEventCapacity = 8;

// One digitizer pixel -> one view pixel, scaled and clamped.
//
// Integer arithmetic on purpose: the guest truncates the float coordinates to
// int with FCVTZS, so the guest has always seen integers and a rounding
// difference here would be a difference the guest cannot represent.
inline int rr3TouchMapAxis(int raw, int source, int view) {
    if (source <= 0 || view <= 0) {
        return 0;
    }
    long long scaled =
        (static_cast<long long>(raw) * static_cast<long long>(view)) /
        static_cast<long long>(source);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > static_cast<long long>(view) - 1) {
        scaled = static_cast<long long>(view) - 1;
    }
    return static_cast<int>(scaled);
}

// One digitizer pixel -> one view pixel, through the rectangle that shows it.
//
// The origin is subtracted first, so a digitizer point outside the rectangle
// maps to the edge of the view rather than to a wildly wrong place.
inline int rr3TouchMapAxisRect(int raw, int origin, int extent, int view) {
    if (extent <= 0 || view <= 0) {
        return 0;
    }
    long long scaled =
        (static_cast<long long>(raw - origin) * static_cast<long long>(view)) /
        static_cast<long long>(extent);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > static_cast<long long>(view) - 1) {
        scaled = static_cast<long long>(view) - 1;
    }
    return static_cast<int>(scaled);
}

// True when the digitizer point can reach the guest's view.
//
// Two different situations, two different answers:
//
//   * A point INSIDE the digitizer but OUTSIDE the rectangle that shows the
//     view is on a letterbox bar.  It is on the screen, but it is not on the
//     guest's screen, and it must not press anything.
//   * A point OUTSIDE the digitizer itself is the "finger dragged off the
//     physical screen" case.  That keeps the historical answer: it is
//     CLAMPED into the view, not dropped.
inline bool rr3TouchReachesView(const TouchMapping& mapping, int rawX,
                                int rawY) {
    const int width = rr3TouchRectWidth(mapping);
    const int height = rr3TouchRectHeight(mapping);
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (rawX < 0 || rawY < 0 ||
        rawX >= mapping.sourceWidth || rawY >= mapping.sourceHeight) {
        return true;
    }
    return rawX >= rr3TouchRectX(mapping) &&
           rawX < rr3TouchRectX(mapping) + width &&
           rawY >= rr3TouchRectY(mapping) &&
           rawY < rr3TouchRectY(mapping) + height;
}

// Converts one frame of digitizer samples into the guest events it implies.
// Returns the number of events written to `out`, which is at most
// outCapacity (events past the capacity are DROPPED, the tracker is still
// advanced -- an overflow means the caller sized the buffer wrong).
//
// The rules, in order:
//
//   1. The finger the tracker is driving is released (End, last=true) when the
//      digitizer says it is gone -- either because the sample carries the End
//      attribute, or because the finger vanished between frames. A lift that
//      is never delivered leaves the guest holding a pressed button forever.
//   2. A new finger is pressed (Begin) only while no finger is being driven.
//   3. Before that press, the guest's OWN stale pointer is released FIRST
//      (End, last=false) when the guest still believes a finger is down. This
//      is the trap the guest's shipped down=1 sets: without the release the
//      engine's "store only when no finger is down" branch drops the press.
//   4. While `suspend` is true the plan emits the release of the finger this
//      port holds and nothing else. `suspend` is set when something else owns
//      the guest's touch path -- the file control's script, or the guest being
//      paused or unfocused -- so the hardware path never presses into a
//      gesture that is not its own.
//
// `guest` is the guest's own state, read back from the guest's touch object;
// it is an INPUT, not something this function guesses.
inline int rr3PlanTouchFrame(const TouchSample* samples, int sampleCount,
                             TouchTracker* tracker,
                             const TouchGuestState& guest,
                             const TouchMapping& mapping, bool suspend,
                             TouchEvent* out, int outCapacity) {
    if (tracker == nullptr || out == nullptr || outCapacity <= 0) {
        return 0;
    }
    int produced = 0;
    auto emit = [&](TouchAction action, int pointerId, int x, int y, bool last) {
        if (produced < outCapacity) {
            out[produced].action = action;
            out[produced].pointerId = pointerId;
            out[produced].x = x;
            out[produced].y = y;
            out[produced].last = last;
            ++produced;
        }
    };
    const auto mapX = [&](int raw) {
        return rr3TouchMapAxisRect(raw, rr3TouchRectX(mapping),
                                   rr3TouchRectWidth(mapping),
                                   mapping.viewWidth);
    };
    const auto mapY = [&](int raw) {
        return rr3TouchMapAxisRect(raw, rr3TouchRectY(mapping),
                                   rr3TouchRectHeight(mapping),
                                   mapping.viewHeight);
    };

    // The sample of the finger being driven, if the digitizer still reports it.
    const TouchSample* tracked = nullptr;
    if (tracker->down && samples != nullptr) {
        for (int i = 0; i < sampleCount; ++i) {
            if (samples[i].finger == tracker->finger) {
                tracked = &samples[i];
                break;
            }
        }
    }

    // The pointer this frame's End already let go of, so the stale-pointer
    // release below does not release the same pointer twice.
    int releasedPointerId = -1;

    if (suspend) {
        // Something else owns the guest's touch path. The only thing the
        // hardware path may still do is let go of the finger it holds, so the
        // guest is never left with two pointers down.
        if (tracker->down) {
            emit(TouchAction::End, tracker->pointerId, tracker->x, tracker->y,
                 true);
            releasedPointerId = tracker->pointerId;
            tracker->down = false;
        }
        return produced;
    }

    if (tracker->down) {
        if (tracked == nullptr) {
            // The finger vanished between frames without an End sample.
            emit(TouchAction::End, tracker->pointerId, tracker->x, tracker->y,
                 true);
            releasedPointerId = tracker->pointerId;
            tracker->down = false;
        } else if (tracked->end) {
            const int vx = mapX(tracked->rawX);
            const int vy = mapY(tracked->rawY);
            emit(TouchAction::End, tracker->pointerId, vx, vy, true);
            releasedPointerId = tracker->pointerId;
            tracker->down = false;
        } else if (!rr3TouchReachesView(mapping, tracked->rawX, tracked->rawY)) {
            // The finger left the area that shows the guest's view -- it slid
            // onto a letterbox bar.  The guest must not keep a button held
            // for a finger that is no longer on its screen.
            emit(TouchAction::End, tracker->pointerId, tracker->x, tracker->y,
                 true);
            releasedPointerId = tracker->pointerId;
            tracker->down = false;
        } else {
            const int vx = mapX(tracked->rawX);
            const int vy = mapY(tracked->rawY);
            if (vx != tracker->x || vy != tracker->y) {
                emit(TouchAction::Move, tracker->pointerId, vx, vy, false);
                tracker->x = vx;
                tracker->y = vy;
            }
        }
    }

    if (!tracker->down && samples != nullptr && sampleCount > 0) {
        // The finger to press: the first one that is not lifting. The guest's
        // engine is single-pointer, so exactly one finger is driven and the
        // others wait.
        const TouchSample* pick = nullptr;
        for (int i = 0; i < sampleCount; ++i) {
            if (!samples[i].end &&
                rr3TouchReachesView(mapping, samples[i].rawX, samples[i].rawY)) {
                pick = &samples[i];
                break;
            }
        }
        if (pick == nullptr) {
            // Every reported finger is lifting this frame. A finger that
            // LANDED and lifted inside one frame still gets a press and a
            // release, or a tap would be invisible to the guest. A finger
            // that was already down and is lifting now was ALREADY pressed,
            // and pressing it again would be a second press of a finger the
            // guest has just been told is gone.
            for (int i = 0; i < sampleCount; ++i) {
                if (samples[i].start &&
                    rr3TouchReachesView(mapping, samples[i].rawX, samples[i].rawY)) {
                    pick = &samples[i];
                    break;
                }
            }
        }
        // No candidate means there is nothing new to press: every finger the
        // digitizer reported was already down and is now lifting.
        if (pick == nullptr) {
            return produced;
        }
        const int vx = mapX(pick->rawX);
        const int vy = mapY(pick->rawY);
        // THE STALE POINTER GOES FIRST.
        if (guest.down &&
            !(releasedPointerId >= 0 && guest.pointerId == releasedPointerId)) {
            emit(TouchAction::Release, guest.pointerId, guest.x, guest.y, false);
        }
        emit(TouchAction::Begin, static_cast<int>(pick->finger), vx, vy, false);
        tracker->down = true;
        tracker->pointerId = static_cast<int>(pick->finger);
        tracker->finger = pick->finger;
        tracker->x = vx;
        tracker->y = vy;
        if (pick->end) {
            emit(TouchAction::End, tracker->pointerId, vx, vy, true);
            tracker->down = false;
        }
    }

    return produced;
}

}  // namespace rr3
