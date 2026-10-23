// A HOST-SIDE test for the pure touch conversion.
//
// WHY A SEPARATE PROGRAM. The interesting part of the hardware touch path --
// the pointer id, the stale-pointer release, the coordinate mapping -- is pure
// arithmetic on a handful of samples, and pure arithmetic can be PROVED
// without a touchscreen, without the guest and without a Switch. The
// conversion deliberately lives in a header that depends on neither libnx nor
// JNI (native-switch/include/rr3/guest_touch_convert.hpp) so that this file
// can include it unchanged and run as an ordinary program.
//
// Compiled and run by native-switch/tests/test_guest_touch_hw.py with the
// HOST compiler. It is NOT part of the NRO: the Makefile's SOURCES list does
// not include the tests directory.
//
// The exit code is the verdict: 0 when every check passed, 1 otherwise, and
// every failure is printed with the file and line that made it.

#include "rr3/guest_touch_convert.hpp"

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line) {
    ++g_checks;
    if (condition) {
        std::printf("ok   %s\n", what);
        return;
    }
    std::printf("FAIL line %d: %s\n", line, what);
    ++g_failures;
}

#define CHECK(cond, what) check((cond), (what), __LINE__)

using rr3::kTouchEventCapacity;
using rr3::rr3PlanTouchFrame;
using rr3::TouchAction;
using rr3::TouchEvent;
using rr3::TouchGuestState;
using rr3::TouchMapping;
using rr3::TouchSample;
using rr3::TouchTracker;

// The Switch's digitizer and the guest's view are both 1280x720.
const TouchMapping kIdentity{1280, 720, 1280, 720};
// A digitizer that is half the guest's view, to prove the mapping is a
// mapping and not an accident of the arithmetic.
const TouchMapping kHalved{640, 360, 1280, 720};

TouchSample sample(int x, int y, unsigned int finger, bool start, bool end) {
    TouchSample s{};
    s.rawX = x;
    s.rawY = y;
    s.finger = finger;
    s.start = start;
    s.end = end;
    return s;
}

// CASE 1: the first press of a run.
//
// The guest's touch object SHIPS with down=1 and x=y=-1, and the guest's
// engine stores a new point ONLY when no finger is down. So the stale
// pointer has to be released BEFORE the press, and the release has to come
// first in the event list.
void caseFirstPressReleasesTheStalePointer() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = true;
    guest.pointerId = 0;
    guest.x = -1;
    guest.y = -1;

    TouchSample samples[1];
    samples[0] = sample(640, 360, 0u, true, false);

    TouchEvent events[kTouchEventCapacity];
    const int produced =
        rr3PlanTouchFrame(samples, 1, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);

    CHECK(produced == 2, "the first press produces two events");
    CHECK(produced >= 1 && events[0].action == TouchAction::Release,
          "the stale pointer is released FIRST");
    CHECK(produced >= 1 && events[0].pointerId == 0,
          "the release carries the GUEST's pointer id");
    CHECK(produced >= 1 && events[0].x == -1 && events[0].y == -1,
          "the release carries the GUEST's coordinates");
    CHECK(produced >= 1 && events[0].last == false,
          "the stale release is not the last pointer (last=false)");
    CHECK(produced >= 2 && events[1].action == TouchAction::Begin,
          "the press follows the release");
    CHECK(produced >= 2 && events[1].pointerId == 0,
          "the press carries the digitizer's finger id");
    CHECK(produced >= 2 && events[1].x == 640 && events[1].y == 360,
          "the press carries the guest's view pixels");
    CHECK(tracker.down, "the tracker holds the finger");
    CHECK(tracker.pointerId == 0, "the tracker holds the pointer id");
    CHECK(tracker.x == 640 && tracker.y == 360, "the tracker holds the position");
}

// CASE 2: a press with no stale pointer produces exactly one event.
void casePressWithoutAStalePointer() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample samples[1];
    samples[0] = sample(100, 200, 3u, true, false);

    TouchEvent events[kTouchEventCapacity];
    const int produced =
        rr3PlanTouchFrame(samples, 1, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);

    CHECK(produced == 1, "a press with no stale pointer produces one event");
    CHECK(produced >= 1 && events[0].action == TouchAction::Begin,
          "and that event is the press");
    CHECK(produced >= 1 && events[0].pointerId == 3,
          "the pointer id is the digitizer's finger id");
    CHECK(tracker.down && tracker.finger == 3u,
          "the tracker holds that finger");
}

