#pragma once
//
// Touch injection into the rehosted guest.
//
// WHY THIS EXISTS. The guest's front end draws a real UI (a track scene, a
// status bar, a modal panel with a button) and the guest's OWN attract mode
// drives it with touches -- the archived guest-view log carries
// "Selecting component: MAP_EVENT_CARD_BTN", "MenuSelectHit: PRESS",
// "TouchStart: x = 1093 y = 549". The port had no way to send one: the only
// input path mapped libnx buttons to ANDROID KEY/AXIS events.
//
// THE GUEST'S TOUCH PATH (the ORACLE is libRealRacing3.so + the decompiled
// guest Java, not this comment):
//
//   GLView.onTouchEvent(MotionEvent)               guest Java
//     -> GLView.TouchEvent.run()                    guest Java, GL thread
//       -> MainActivity.onTouchBeginJNI(id,x,y)     guest native, ELF 0x12b39d8
//       -> MainActivity.onTouchMoveJNI(id,x,y)      guest native, ELF 0x12b3a14
//       -> MainActivity.onTouchEndJNI(id,x,y,last)  guest native, ELF 0x12b3a58
//       -> MainActivity.onTouchCancelJNI()          guest native, ELF 0x12b3ac0
//
// The four JNI exports are reached through the SAME mechanism the port
// already uses for the controller (SetButtonValueJNI): resolveMainImageExport()
// on the guest image, then call the export with the guest's JNIEnv. The guest
// never sees a pointer poke -- the event goes in through the entry point the
// guest's own Java uses.
//
// COORDINATES. The JNI exports truncate the float coordinates to int with
// FCVTZS and hand (engine, pointerId, x, y) to the engine's touch handler,
// so the unit is VIEW PIXELS in the 1280x720 surface the port reports through
// onViewChangedJNI -- the same space as the framebuffer PNGs and the same
// space the guest's own attract mode logs.
//
// THE CONTROL. OFF BY DEFAULT: with no file on the card this code never
// touches the guest's touch path, so a normal run is unchanged.
//
//   sdmc:/switch/rr3-native-rehost/rr3-touch.txt
//
// One event per line:
//
//     [at <frame>] <x> <y> <action> [pointerId]
//
//   x, y      view pixels (0..1279, 0..719)
//   action    down | move | up
//   pointerId optional, default 0. The guest compares the pointer id between
//             the down and the move/end, so a pair must use the same one.
//   at <frame> optional. The event waits until the port's frame counter
//             reaches <frame> before it is injected. The port's frame
//             counter is the guest's frame counter to within one (the frame
//             loop pumps input once per iteration, just before the guest's
//             render call). This is what makes a script REPRODUCIBLE with no
//             live host->port channel: the whole script can be on the card
//             before the run starts and still land on the frame it names.
//
// The file is APPEND-ONLY: the host appends lines, the port injects each new
// line exactly once. Blank lines and lines starting with '#' are comments.
//
// The host reaches the file at
//   E:\Eden\user\sdmc\switch\rr3-native-rehost\rr3-touch.txt
// on a default Eden install, which is what makes it scriptable while the
// emulator is running.
//
namespace rr3 {

// Resolves the guest's four touch JNI exports. Returns false when the guest
// image does not export them (the touch path is then unavailable and every
// later call is a no-op).
bool prepareGuestTouchInput();

// True when the touch exports resolved.
bool guestTouchInputReady();

// Called once per frame from the port's frame loop, on the same thread that
// calls the guest's render entry point -- which is the same relationship the
// guest's own GLView.queueEvent() has on Android. Reads the control file,
// injects at most ONE queued event, and logs what it injected.
void pumpGuestTouchInput(void* jniEnvironment, void* activityObject);

// The control file's path, for logging and for tests.
const char* guestTouchControlPath();

}  // namespace rr3
