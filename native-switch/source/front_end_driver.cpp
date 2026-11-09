// The semantic front-end driver.
//
// Drives the guest's front end by SCREEN NAME instead of by frame number.
//
// WHY THIS EXISTS
// ---------------
// The port's attract mode was driven by a frame-based pad script
// (native-switch/card/rr3-pad-autopilot.txt). That script failed across three
// consecutive rounds with the exact same root cause:
//
//   m3  Confirm button held down -> guest froze on "you received 50 gold" modal
//       (FrontEndAwardScreen / RewardPopup.gui) because held buttons produce no
//       button-down transition.
//   v3  Confirm button pulsed -> dismissed popup and reached the race at
//       frame 1830.
//   v4  Confirm button pulsed with save data restored -> repeatedly pressed
//       confirm and walked the guest into the NASCAR promo submenu
//       (CareerGroupScreen) with 223 controls, never reaching a race.
//
// This driver replaces blind frame-number button pulsing with a semantic state
// machine:
//   1. Read the current screen name from the cached UiControlSample.
//   2. Look the screen up in a static data table of actions.
//   3. Act ONCE per screen visit (preventing the v4 walk-out failure).
//   4. Log every decision under phase "fe-driver".
//   5. If a target control is not found, log it as a finding and do nothing.
//
// AND THE FRAME LOOP NEVER TOUCHES GUEST MEMORY
// ---------------------------------------------
// The driver reads only the CACHED UiControlSample and control rectangles
// populated by pumpUiControlProbe, and is pumped from the INPUT PUMP
// (pumpGuestControllerInput) on the guest's own thread.
// ---------------------------------------------------------------------------

#include "rr3/front_end_driver.hpp"

#include "rr3/jni_bridge.hpp"
#include "rr3/race_state_probe.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/ui_control_probe.hpp"
#include "rr3/ui_tap.hpp"

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

constexpr const char* kGuestViewLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// THE SCREEN ACTION TABLE.
//
// Pure data: one entry per screen.
// Each entry specifies the action (None, PressControl, OpenScreen), the target
// (control id_string or screen name), and the measurement justification.
constexpr ScreenAction kScreenActions[] = {
    // 1. Reward popup: "you received 50 gold" modal dialog.
    // Measured in m3: held button froze on FrontEndAwardScreen at frame 5200
    // (Chinese text: \u60a8\u6536\u5230\u4e8650\u91d1\u5e01).
    // RewardPopup.gui declares BTN_CLOSE (GuiButton) anchored TopRight where
    // the hint glove points.
    {
        "FrontEndAwardScreen",
        DriverActionKind::PressControl,
        "BTN_CLOSE",
        "Dismiss the 50 gold reward modal (RewardPopup.gui) that froze m3 at frame 5200",
    },
    {
        "RewardPopup",
        DriverActionKind::PressControl,
        "BTN_CLOSE",
        "Dismiss reward modal when reported under document root name",
    },
    {
        "AwardsScreen",
        DriverActionKind::PressControl,
        "BTN_CLOSE",
        "Dismiss award screen overlay if encountered during career progression",
    },

    // 2. Event selection: Stream Selection screen.
    // Measured in m3, v3, v4 at frame 150 (342 controls).
    // CareerBanner_foursquare.gui declares ACTION_BTN (GuiButton at 793,359,284,284).
    {
        "EventMapScreen",
        DriverActionKind::PressControl,
        "ACTION_BTN",
        "Advance from Stream Selection into event card list (CareerBanner_foursquare.gui)",
    },

    // 3. Event card selection: EventsScreen.
    // Measured in v3 at frame 1530 (748 controls).
    // Event_card.gui declares CARD_BUTTON (ImageButton at 193,200,288,410).
    {
        "EventsScreen",
        DriverActionKind::PressControl,
        "CARD_BUTTON",
        "Select the first event card to proceed to car selection (Event_card.gui)",
    },

    // 4. Car selection: CarSelectScreen.
    // Measured in v3 at frame 1620 (1200 controls).
    // MainMenu.gui / RaceStartScreen.gui declares BTN_RACE (GuiButton at 1107,697).
    // Pressing BTN_RACE transitions the guest into the race (metrics live at f=1830).
    {
        "CarSelectScreen",
        DriverActionKind::PressControl,
        "BTN_RACE",
        "Start the race from car selection (MainMenu.gui / RaceStartScreen.gui)",
    },
    {
        "RaceStartScreen",
        DriverActionKind::PressControl,
        "BTN_RACE",
        "Confirm race start if RaceStartScreen modal is active",
    },

    // 5. NASCAR promo panel / non-raceable submenus: CareerGroupScreen.
    // Measured in v4 at frame 330 (223 controls): repeated confirm presses walked
    // the guest into the promo panel. Do NOT press anything there; navigate BACK
    // to EventMapScreen via the guest's registered entry 0xA679A0.
    {
        "CareerGroupScreen",
        DriverActionKind::OpenScreen,
        "EventMapScreen",
        "Escape NASCAR promo panel back to Stream Selection (kGuestScreenEntries 0xA679A0)",
    },
    {
        "CHAMPIONSHIP_ROUND_HUB_SCREEN",
        DriverActionKind::OpenScreen,
        "EventMapScreen",
        "Escape championship round hub back to Stream Selection",
    },
};