// CASE 3: the move. Only a MOVED finger moves.
void caseMove() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample samples[1];
    samples[0] = sample(640, 360, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(samples, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);

    TouchSample still[1];
    still[0] = sample(640, 360, 0u, false, false);
    int produced =
        rr3PlanTouchFrame(still, 1, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 0, "a finger that did not move produces nothing");

    TouchSample moved[1];
    moved[0] = sample(641, 360, 0u, false, false);
    produced = rr3PlanTouchFrame(moved, 1, &tracker, guest, kIdentity, false,
                                  events, kTouchEventCapacity);
    CHECK(produced == 1, "a finger that moved produces one event");
    CHECK(produced >= 1 && events[0].action == TouchAction::Move,
          "and that event is the move");
    CHECK(produced >= 1 && events[0].pointerId == 0,
          "the move carries the SAME pointer id as the press");
    CHECK(produced >= 1 && events[0].x == 641 && events[0].y == 360,
          "the move carries the new position");
    CHECK(tracker.x == 641, "the tracker follows the finger");
}

// CASE 4: the lift.
void caseLift() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample down[1];
    down[0] = sample(640, 360, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(down, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);

    TouchSample up[1];
    up[0] = sample(641, 360, 0u, false, true);
    const int produced =
        rr3PlanTouchFrame(up, 1, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 1, "the lift produces one event");
    CHECK(produced >= 1 && events[0].action == TouchAction::End,
          "and that event is the lift");
    CHECK(produced >= 1 && events[0].pointerId == 0,
          "the lift carries the SAME pointer id as the press");
    CHECK(produced >= 1 && events[0].x == 641 && events[0].y == 360,
          "the lift carries the position the finger lifted at");
    CHECK(produced >= 1 && events[0].last == true,
          "the lift is the last pointer (last=true), like Android's ACTION_UP");
    CHECK(!tracker.down, "the tracker lets go");

    // Nothing after the lift.
    const int after =
        rr3PlanTouchFrame(nullptr, 0, &tracker, guest, kIdentity, false,
                          events, kTouchEventCapacity);
    CHECK(after == 0, "nothing happens after the lift");
}

// CASE 5: the mapping is a mapping. A half-size digitizer is scaled.
void caseMapping() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample samples[1];
    samples[0] = sample(320, 180, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(samples, 1, &tracker, guest, kHalved, false, events,
                      kTouchEventCapacity);
    CHECK(tracker.x == 640 && tracker.y == 360,
          "a 640x360 digitizer is scaled to the guest's 1280x720 view");

    // And the identity mapping really is the identity.
    TouchTracker identityTracker{};
    TouchSample identitySamples[1];
    identitySamples[0] = sample(1234, 567, 0u, true, false);
    rr3PlanTouchFrame(identitySamples, 1, &identityTracker, guest, kIdentity,
                      false, events, kTouchEventCapacity);
    CHECK(identityTracker.x == 1234 && identityTracker.y == 567,
          "the 1280x720 mapping is the identity");

    // Out of range is clamped into the view.
    TouchTracker clampTracker{};
    TouchSample clamped[1];
    clamped[0] = sample(9999, 9999, 0u, true, false);
    rr3PlanTouchFrame(clamped, 1, &clampTracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);
    CHECK(clampTracker.x == 1279 && clampTracker.y == 719,
          "a coordinate past the view is clamped to the view's last pixel");
}

// CASE 6: the finger vanishes between frames without an End sample. The
// guest must still be told the finger is gone, or a button stays held.
void caseVanishingFinger() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample down[1];
    down[0] = sample(640, 360, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(down, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);

    const int produced =
        rr3PlanTouchFrame(nullptr, 0, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 1, "a finger that vanishes produces a lift");
    CHECK(produced >= 1 && events[0].action == TouchAction::End,
          "and that event is the lift");
    CHECK(produced >= 1 && events[0].x == 640 && events[0].y == 360,
          "the lift carries the last known position");
    CHECK(!tracker.down, "the tracker lets go");
}

