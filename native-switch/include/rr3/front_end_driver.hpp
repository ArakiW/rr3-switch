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
