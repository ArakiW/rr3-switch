#include "rr3/guest_touch_hw.hpp"

#include "rr3/gfx_backend.hpp"
#include "rr3/guest_touch.hpp"
#include "rr3/recovery_guard.hpp"

#include <switch.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

// The escape hatch for the touchscreen-init command.
//
// libnx's PUBLIC touchscreen initialiser is exactly the HID service's
// command 11 followed by an ABORT when the HID service rejects the command
// (libnx.a's hid.o: mov w0, #0xb ; bl _hidCmdInAruidNoOut ; cbnz w0,
// <diagAbortWithResult>). The private helper is not reachable -- readelf
// reports _hidCmdInAruidNoOut as a LOCAL symbol in libnx.a's hid.o, so it
// cannot be linked -- which leaves the public wrapper as the only way to
// send the command.
//
// MEASURED on Eden (archive 20261024-061159-eden-touchhw-cap): the wrapper
// RETURNED instead of aborting, so the command is accepted there. On a
// platform whose HID service rejects it, the port would die at startup, so
// this flag skips the command. The touch path does not need it: libnx's
// hidGetTouchScreenStates() reads the touchscreen entry out of the HID
// shared memory, and the shared memory is filled by the platform whether or
// not the command was sent.
constexpr const char* kTouchHwNoInitFlag =
    "sdmc:/switch/rr3-native-rehost/touch-hw-noinit.flag";

constexpr const char* kTouchHwLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// The capability probe. OFF BY DEFAULT: it drives the HID service's debug
// autopilot, which is a thing an emulator can do and a retail console cannot.
// The probe exists to answer the question a run cannot otherwise answer --
// "can this platform deliver a touch at all?" -- and it is a diagnostic, not
// a feature. The TOUCH PATH ITSELF is not behind this flag: a touchscreen
// that needs a switch flipped is not a touchscreen.
constexpr const char* kTouchHwProbeFlag =
    "sdmc:/switch/rr3-native-rehost/touch-hw-probe.flag";

// The synthetic finger.
//
// The probe drives it through the HID service's DEBUG AUTOPILOT, so the
// EMULATOR can exercise the REAL hardware path: the autopilot publishes a
// finger into the HID shared memory, libnx reads it out with
// hidGetTouchScreenStates(), the port converts it exactly as it converts a
// real digitizer's sample, and the guest sees a finger. MEASURED on Eden
// (archives 20261024-061159-eden-touchhw-cap and ...-eden-touchhw): the
// autopilot command is accepted (result 0x0) and the state DOES reach the
// shared memory, one read late.
//
// The coordinates are distinctive on purpose: the guest's own log then shows
// the guest holding EXACTLY these view pixels, which is the whole claim.
constexpr int kSyntheticX = 321;
constexpr int kSyntheticY = 654;
constexpr unsigned kSyntheticFinger = 7;
// How long the finger is held, in port frames.
constexpr unsigned kSyntheticHoldFrames = 120;

// The digitizer and the guest's view are both 1280x720 on a Switch, so the
// mapping is the identity. Both sizes are NAMED here so that a different
// digitizer would be a changed constant and not a changed computation.
//
// THE VIEW SIZE IS THE ONE THE PORT TELLS THE GUEST (onViewChangedJNI), so
// when rr3-screen.txt's "view=WxH" moves it, the mapping has to move with
// it: the digitizer still reports 1280x720 pixels, and the guest's view is
// now a different size.
constexpr TouchMapping kTouchMapping{1280, 720, 1280, 720};

TouchMapping touchMapping() {
    // The digitizer still reports the OUTPUT screen's pixels, but the guest's
    // view is shown in a RECTANGLE of that screen: the present path fits the
    // view in and leaves bars when the two aspect ratios differ.  The
    // mapping therefore carries the rectangle and not just the sizes -- a tap
    // on a bar is a tap on nothing, and a tap inside the rectangle has to
    // land on the button the player actually sees.
    const GfxPresentRect rect = gfxPresentRect(1280, 720);
    TouchMapping mapping{1280, 720, gfxViewWidth(), gfxViewHeight()};
    mapping.sourceX = rect.x;
    mapping.sourceY = rect.y;
    mapping.sourceRectWidth = rect.width;
    mapping.sourceRectHeight = rect.height;
    return mapping;
}

// How often the digitizer's raw state is summarised in the log. The guest's
// own log is the primary instrument; this is the sidebar.
constexpr unsigned kTouchHwStateEveryPumps = 300;