// CASE 7: a finger that lands and lifts inside one frame still gets a press
// and a release, or a tap would be invisible.
void caseTapInOneFrame() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample samples[1];
    samples[0] = sample(640, 360, 0u, true, true);
    TouchEvent events[kTouchEventCapacity];
    const int produced =
        rr3PlanTouchFrame(samples, 1, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 2, "a tap produces a press and a lift");
    CHECK(produced >= 1 && events[0].action == TouchAction::Begin,
          "the press comes first");
    CHECK(produced >= 2 && events[1].action == TouchAction::End,
          "the lift comes second");
    CHECK(!tracker.down, "the tracker lets go after the tap");
}

// CASE 8: the file control owns the guest's touch path. The hardware path
// must not press into a scripted gesture; it may only let go of what it
// holds.
void caseSuspended() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample down[1];
    down[0] = sample(640, 360, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(down, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);
    CHECK(tracker.down, "the tracker holds the finger before the suspension");

    TouchSample still[1];
    still[0] = sample(640, 360, 0u, false, false);
    const int produced =
        rr3PlanTouchFrame(still, 1, &tracker, guest, kIdentity, true, events,
                          kTouchEventCapacity);
    CHECK(produced == 1, "a suspended frame only lets the finger go");
    CHECK(produced >= 1 && events[0].action == TouchAction::End,
          "and that event is the lift");
    CHECK(!tracker.down, "the tracker lets go");

    // And while suspended, a NEW finger is not pressed at all.
    TouchSample again[1];
    again[0] = sample(700, 400, 0u, true, false);
    const int pressed =
        rr3PlanTouchFrame(again, 1, &tracker, guest, kIdentity, true, events,
                          kTouchEventCapacity);
    CHECK(pressed == 0, "a suspended frame never presses");
    CHECK(!tracker.down, "and the tracker stays free");
}

// CASE 9: the guest's engine is single-pointer. A second finger is ignored
// while one is being driven.
void caseSecondFingerIgnored() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample first[1];
    first[0] = sample(640, 360, 0u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(first, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);

    TouchSample both[2];
    both[0] = sample(640, 360, 0u, false, false);
    both[1] = sample(100, 100, 1u, true, false);
    const int produced =
        rr3PlanTouchFrame(both, 2, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 0, "a second finger does not press while one is held");
    CHECK(tracker.finger == 0u, "the driven finger is unchanged");
}

// CASE 10: the stale release is NOT repeated when the pointer this frame
// released IS the guest's stale pointer.
void caseNoDoubleRelease() {
    TouchTracker tracker{};
    TouchGuestState guest{};
    guest.down = false;

    TouchSample down[1];
    down[0] = sample(640, 360, 5u, true, false);
    TouchEvent events[kTouchEventCapacity];
    rr3PlanTouchFrame(down, 1, &tracker, guest, kIdentity, false, events,
                      kTouchEventCapacity);

    // The guest still reports the pointer it was given, still down.
    guest.down = true;
    guest.pointerId = 5;
    guest.x = 640;
    guest.y = 360;

    // The finger lifts and a new one lands in the same frame.
    TouchSample lift[2];
    lift[0] = sample(640, 360, 5u, false, true);
    lift[1] = sample(700, 400, 6u, true, false);
    const int produced =
        rr3PlanTouchFrame(lift, 2, &tracker, guest, kIdentity, false, events,
                          kTouchEventCapacity);
    CHECK(produced == 2, "the lift and the next press are two events");
    CHECK(produced >= 1 && events[0].action == TouchAction::End,
          "the lift comes first");
    CHECK(produced >= 2 && events[1].action == TouchAction::Begin,
          "the next press comes second");
    CHECK(produced >= 2 && events[1].pointerId == 6,
          "the next press carries the new finger's id");
    CHECK(produced >= 1 && events[0].pointerId == 5,
          "the lift carries the old finger's id");
}

}  // namespace

int main() {
    // Unbuffered: if a case crashes the process, the checks that already
    // ran are still on stdout, so the failure says WHERE it died.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    caseFirstPressReleasesTheStalePointer();
    casePressWithoutAStalePointer();
    caseMove();
    caseLift();
    caseMapping();
    caseVanishingFinger();
    caseTapInOneFrame();
    caseSuspended();
    caseSecondFingerIgnored();
    caseNoDoubleRelease();

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
