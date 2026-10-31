#pragma once
//
// THE SEMANTIC UI PROBE: the guest's OWN screen tree, read by control id.
//
// WHY THIS EXISTS
// ---------------
// The port's UI driver finds buttons by their COLOURS (the guest's red glove,
// the orange accent button).  That works, but it cannot say WHAT it clicked,
// it cannot click a button that is not accented, and it cannot reach the
// game's meta screens at all: a full run opens 58 of the guest's 967 UI
// features (docs/FEATURE-MATRIX.md) -- no bank, no garage customisation, no
// crew, no community, no friends, no leaderboard, no multiplayer.
//
// The guest NAMES every control: the .gui files are the screens' definitions
// and their elements carry the ids the guest's own code looks up ("BTN_RACE_NOW",
// "BTN_CONTINUE", "BTN_OK", ...).  So the port can ask the guest WHERE a button
// is instead of guessing with pixels.
//
// THE .gui FILES ARE ENCRYPTED, AND THE CIPHER IS THE ONE ALREADY REVERSED.
// Each .gui is a stream-ciphered XML document, using the guest's RC4 variant
// with the SAME key as the device profiles (a535b3b1e843e7cf).  Decrypted,
// all 939 of them are well-formed XML with an `id_string` on every widget.
// tools/gui_index.py reads them; that tool is the source of the ID VOCABULARY
// this probe is checked against.  (Measured 2026-10-31: 939 files, 0 failures,
// 4,729 distinct id_strings, 11,936 widget elements.)
//
// THE MEASURED LAYOUT (all of it read out of the guest binary)
// -----------------------------------------------------------
//   G                      the CGlobal (race_state_probe's raceStateResolveGlobal)
//   G + 0x17f8             the MainMenuManager
//   mgr + 0xa8             the screen registry's root (a tree; 65 screens)
//   node + 0x20            a registry node's NAME, a 24-byte libc++ string
//   node + 0x38            a registry node's screen INSTANCE
//   comp + 0x34            a component's numeric id
//   comp + 0x38            a component's NAME, a 24-byte libc++ string
//   comp + 0x138           a component's CHILDREN container
//   comp + 0x160           the component's AABB
//   comp + 0x180/0x184     a component's rectangle: x, y
//   comp + 0x188/0x18c     ... and x+w, y+h
//   G + 0x70               the guest's viewport (4 x u32), used when G+0xd20 == 1
//
// (An earlier draft of this file claimed comp+0x140 was a SIBLING link.  The
// guest's own find-by-id vtable slot walks a CONTAINER at +0x138 instead, so
// +0x140 is that container's end pointer, not a sibling.  Trust the slot.)
//
// EVERY POINTER IS ACCEPTED ONLY IF IT PROVES ITSELF
// ---------------------------------------------------
// A pointer is taken as a component ONLY when the name at +0x38 reads back as
// a plausible id: 1..63 characters, all of them [A-Za-z0-9_].  So a wrong
// offset yields NO control instead of a crash, and the probe reports what it
// found rather than what it assumed.
//
// AND THE FRAME LOOP NEVER TOUCHES GUEST MEMORY
// ---------------------------------------------
// The probe is pumped from the INPUT PUMP (the same place the race-state
// probe is pumped, and for the same reason: calling into the guest's memory
// from the frame loop froze the run twice, measured in q23 and q24).  The
// frame loop reads the CACHED sample and nothing else.
// ---------------------------------------------------------------------------
#include <cstdint>

