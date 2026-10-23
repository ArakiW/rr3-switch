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

// ---- the shared injection primitive -----------------------------------------
//
// The file control and the hardware touch path go through ONE function, so
// both reach the guest through the same entry points, with the same argument
// order and the same log line. Two call sites that each look right is how a
// port ends up with a touch path that works from a script and not from a
// finger.

// What the guest has to be told, in the guest's own terms.
enum class GuestTouchAction {
    Begin,    // onTouchBeginJNI(id, x, y)
    Move,     // onTouchMoveJNI(id, x, y)
    End,      // onTouchEndJNI(id, x, y, last=true)
    Release,  // onTouchEndJNI(id, x, y, last=false)
};

// The guest's own touch state, read back from the guest's touch object.
//
// "down" is the byte the guest's touch object SHIPS with set to 1 and with
// x=y=-1, and the guest's engine stores a new point only when no finger is
// down -- so a press that arrives while this says down=1 is swallowed. Read
// it before pressing and release that pointer first.
struct GuestTouchState {
    bool valid;      // the guest's touch object could be read at all
    bool down;       // the guest believes a finger is down
    int pointerId;   // the pointer the guest believes is down
    int x;
    int y;
    unsigned paused; // the guest's paused gate (engine + 0x4d0)
    unsigned focus;  // the guest's focus gate (engine + 0x4d1)
};

// Reads the guest's own touch state. Returns an all-zero state when the
// guest's touch object cannot be read.
GuestTouchState readGuestTouchState();

// Injects one event into the guest's touch path and logs it.
//
// `phase` is the log line's phase name: "touch-inject" for the file control,
// "touch-hw-inject" for the hardware path. The detail format is the same for
// both, so one reader understands both.
void injectGuestTouchEvent(void* jniEnvironment, void* activityObject,
                           GuestTouchAction action, int pointerId, int x, int y,
                           bool last, const char* phase);

// True while the file control's control file is on the card. The file
// control owns the guest's touch path while it is, and the hardware path
// stands down. Presence is sampled by the file control's own poll, which
// reaches the card every third frame; until that first poll has happened,
// guestTouchControlHasPolled() is false and the hardware path stands down
// too.
bool guestTouchControlFilePresent();

// True while the file control has events queued that have not been injected
// yet (due or not).
bool guestTouchControlHasPending();

// True in the frame in which the file control injected an event. The
// hardware path is pumped right after the file control's, in the same frame,
// so this is what keeps the two from putting two pointers down at once.
bool guestTouchControlInjectedThisFrame();

// True once the file control has looked at the card at least once. Until
// then it cannot say whether a control file is there, and the hardware path
// stands down rather than guess.
bool guestTouchControlHasPolled();

}  // namespace rr3