constexpr size_t kScreenActionCount =
    sizeof(kScreenActions) / sizeof(kScreenActions[0]);

// Visit tracking state.
// An action is executed ONCE per screen visit to prevent repeated actuation.
char s_currentScreen[64] = {0};
bool s_actedOnVisit = false;
bool s_notFoundLogged = false;
unsigned long long s_visitFrame = 0;
unsigned long long s_actionOrdinal = 0;

// The shared tap state machine.
// A tap is down-then-up across 40 frames; see rr3/ui_tap.hpp for the
// measurement (q21 vs q25) proving a same-frame tap is dropped by the guest.
rr3::ui::Tapper s_tapper;

}  // namespace

const ScreenAction* frontEndDriverTable(size_t* count) {
    if (count != nullptr) {
        *count = kScreenActionCount;
    }
    return kScreenActions;
}

const ScreenAction* frontEndDriverLookup(const char* screenName) {
    if (screenName == nullptr || screenName[0] == '\0') {
        return nullptr;
    }
    for (size_t i = 0; i < kScreenActionCount; ++i) {
        if (std::strcmp(kScreenActions[i].screen, screenName) == 0) {
            return &kScreenActions[i];
        }
    }
    return nullptr;
}

void pumpFrontEndDriver(unsigned long long frame,
                        void* jniEnvironment,
                        void* activityObject) {
    if (jniEnvironment == nullptr) {
        jniEnvironment = guestJniEnvironment();
    }
    if (activityObject == nullptr) {
        activityObject = guestActivityObject();
    }

    // 1. DORMANT DURING RACE.
    // The front-end driver only operates outside the race. In a race,
    // touch inputs steer or brake the car, so UI actuation must not fire.
    const RaceStateSample& raceSample = raceStateSample();
    if (raceSample.activeIndex >= 0) {
        // If a tap happened to be in flight when the race started, release it cleanly.
        s_tapper.service(jniEnvironment, activityObject, frame);
        return;
    }

    // 2. ALWAYS SERVICE TAP IN FLIGHT.
    // Every frame decrements the hold duration. After 40 frames, the release
    // (onTouchEndJNI) is injected.
    s_tapper.service(jniEnvironment, activityObject, frame);

    // 3. READ CACHED UI CONTROL PROBE SAMPLE.
    // Pumping happens on the guest's thread from the input pump, but does not
    // walk guest memory directly: it consumes the sample populated by
    // pumpUiControlProbe on its own cadence (30 frames).
    const UiControlSample& sample = uiControlSample();
    if (!sample.ok || sample.screen[0] == '\0') {
        return;
    }

    // 4. DETECT SCREEN VISIT CHANGES.
    // When the screen name changes, reset the visit tracking state.
    if (std::strcmp(s_currentScreen, sample.screen) != 0) {
        std::snprintf(s_currentScreen, sizeof(s_currentScreen), "%s", sample.screen);
        s_actedOnVisit = false;
        s_notFoundLogged = false;
        s_visitFrame = frame;
    }

    // 5. ACT ONCE PER SCREEN VISIT.
    // If an action has already been taken for this visit, do nothing more.
    // This is the core architectural fix for the v4 failure.
    if (s_actedOnVisit) {
        return;
    }

    // 6. DO NOT ISSUE A NEW ACTION WHILE A TAP IS IN FLIGHT.
    if (s_tapper.pending()) {
        return;
    }

    // 7. LOOK UP SCREEN IN THE ACTION TABLE.
    const ScreenAction* actionEntry = frontEndDriverLookup(s_currentScreen);
    if (actionEntry == nullptr) {
        if (!s_notFoundLogged) {
            s_notFoundLogged = true;
            char diag[256];
            std::snprintf(diag, sizeof(diag),
                          "frame=%llu screen=%s action=none why=unmapped-screen",
                          frame, s_currentScreen);
            appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
        }
        return;
    }

    // 8. EXECUTE ACTION BY KIND.
    switch (actionEntry->action) {
    case DriverActionKind::None: {
        s_actedOnVisit = true;
        char diag[256];
        std::snprintf(diag, sizeof(diag),
                      "frame=%llu screen=%s action=none reason=%s",
                      frame, s_currentScreen, actionEntry->reason);
        appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
        break;
    }

    case DriverActionKind::OpenScreen: {
        // Navigate back to a raceable event via uiScreenOpen.
        // allowOpen is true because target ("EventMapScreen") is verified in
        // kGuestScreenEntries (ELF 0xA679A0).
        s_actedOnVisit = true;
        const bool opened = uiScreenOpen(actionEntry->target, true);
        char diag[256];
        std::snprintf(diag, sizeof(diag),
                      "frame=%llu screen=%s action=open target=%s status=%s reason=%s",
                      frame, s_currentScreen, actionEntry->target,
                      opened ? "opened" : "rejected", actionEntry->reason);
        appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
        break;
    }

    case DriverActionKind::PressControl: {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        const bool found = uiControlFind(actionEntry->target, &x, &y, &w, &h);
        if (!found) {
            // Control is not present on screen yet (e.g. animating in).
            // Log once as a finding (not an error) and do nothing this frame.
            if (!s_notFoundLogged) {
                s_notFoundLogged = true;
                char diag[256];
                std::snprintf(diag, sizeof(diag),
                              "frame=%llu screen=%s action=press id=%s found=0 "
                              "why=control-not-found reason=%s",
                              frame, s_currentScreen, actionEntry->target,
                              actionEntry->reason);
                appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
            }
            return;
        }

        // Control is found. Verify view bounds before injecting touch.
        const int cx = x + w / 2;
        const int cy = y + h / 2;
        const int viewW = sample.viewportW > 0 ? sample.viewportW : 1280;
        const int viewH = sample.viewportH > 0 ? sample.viewportH : 720;
        const bool inside =
            (w > 0 && h > 0 && cx >= 0 && cy >= 0 && cx < viewW && cy < viewH);

        if (!inside) {
            if (!s_notFoundLogged) {
                s_notFoundLogged = true;
                char diag[256];
                std::snprintf(diag, sizeof(diag),
                              "frame=%llu screen=%s action=press id=%s found=1 "
                              "x=%d y=%d w=%d h=%d view=%dx%d why=out-of-view reason=%s",
                              frame, s_currentScreen, actionEntry->target,
                              x, y, w, h, viewW, viewH, actionEntry->reason);
                appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
            }
            return;
        }

        // Press the control using the shared 40-frame Tapper.
        if (s_tapper.press(jniEnvironment, activityObject, frame, cx, cy, "fe-driver-press")) {
            s_actedOnVisit = true;
            char diag[256];
            std::snprintf(diag, sizeof(diag),
                          "frame=%llu screen=%s action=press id=%s found=1 "
                          "x=%d y=%d w=%d h=%d cx=%d cy=%d hold=%u reason=%s",
                          frame, s_currentScreen, actionEntry->target,
                          x, y, w, h, cx, cy, rr3::ui::kTapHoldFrames,
                          actionEntry->reason);
            appendGuestBootLine(kGuestViewLogPath, "fe-driver", ++s_actionOrdinal, 0, diag);
        }
        break;
    }
    }
}

}  // namespace rr3
