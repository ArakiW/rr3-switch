### 1. Plain-Language Summary of the Driver

The semantic front-end driver replaces brittle, frame-based pad autopilot scripts with a state-aware UI controller driven by the guest game's own screen names and control IDs. On each cycle of the input pump, it inspects the cached screen sample collected by `ui_control_probe` from the guest's screen tree. When an active screen name matches an entry in a static lookup table, the driver performs exactly one action for that screen visit—either tapping a designated button ID (`BTN_CLOSE`, `ACTION_BTN`, `CARD_BUTTON`, `BTN_RACE`) using the validated 40-frame touch state machine, or navigating back to raceable events via `uiScreenOpen("EventMapScreen", true)`. Every decision, target control, coordinate, and missing-control finding is logged to `sdmc:/switch/rr3-native-rehost.guest-view.log` under `phase=fe-driver`. The driver acts once per visit to prevent repeated actuations from walking the guest into unwanted submenus (the v4 NASCAR defect), stays dormant during races, and never touches guest memory from the frame loop.

---

### 2. Complete File Contents

#### [include/rr3/front_end_driver.hpp](file:///E:/AI/RealRacing3-switch/native-switch/include/rr3/front_end_driver.hpp)
```cpp
#pragma once
//
// THE SEMANTIC FRONT-END DRIVER.
//
// Drives the guest's front-end UI by SCREEN NAME and CONTROL ID instead of
// by frame number and blind button presses.
//
// WHY THIS EXISTS
// ---------------
// The port's attract mode was originally driven by a frame-based pad script
// (native-switch/card/rr3-pad-autopilot.txt). That script failed across three
// consecutive rounds with the same root cause:
//
//   m3  Confirm held down -> guest froze on "you received 50 gold" modal
//       because held buttons produce no button-down transition.
//   v3  Confirm pulsed -> modal dismissed and reached race at frame 1830.
//   v4  Confirm pulsed with save data restored -> repeatedly pressed confirm
//       and walked the guest into the NASCAR promo submenu (CareerGroupScreen)
//       with 223 controls, never reaching a raceable event.
//
// Tuning frame numbers cannot fix a 967-feature guest UI. This driver replaces
// frame-based button spam with a state machine that reads the guest's current
// screen name, looks up an action in a static data table, and acts ONCE per
// screen visit.
//
// AND THE FRAME LOOP NEVER TOUCHES GUEST MEMORY
// ---------------------------------------------
// The driver reads only the CACHED UiControlSample and control rectangles
// populated by pumpUiControlProbe, and is pumped from the INPUT PUMP
// (pumpGuestControllerInput) on the guest's own thread.
// ---------------------------------------------------------------------------

#include <cstddef>

namespace rr3 {

// The action to take when visiting a screen.
enum class DriverActionKind {
    None,          // Do nothing on this screen (passive or intermediate view).
    PressControl,  // Tap a specific guest control id_string on this screen.
    OpenScreen,    // Escape/navigate to a target registered screen via uiScreenOpen.
};

// One entry in the front-end screen action table.
struct ScreenAction {
    const char* screen;        // Guest screen name as reported by the registry / sample.
    DriverActionKind action;   // What to do when this screen is active.
    const char* target;        // Control id_string (for PressControl) or target screen name (for OpenScreen).
    const char* reason;        // Explanation citing the failure/measurement addressed.
};

// Returns the registered screen action table and its entry count.
const ScreenAction* frontEndDriverTable(size_t* count);

// Looks up the action for a given screen name. Returns nullptr if not in table.
const ScreenAction* frontEndDriverLookup(const char* screenName);

// Pumps the front-end driver from the input pump (pumpGuestControllerInput).
// Safe to call every frame. jniEnvironment and activityObject default to
// the handles from jni_bridge if null.
void pumpFrontEndDriver(unsigned long long frame,
                        void* jniEnvironment = nullptr,
                        void* activityObject = nullptr);

}  // namespace rr3
```

#### [source/front_end_driver.cpp](file:///E:/AI/RealRacing3-switch/native-switch/source/front_end_driver.cpp)
```cpp
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
```

---

### 3. The One-Line Input Pump Change