// How many hardware events are logged in full before the log switches to a
// rate-limited summary. A finger on the screen produces a move per frame, so
// an unbounded log would drown the guest's own lines.
constexpr unsigned kTouchHwEventLogLimit = 64;
// ... and how many events pass between summaries once the limit is reached.
constexpr unsigned kTouchHwSummaryEvery = 256;

TouchTracker g_tracker{};
bool g_prepared = false;
bool g_digitizerPresent = false;
bool g_absentLogged = false;
unsigned g_pumpCount = 0;
unsigned g_eventsLogged = 0;
unsigned g_eventsTotal = 0;
unsigned g_suspendState = 0;
// The synthetic gesture's state machine.
bool g_syntheticActive = false;
bool g_syntheticStarted = false;
bool g_syntheticReleased = false;
unsigned g_syntheticFrame = 0;
// The number of fingers the digitizer reported on the previous frame, so a
// change is logged once instead of every frame.
int g_lastTouchCount = -1;

bool flagPresent(const char* path) {
    if (path == nullptr) {
        return false;
    }
    struct stat info {};
    return ::stat(path, &info) == 0;
}

// Reads the digitizer's shared memory. Returns the number of touchscreen
// ENTRIES (1 on a Switch) and fills `state` with the first entry.
//
// The HID shared memory is the ORACLE for "is there a touchscreen on this
// platform at all": libnx's hidGetTouchScreenStates() copies the entries out
// of it with atomic loads, so a platform with no touchscreen reports zero
// entries rather than inventing one.
size_t readDigitizer(HidTouchScreenState* state) {
    if (state == nullptr) {
        return 0;
    }
    std::memset(state, 0, sizeof(*state));
    // libnx's hidGetTouchScreenStates() ABORTS when the HID shared memory is
    // not mapped (its own check: cbz on hidGetSharedmemAddr() -> result
    // 0x1159). The pointer is asked for first, so a platform without the HID
    // service is reported instead of killing the run.
    if (hidGetSharedmemAddr() == nullptr) {
        return 0;
    }
    return hidGetTouchScreenStates(state, 1);
}

void logTouchHw(const char* phase, unsigned long long ordinal,
                const char* details) {
    appendGuestBootLine(kTouchHwLogPath, phase, ordinal, 0, details);
}

// The capability probe: does this platform have a touchscreen, and can the
// emulator be made to produce one?
//
// It asks the HID debug service to AUTOPILOT a synthetic finger. The
// autopilot is the same mechanism the emulator's own debugger uses, so a
// successful round trip proves the whole chain -- shared memory, libnx read,
// port conversion, guest call -- with a finger no human ever touched.
//
// The finger is driven from the FRAME PUMP, not from here: the emulator
// publishes the autopilot state into the HID shared memory on its own
// schedule (MEASURED: it lands one read late), so a set-and-read from the
// same thread can miss it. The frame pump re-asserts the autopilot every
// frame while the finger is held, which is what a real digitizer does.
void runTouchHardwareProbe() {
    char detail[320];

    HidTouchScreenState before{};
    const size_t beforeEntries = readDigitizer(&before);
    std::snprintf(detail, sizeof(detail),
                  "stage=digitizer-before entries=%zu touches=%d",
                  beforeEntries, static_cast<int>(before.count));
    logTouchHw("touch-hw-probe", 960, detail);

    // The debug autopilot. It needs the hid debug service, which only exists
    // for a debug-enabled title -- on a retail console this fails and the
    // probe says so, which is the honest answer.
    const Result initResult = hiddbgInitialize();
    char hiddbg[128];
    std::snprintf(hiddbg, sizeof(hiddbg), "stage=hiddbg-init result=0x%x",
                   static_cast<unsigned>(initResult));
    logTouchHw("touch-hw-probe", 960, hiddbg);
    if (R_FAILED(initResult)) {
        return;
    }

    // Armed: the frame pump drives the finger from here.
    g_syntheticActive = true;
    g_syntheticStarted = false;
    g_syntheticReleased = false;
    g_syntheticFrame = 0;
    std::snprintf(detail, sizeof(detail),
                  "stage=synthetic-armed finger=%u x=%d y=%d hold=%u frames",
                  kSyntheticFinger, kSyntheticX, kSyntheticY,
                  kSyntheticHoldFrames);
    logTouchHw("touch-hw-probe", 960, detail);
}