namespace rr3 {

// One control, as the guest's own screen tree holds it.
struct UiControl {
    char name[64];
    int x{};
    int y{};
    int w{};
    int h{};
};

// What one sample found.
struct UiControlSample {
    bool ok{};
    unsigned long long frame{};
    // The screen's own name, when one was found.
    char screen[64]{};
    // How many named controls the walk found, and where they are.
    //
    // The controls are on the HEAP, and the pointer is owned by the probe:
    // it stays valid until the next pump.  A screen carries hundreds of
    // controls -- measured 342 on EventMapScreen and 452 on CarSelectScreen
    // -- so a fixed array would truncate the list exactly where the
    // interesting buttons are, and a truncated list is a silent failure.
    int count{};
    const UiControl* items{};
    // The guest's viewport, as the probe read it.  The rectangles are in this
    // space: measured, BTN_LEFT_CAR's runtime rectangle is (16,513,52,54) at
    // a 1280x720 view, and the .gui file's own transform (0.0122839,
    // 0.7125) times the view size gives (15.7, 513) -- the same place.
    int viewportW{};
    int viewportH{};
    // How many screens the guest's registry holds, and how many of them had a
    // live component tree.  A screen with a live tree is the one on top.
    int screensRegistered{};
    int screensLive{};
};

// Samples the guest's screen tree.
//
// Call it from the INPUT PUMP.  It is safe to call every frame: the walk
// runs on a CADENCE, not every frame, because the walk touches a lot of
// guest pages and the project has already measured that a probe spending
// ~1500 svcQueryMemory calls per sample drops the guest's frame rate.
void pumpUiControlProbe(unsigned long long frame);

// The most recent sample. Never null.
const UiControlSample& uiControlSample();

// Finds a control by its id (the name the guest's .gui files use) and reports
// its rectangle in the guest's own layout units.
//
// FALSE when the probe has not found that control.  This is the entry point
// the UI driver uses: it asks for a control BY NAME and gets a rectangle, and
// when the answer is no it falls back to the colour detector.
bool uiControlFind(const char* name, int* x, int* y, int* w, int* h);

// The next control the card's rr3-ui.txt asks to be pressed.
//
// THE FILE.  One request per line, the same shape as the other card-side
// control files:
//
//     # a comment
//     at=6000 BTN_RACE_NOW      ; not before frame 6000
//     BTN_CONTINUE              ; as soon as it exists
//
// The name is the control id the guest itself uses ("BTN_RACE_NOW"), which
// is the vocabulary the .gui files declare and the guest's binary contains.
//
// WHY IT IS A FILE AND NOT A POLICY.  The colour driver decides on its own
// what to press, which is why it pressed the same decoration 45 times in a
// row.  A semantic driver that decides on its own would make the same class
// of mistake with better eyesight.  So the semantic driver only ever presses
// what the card NAMED, and the card is a plain text file a human wrote.
//
// A request is served ONCE: the port calls uiControlServeRequest() after it
// has actually pressed the control.  A request whose control does not exist
// yet is simply not returned, and the probe keeps looking.
//
// FALSE when nothing is due, or when the named control is not on screen.
//
// The frame counter is the SAME one rr3-action.txt's "at=" uses, so the two
// card-side files mean the same thing by "frame".
bool uiControlNextRequest(UiControl* out);

// Marks a request as done, after the port has pressed it.
void uiControlServeRequest(const char* name);

// ---------------------------------------------------------------------------
// OPENING A SCREEN BY ITS NAME
//
// THE RECIPE (measured in the guest's own binary, and re-derived
// independently by a second pass):
//
//   0x14a4820  FrontEnd2::Manager::OpenScreen(Screen* screen, int flags)
//       0x14a4858  ldr  w8, [x0, #0x3b8]        ; the screen stack's depth
//       0x14a4864  mov  w9, #0x30
//       0x14a4868  umaddl x8, w8, w9, x19      ; mgr + (depth-1)*0x30
//       0x14a486c  ldr  x8, [x8, #0xb8]        ; the screen on TOP
//       0x14a4870  cmp  x8, x21                 ; already the one we want?
//       0x14a4874  b.eq  ...                   ; then there is nothing to do
//       0x14a4880  bl   0x10bb9c0              ; (screen, mgr) bookkeeping
//       0x14a488c  bl   0x14a44b8              ; (mgr, screen) bookkeeping
//       0x14a4890  and  w8, w20, #1             ; flags & 1
//       0x14a4898  mov  x1, sp                  ; the DESCRIPTOR
//       0x14a48a0  str  wzr, [sp]              ; kind  = 0
//       0x14a48a4  str  x21, [sp, #8]          ; the screen instance
//       0x14a48a8  strb w8, [sp, #0x10]        ; flags & 1
//       0x14a48ac  str  w9, [sp, #0x14]        ; 1
//       0x14a48b0  bl   0x14a48ec              ; the switch itself
//
// The descriptor is 24 bytes: {u32 kind; u32 pad; u64 screen; u8 flag; u32 one}.
//
// The screen INSTANCE is already known by name: the guest's registry holds 59
// screens, and the port walks it to find one.  So the port can open any of
// them -- CarPurchaseScreen, CarCustomisationScreen, UpgradesScreen,
// CommunityScreen, MyGarageScreen -- without hunting for pixels.
//
// THE CALL IS A CALL INTO GUEST CODE, so it happens on the guest's own
// thread (the input pump is inside the frame loop, which IS the guest's
// thread) and with the guest's TLS installed, exactly like the port's
// existing named-command mechanism in source/guest_action.cpp.
//
// Returns TRUE when the screen was found and the call was made.  A screen
// that is not in the registry is reported, not guessed at.
//
// allowOpen GATES THE DANGEROUS HALF.  MEASURED 2026-10-31 (q51): the
// build step works -- UpgradesScreen's XML loader produced a component tree
// -- and OpenScreen was called, and the guest DIED.  So the open is not
// proven, and a capability that can take the guest down must not be one
// keystroke away: with allowOpen false the port BUILDS the screen and
// VERIFIES the tree and stops there, and only the card's "open=1" line
// turns the open on.
bool uiScreenOpen(const char* name, bool allowOpen = false);

// Serves the card's rr3-open.txt: one registered screen name per line, and
// the port opens them one at a time, in order, when the front end is up.
//
// Nothing is active by default: the repository's rr3-open.txt is all
// comments.
void pumpUiScreenOpen(unsigned long long frame);

}  // namespace rr3