In [native-switch/source/guest_input.cpp](file:///E:/AI/RealRacing3-switch/native-switch/source/guest_input.cpp#L610-L618), inside `pumpGuestControllerInput`:

```cpp
    // This is a CALL INTO the guest, so it is pumped from the input pump,
    // which is the guest's own thread, and it installs the guest's TLS.
    pumpUiScreenOpen(g_frameCounter);
    pumpFrontEndDriver(g_frameCounter);

    const bool driving = raceStateDriveEnabled();
```

*(Note: `#include "rr3/front_end_driver.hpp"` is added to the header include list at line 79 of `guest_input.cpp`).*

---

### 4. Table of Screens, Control IDs, and Origins

| Screen Name (`sample.screen`) | Action | Target (`id_string` or Screen) | Source & Origin | Measured Role & Behavior |
| :--- | :--- | :--- | :--- | :--- |
| `FrontEndAwardScreen` | `PressControl` | `BTN_CLOSE` | `.build/eden-runs/...-m3-.../rr3-native-rehost.guest-view.log:78427` (`phase=ui-names ... FrontEndAwardScreen=Front End Award Screen(22)`), `RewardPopup.gui` line 144, Chinese dialog string `\u60a8\u6536\u5230\u4e8650\u91d1\u5e01` in `m3` at frame 5200. | Dismisses the 50-gold modal dialog that froze m3 under a continuous confirm hold. Control is a `GuiButton` anchored `TopRight` at (902..970, 200..248). |
| `RewardPopup` | `PressControl` | `BTN_CLOSE` | `v3` log `phase=map-app rr3core:/files/.depot/gui/processed/RewardPopup.gui` and `RewardPopup.gui`. | Fallback alias when the popup is inspected under its document root name instead of the registry screen name. |
| `AwardsScreen` | `PressControl` | `BTN_CLOSE` | `tools/gui_index.py` query across decrypted `AwardsScreen.gui`. | Dismisses intermediate milestone and level-up award screens during career progression. |
| `EventMapScreen` | `PressControl` | `ACTION_BTN` | `m3`, `v3`, `v4` logs at f=150 (`phase=ui-ctl-br EventMapScreen controls=342 ACTION_BTN(793,359,284,284)`), `CareerBanner_foursquare.gui`. | Advances the guest from the top-level Stream Selection landing screen into the series event cards. |
| `EventsScreen` | `PressControl` | `CARD_BUTTON` | `v3` log at f=1530 (`phase=ui-ctl-br EventsScreen controls=748 CARD_BUTTON(193,200,288,410)`), `Event_card.gui`. | Selects the race event card to proceed to car selection. |
| `CarSelectScreen` | `PressControl` | `BTN_RACE` | `v3` log at f=1620 (`phase=ui-ctl-br CarSelectScreen controls=1200`), `MainMenu.gui` / `RaceStartScreen.gui` (`BTN_RACE` at 1107,697), pad autopilot comment. | Taps the orange race confirmation button, launching into the race track (metrics live at f=1830 in `v3`). |
| `RaceStartScreen` | `PressControl` | `BTN_RACE` | `tools/gui_index.py` query on `RaceStartScreen.gui` (`BTN_RACE`, `GuiButton`). | Confirms start if the race start modal overlay is active. |
| `CareerGroupScreen` | `OpenScreen` | `EventMapScreen` | `v4` log at f=330 (`phase=ui-trees CareerGroupScreen[223 ...]`), `ui_control_probe.cpp` entry `0xA679A0`. | Escapes the NASCAR championship promo submenu (the v4 failure) back to the raceable event map via the guest's registered `EventMapScreen` entry. |
| `CHAMPIONSHIP_ROUND_HUB_SCREEN` | `OpenScreen` | `EventMapScreen` | `m3`/`v3`/`v4` logs `phase=ui-names ... CHAMPIONSHIP_ROUND_HUB_SCREEN(29)`. | Escapes promo championship hub overlays back to Stream Selection. |

---

### 5. Review: Potential Failure Modes and Diagnostic Measurements

1. **Modal Screen Shadowing (Screen Count Inversion):**
   - *Failure:* `ui_control_probe.cpp` selects `sample.screen` by finding the live screen with the largest number of controls (`controls.size() > liveMost`). When a small modal dialog (such as `FrontEndAwardScreen` with 10 controls) is shown over a complex background (such as `EventMapScreen` with 342 controls), `sample.screen` remains `"EventMapScreen"`. The driver looks up `"EventMapScreen"` and tries to actuate `ACTION_BTN` behind the modal dialog instead of pressing `BTN_CLOSE`.
   - *One Measurement:* In `guest-view.log`, `phase=ui-trees` logs `EventMapScreen[342 ...]` and `FrontEndAwardScreen[10 ... BTN_CLOSE ...]` simultaneously, while `phase=fe-driver` logs `screen=EventMapScreen action=press id=ACTION_BTN found=1`.
2. **Dynamic Animation / Transform Clamping:**
   - *Failure:* When transitioning into a screen, the target control exists in the guest's component tree but is animating from outside the screen bounds or has a zero dimension (`w=0`, `h=0`) during its opening interpolation. The driver flags `why=out-of-view` or `why=control-not-found` and defers actuation. If an animation freezes or never brings the control within view, the driver stalls.
   - *One Measurement:* In `guest-view.log`, `phase=fe-driver` logs `action=press id=ACTION_BTN found=1 why=out-of-view x=... y=... view=1280x720` repeatedly on consecutive walk cadences with identical coordinates.
3. **Registry OpenScreen Rejection (`allowOpen` failure):**
   - *Failure:* On `CareerGroupScreen`, the driver invokes `uiScreenOpen("EventMapScreen", true)`. If the guest's screen manager enforces a modal precondition or an invalid state flag in its bookkeeping (`ELF 0x14a44b8`), `OpenScreen` returns false or aborts the transition.
   - *One Measurement:* In `guest-view.log`, `phase=fe-driver` logs `screen=CareerGroupScreen action=open target=EventMapScreen status=rejected`, followed on the next probe sample by `screen=CareerGroupScreen` still active.
4. **Initial Stale Touch Pointer Swallowing:**
   - *Failure:* The guest initializes its touch state with an internal pointer marked down at `(-1, -1)`. If `Tapper` attempts to inject `onTouchBeginJNI` without first emitting the stale release event (`onTouchEndJNI` at -1, -1), the guest drops the first tap.
   - *One Measurement:* In `guest-view.log`, `phase=fe-driver` logs `action=press ... hold=40`, but the subsequent `phase=ui-ctl` line shows no change in screen or control tree hierarchy across 300+ frames.

---

### 6. The Single Most Likely Failure Mode and Its Diagnostic Measurement

- **The Single Most Likely Flaw:**
  **Modal Dialog Masking by `liveMost`:**
  In `native-switch/source/ui_control_probe.cpp:1012-1015`, `sample.screen` is assigned to whichever live screen has the *highest control count*:
  ```cpp
  if (static_cast<int>(controls.size()) > liveMost) {
      liveMost = static_cast<int>(controls.size());
      std::memcpy(liveName, screens[i].name, sizeof(liveName));
  }
  ```
  A modal popup like `FrontEndAwardScreen` (the 50-gold popup from m3) contains roughly 10 widgets, whereas the underlying `EventMapScreen` contains 342 widgets. As a result, `sample.screen` will report `"EventMapScreen"`, completely masking the presence of `FrontEndAwardScreen`. The driver will look up `"EventMapScreen"`, attempt to actuate `"ACTION_BTN"` (which is either blocked by the modal backdrop or swallowed), and will never look up `"FrontEndAwardScreen"` to press `"BTN_CLOSE"`.

- **The Single Diagnostic Measurement:**
  In `sdmc:/switch/rr3-native-rehost.guest-view.log`, a line with `phase=ui-trees` containing `FrontEndAwardScreen[... BTN_CLOSE ...]` (or `phase=jni-utf163` showing the Chinese reward string `\u60a8\u6536\u5230\u4e8650\u91d1\u5e01`), while `phase=fe-driver` logs `screen=EventMapScreen action=press id=ACTION_BTN found=1`, followed by the run remaining permanently stuck on that screen.