// Drives the synthetic finger for one frame. Returns true while the gesture
// is still running.
bool pumpSyntheticFinger() {
    if (!g_syntheticActive) {
        return false;
    }
    // Wait until the file control has looked at the card. Until then it
    // cannot say whether a script owns the guest's touch path, and the
    // hardware path stands down -- so a finger asserted in that window
    // would be asserted into a path nobody owns.
    if (!guestTouchControlHasPolled()) {
        return true;
    }
    HidTouchState finger{};
    finger.attributes = g_syntheticFrame == 0 ? HidTouchAttribute_Start : 0u;
    finger.finger_id = kSyntheticFinger;
    finger.x = static_cast<u32>(kSyntheticX);
    finger.y = static_cast<u32>(kSyntheticY);
    const Result setResult = hiddbgSetTouchScreenAutoPilotState(&finger, 1);
    if (!g_syntheticStarted) {
        g_syntheticStarted = true;
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "stage=synthetic-set result=0x%x finger=%u x=%d y=%d",
                      static_cast<unsigned>(setResult), finger.finger_id,
                      kSyntheticX, kSyntheticY);
        logTouchHw("touch-hw-probe", 960, detail);
    }
    ++g_syntheticFrame;
    if (g_syntheticFrame > kSyntheticHoldFrames) {
        const Result unsetResult = hiddbgUnsetTouchScreenAutoPilotState();
        g_syntheticActive = false;
        g_syntheticReleased = true;
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "stage=synthetic-unset result=0x%x after=%u frames",
                      static_cast<unsigned>(unsetResult), g_syntheticFrame);
        logTouchHw("touch-hw-probe", 960, detail);
        hiddbgExit();
    }
    return g_syntheticActive;
}

}  // namespace

bool prepareGuestTouchHardware() {
    g_prepared = false;
    g_digitizerPresent = false;
    g_absentLogged = false;
    g_pumpCount = 0;
    g_eventsLogged = 0;
    g_eventsTotal = 0;
    g_suspendState = 0;
    g_lastTouchCount = -1;
    g_syntheticActive = false;
    g_syntheticStarted = false;
    g_syntheticReleased = false;
    g_syntheticFrame = 0;
    g_tracker = TouchTracker{};

    char detail[320];

    // hidInitialize() is REFERENCE COUNTED inside libnx (libnx.a's hid.o
    // takes a mutex, bumps a counter and only does the real work when the
    // counter was zero), so calling it here is safe whether or not libnx
    // already did it at startup -- and it guarantees that the HID service
    // session and the HID shared memory exist, which is what the digitizer
    // read below needs.
    const Result hidResult = hidInitialize();
    std::snprintf(detail, sizeof(detail), "hid-initialize result=0x%x",
                   static_cast<unsigned>(hidResult));
    logTouchHw("touch-hw-armed", 957, detail);

    if (hidGetSharedmemAddr() == nullptr) {
        logTouchHw("touch-hw-absent", 958,
                   "reason=no-hid-shared-memory (the HID service is not "
                   "mapped on this platform)");
        return false;
    }

    // The touchscreen-init command. MEASURED on Eden (archive
    // 20261024-061159-eden-touchhw-cap): libnx's hidInitializeTouchScreen()
    // returned instead of aborting, so the HID service accepts the command
    // there. It is libnx's documented way to bring the touchscreen up and it
    // is the same call every libnx touch program makes.
    //
    // It is guarded by the HID service being available at all: libnx's
    // wrapper ABORTS the process when the command fails, and a platform
    // without the HID service is better served by a port that says so than
    // by a port that dies at startup.
    if (R_SUCCEEDED(hidResult) && !flagPresent(kTouchHwNoInitFlag)) {
        logTouchHw("touch-hw-init", 956,
                   "calling hidInitializeTouchScreen(): libnx aborts the "
                   "process if the HID service rejects command 11");
        hidInitializeTouchScreen();
        logTouchHw("touch-hw-init", 956, "hidInitializeTouchScreen returned");
    } else {
        logTouchHw("touch-hw-init", 956,
                   "hidInitializeTouchScreen() NOT called: the HID service is "
                   "not available");
    }

    HidTouchScreenState state{};
    const size_t entries = readDigitizer(&state);
    g_digitizerPresent = entries > 0;
    const TouchMapping mapping = touchMapping();
    std::snprintf(detail, sizeof(detail),
                  "entries=%zu touches=%d present=%d mapping=%dx%d->%dx%d "
                  "rect=%d,%d %dx%d",
                  entries, static_cast<int>(state.count),
                  g_digitizerPresent ? 1 : 0, mapping.sourceWidth,
                  mapping.sourceHeight, mapping.viewWidth,
                  mapping.viewHeight, mapping.sourceX, mapping.sourceY,
                  rr3TouchRectWidth(mapping), rr3TouchRectHeight(mapping));
    logTouchHw("touch-hw-armed", 957, detail);

    g_prepared = true;

    // The probe is opt-in and runs once. It runs AFTER the init command so
    // that it measures the state a real program would see.
    if (flagPresent(kTouchHwProbeFlag)) {
        runTouchHardwareProbe();
    }
    return g_digitizerPresent;
}

bool guestTouchHardwareAvailable() {
    return g_digitizerPresent;
}

unsigned int guestTouchHardwareEventCount() {
    return g_eventsTotal;
}

void pumpGuestTouchHardware(void* jniEnvironment, void* activityObject) {
    if (!g_prepared || jniEnvironment == nullptr) {
        return;
    }
    ++g_pumpCount;

    // 1. The digitizer. One shared-memory copy per frame; this is the cheap
    //    part of the hardware path.
    //
    //    The synthetic finger (when the probe armed it) is asserted FIRST,
    //    because the autopilot publishes into the shared memory on the
    //    emulator's schedule and the read that follows is what sees it.
    pumpSyntheticFinger();
    HidTouchScreenState hw{};
    const size_t entries = readDigitizer(&hw);
    if (entries == 0) {
        if (!g_absentLogged) {
            g_absentLogged = true;
            g_digitizerPresent = false;
            logTouchHw("touch-hw-absent", 958,
                       "reason=no-touchscreen-entries (the HID shared memory "
                       "reports no touchscreen device)");
        }
        return;
    }
    g_digitizerPresent = true;

    // What the digitizer actually reported, whenever the number of fingers on
    // the glass changes. A run in which this never fires and the event log
    // is empty is a run in which NOTHING was on the glass -- which is the
    // difference between "the touchscreen does not work" and "nobody touched
    // it".
    if (hw.count != g_lastTouchCount) {
        g_lastTouchCount = hw.count;
        char detail[256];
        std::snprintf(detail, sizeof(detail),
                      "touches=%d first{finger=%u x=%u y=%u attrs=0x%x "
                      "diameter=%ux%u}",
                      static_cast<int>(hw.count),
                      hw.count > 0 ? hw.touches[0].finger_id : 0u,
                      hw.count > 0 ? hw.touches[0].x : 0u,
                      hw.count > 0 ? hw.touches[0].y : 0u,
                      hw.count > 0 ? hw.touches[0].attributes : 0u,
                      hw.count > 0 ? hw.touches[0].diameter_x : 0u,
                      hw.count > 0 ? hw.touches[0].diameter_y : 0u);
        logTouchHw("touch-hw-finger", 963, detail);
    }

    // 2. The guest's own state. The guest's JNI exports drop a touch when the
    //    guest is paused or unfocused, and the port must not fight that, so
    //    the gates are read here and the press is suspended while they are
    //    shut. The guest's state is ALSO the source of the stale pointer that
    //    has to be released before the first press.
    const GuestTouchState guest = readGuestTouchState();

    // 3. The file control owns the guest's touch path while its control file
    //    is on the card. The hardware path never presses into a scripted
    //    gesture; it only lets go of a finger it is holding.
    //
    //    Four conditions, because there are four ways the file control can
    //    still own the path: it has not looked at the card yet (so it cannot
    //    say whether a script is there), the file is on the card (the script
    //    may append to it at any moment), the queue still holds events (the
    //    file was deleted but its events are due), and this very frame
    //    already carried a file event (the file control is pumped
    //    immediately before this one).
    const bool controlOwnsTouch = !guestTouchControlHasPolled() ||
                                   guestTouchControlFilePresent() ||
                                   guestTouchControlHasPending() ||
                                   guestTouchControlInjectedThisFrame();
    const bool guestShut =
        !guest.valid || guest.paused != 0 || guest.focus == 0;
    const bool suspend = controlOwnsTouch || guestShut;
    if (suspend && g_suspendState == 0) {
        g_suspendState = 1;
        char detail[224];
        std::snprintf(detail, sizeof(detail),
                      "reason=%s unconfirmed=%d file=%d pending=%d injected=%d "
                      "guest{valid=%d paused=%u focus=%u}",
                      guestShut ? "guest-shut" : "control-file",
                      guestTouchControlHasPolled() ? 0 : 1,
                      guestTouchControlFilePresent() ? 1 : 0,
                      guestTouchControlHasPending() ? 1 : 0,
                      guestTouchControlInjectedThisFrame() ? 1 : 0,
                      guest.valid ? 1 : 0, guest.paused, guest.focus);
        logTouchHw("touch-hw-suspend", 959, detail);
    } else if (!suspend && g_suspendState != 0) {
        g_suspendState = 0;
        logTouchHw("touch-hw-resume", 959, "presses resumed");
    }

    // 4. The digitizer's fingers, in the digitizer's own pixels.
    TouchSample samples[16];
    int sampleCount = 0;
    const int touchCount =
        hw.count > 0 && hw.count <= 16 ? static_cast<int>(hw.count) : 0;
    for (int i = 0; i < touchCount; ++i) {
        const HidTouchState& finger = hw.touches[i];
        samples[sampleCount].rawX = static_cast<int>(finger.x);
        samples[sampleCount].rawY = static_cast<int>(finger.y);
        samples[sampleCount].finger = finger.finger_id;
        samples[sampleCount].start =
            (finger.attributes & HidTouchAttribute_Start) != 0;
        samples[sampleCount].end =
            (finger.attributes & HidTouchAttribute_End) != 0;
        ++sampleCount;
    }

    // 5. The conversion. Pure, and proved by native-switch/tests/
    //    test_guest_touch_hw.py against synthetic samples.
    TouchGuestState guestForPlan{};
    guestForPlan.down = guest.down;
    guestForPlan.pointerId = guest.pointerId;
    guestForPlan.x = guest.x;
    guestForPlan.y = guest.y;

    TouchEvent events[kTouchEventCapacity];
    const int produced =
        rr3PlanTouchFrame(samples, sampleCount, &g_tracker, guestForPlan,
                           touchMapping(), suspend, events, kTouchEventCapacity);

    // 6. The guest.
    for (int i = 0; i < produced; ++i) {
        const TouchEvent& event = events[i];
        GuestTouchAction action = GuestTouchAction::Begin;
        const char* actionName = "down";
        switch (event.action) {
        case TouchAction::Begin:
            action = GuestTouchAction::Begin;
            actionName = "down";
            break;
        case TouchAction::Move:
            action = GuestTouchAction::Move;
            actionName = "move";
            break;
        case TouchAction::End:
            action = GuestTouchAction::End;
            actionName = "up";
            break;
        case TouchAction::Release:
            action = GuestTouchAction::Release;
            actionName = "release";
            break;
        }
        injectGuestTouchEvent(jniEnvironment, activityObject, action,
                              event.pointerId, event.x, event.y, event.last,
                              "touch-hw-inject");
        ++g_eventsTotal;

        if (g_eventsLogged < kTouchHwEventLogLimit) {
            ++g_eventsLogged;
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                          "action=%s id=%d x=%d y=%d last=%d finger=%u frame=%u",
                          actionName, event.pointerId, event.x, event.y,
                          event.last ? 1 : 0, g_tracker.finger, g_pumpCount);
            logTouchHw("touch-hw-event", 961, detail);
        } else if ((g_eventsTotal % kTouchHwSummaryEvery) == 0) {
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "summary total=%u logged=%u (the per-event log stops "
                          "after %u events)",
                          g_eventsTotal, g_eventsLogged, kTouchHwEventLogLimit);
            logTouchHw("touch-hw-summary", 961, detail);
        }
    }

    // 7. The sidebar: what the digitizer says, even when nothing was
    //    injected. A run in which this line says touches=0 and the event log
    //    is empty is a run in which NOTHING was on the glass, which is the
    //    difference between "the touchscreen does not work" and "nobody
    //    touched it".
    if ((g_pumpCount % kTouchHwStateEveryPumps) == 0) {
        char detail[288];
        std::snprintf(detail, sizeof(detail),
                      "entries=%zu touches=%d held=%d gid=%d gx=%d gy=%d "
                      "events=%u suspend=%d",
                      entries, static_cast<int>(hw.count),
                      g_tracker.down ? 1 : 0, guest.pointerId, guest.x,
                      guest.y, g_eventsTotal, suspend ? 1 : 0);
        logTouchHw("touch-hw-state", 962, detail);
    }
}

}  // namespace rr3
