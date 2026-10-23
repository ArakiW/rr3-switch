#include "rr3/guest_touch.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/recovery_guard.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>

namespace rr3 {
namespace {

// ---- the guest's touch entry points ----------------------------------------
//
// The addresses below were read out of the GUEST IMAGE's .dynsym, not out of
// the Java. The Java proves WHICH method the guest calls; only the image
// proves WHERE it lives.
//
//   Java_com_firemint_realracing_MainActivity_onTouchBeginJNI  ELF 0x12b39d8
//   Java_com_firemint_realracing_MainActivity_onTouchMoveJNI   ELF 0x12b3a14
//   Java_com_firemint_realracing_MainActivity_onTouchEndJNI    ELF 0x12b3a58
//   Java_com_firemint_realracing_MainActivity_onTouchCancelJNI ELF 0x12b3ac0
//
// Each export reads the engine singleton out of the GOT slot at ELF
// 0x1f6e540, checks two gate bytes on it (engine+0x4d0 "not paused",
// engine+0x4d1 "has focus"), truncates the float coordinates to int with
// FCVTZS, and tail calls the engine's touch handler with
// (engine, pointerId, x, y). Neither the JNIEnv nor the jobject is read, so
// the port can pass the same pair it passes to SetButtonValueJNI.
//
// The engine's handlers then store the point in the engine object
// (pointer id at +0x14, x at +0x18, y at +0x1c, "a finger is down" at +0x20)
// and hand the event to the menu layer, which is where the guest's own
// "Selecting component: ..." / "MenuSelectHit: PRESS" lines come from.
constexpr unsigned long long kGuestOnTouchBeginVa = 0x12b39d8ULL;
constexpr unsigned long long kGuestOnTouchMoveVa = 0x12b3a14ULL;
constexpr unsigned long long kGuestOnTouchEndVa = 0x12b3a58ULL;

// The read-back instrument.
//
// onTouchBeginJNI loads the engine object out of the GOT slot at ELF
// 0x1f6e540, and the engine's touch handler (ELF 0x12b45f4) writes the
// point into the TOUCH object the engine points at through engine+0x58:
//
//   stp w22, w21, [x20, #0x14]   ; pointer id, x
//   str w19,     [x20, #0x1c]    ; y
//   strb w8,     [x20, #0x20]    ; "a finger is down"
//
// and the engine's touch MOVE/END handlers match the pointer id against
// engine-touch-object+0x14 before they touch anything.
//
// So the guest's OWN touch state can be read back after an injection: if it
// holds the coordinates that were injected, the guest received and consumed
// the event. That is a stronger statement than "the port called a function",
// and it does not depend on the guest's menu layer being awake.
//
// The GOT slot is reached through the onViewRenderJNI export, because that
// export reads the SAME slot:
//   0x12b3614: adrp x8, #0x1f6e000
//   0x12b3618: ldr  x8, [x8, #0x540]   <- slot at ELVVA 0x1f6e540
// so the delta from the export to the slot is 0x1f6e540 - 0x12b3614 =
// 0x00CBAF2C (computed, not hand-added: two hand-added versions of this one
// subtraction were both wrong before it was computed).
constexpr unsigned long long kTouchSingletonSlotDelta = 0x00CBAF2CULL;
// The touch object's own layout, from the engine's touch handler.
constexpr uintptr_t kTouchObjEnabled = 0x0a;
constexpr uintptr_t kTouchObjPointerId = 0x14;
constexpr uintptr_t kTouchObjX = 0x18;
constexpr uintptr_t kTouchObjY = 0x1c;
constexpr uintptr_t kTouchObjDown = 0x20;
// The engine object's layout: the two gate bytes the touch JNI export checks,
// and the pointer to the touch object.
constexpr uintptr_t kEnginePaused = 0x4d0;
constexpr uintptr_t kEngineFocus = 0x4d1;
constexpr uintptr_t kEngineTouchObject = 0x58;

// A guest pointer worth dereferencing: non-zero, above the first page and
// inside the 48-bit user address space. Same gate guest_view_probe.cpp uses.
constexpr uintptr_t kTouchPlausibleLo = 0x10000ULL;
constexpr uintptr_t kTouchPlausibleHi = 0x0000800000000000ULL;

bool touchPointerPlausible(uintptr_t address) {
    return address >= kTouchPlausibleLo && address < kTouchPlausibleHi;
}

// Is this address mapped and readable RIGHT NOW?
//
// The same question, with the same answer, as guest_view_probe.cpp's
// dlMemoryReadable(): ask the kernel with svcQueryMemory and check Perm_R.
// A pointer the guest published can point at a page that is no longer there
// (the guest frees heap objects), and a diagnostic that can crash the run it
// is measuring is worse than no diagnostic.
bool touchMemoryReadable(uintptr_t address) {
    constexpr uintptr_t kPageMask = ~static_cast<uintptr_t>(0xFFF);
    const uintptr_t page = address & kPageMask;
    // A FOUR-entry cache, not a one-entry one, because the per-frame touch
    // path reads three distinct pages -- the activity holder, the engine
    // object and the touch object -- in a fixed order. A one-entry cache
    // misses on every one of them, so every frame paid three svcQueryMemory
    // calls; four entries hold all three and a warm frame loop pays none.
    static uintptr_t s_cachedPage[4] = {0, 0, 0, 0};
    static bool s_cachedReadable[4] = {false, false, false, false};
    static bool s_cachedValid[4] = {false, false, false, false};
    for (int i = 0; i < 4; ++i) {
        if (s_cachedValid[i] && page == s_cachedPage[i]) {
            return s_cachedReadable[i];
        }
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    bool readable = false;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, static_cast<u64>(address)))) {
        readable = false;
    } else {
        readable = (info.perm & Perm_R) != 0;
    }
    int slot = 0;
    for (int i = 1; i < 4; ++i) {
        if (!s_cachedValid[i]) {
            slot = i;
            break;
        }
    }
    s_cachedPage[slot] = page;
    s_cachedReadable[slot] = readable;
    s_cachedValid[slot] = true;
    return readable;
}

bool touchRangeReadable(uintptr_t address, size_t length) {
    if (length == 0) {
        return true;
    }
    if (!touchMemoryReadable(address)) {
        return false;
    }
    return touchMemoryReadable(address + length - 1);
}

// A word out of guest memory, or "not readable". A refused read is reported
// as unreadable and NEVER as a value, so a skipped read can never be
// mistaken for a reading.
bool touchReadWord(uintptr_t address, uint64_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (guestImageContains(address, sizeof(uint64_t))) {
        *out = *reinterpret_cast<const volatile uint64_t*>(address);
        return true;
    }
    if (!touchPointerPlausible(address) ||
        !touchRangeReadable(address, sizeof(uint64_t))) {
        return false;
    }
    *out = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
}

bool touchReadByte(uintptr_t address, uint8_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, 1)) {
        if (!touchPointerPlausible(address) || !touchMemoryReadable(address)) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint8_t*>(address);
    return true;
}

// A 32-bit word out of guest memory, or "not readable". The touch object's
// coordinates are 32-bit (the guest writes them with STP W / STR W).
bool touchReadDword(uintptr_t address, uint32_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint32_t))) {
        if (!touchPointerPlausible(address) ||
            !touchRangeReadable(address, sizeof(uint32_t))) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint32_t*>(address);
    return true;
}

// The guest's own touch state, read back.
struct TouchState {
    bool valid;
    bool engineReadable;
    bool objectReadable;
    uintptr_t engine;
    uintptr_t touchObject;
    unsigned paused;
    unsigned focus;
    unsigned enabled;
    int pointerId;
    int x;
    int y;
    unsigned down;
};

// The engine object behind the GOT slot, or 0 when it cannot be read.
//
// TWO dereferences, and the second one is the one that matters.
//
//   0x12b39d8: adrp x8, #0x1f6e000
//   0x12b39dc: ldr  x8, [x8, #0x540]   <- x8 = *(0x1f6e540)  ("holder")
//   0x12b39e0: ldr  x8, [x8]           <- x8 = *holder      (the activity)
//   0x12b39e8: ldrb w9, [x8, #0x4d0]   <- the paused flag
//
// and onWindowFocusChangedJNI (0x12b3508) does the same two steps and
// then writes the focus flag at activity+0x4d1. Reading only ONE level
// gives the holder, whose +0x4d0/+0x4d1 are not those flags -- which is
// why an earlier version of this read-back reported focus=0 while the
// guest demonstrably had focus.
uintptr_t readEngineObject() {
    static uintptr_t renderExport = 0;
    if (renderExport == 0) {
        renderExport = resolveMainImageExport(
            "Java_com_firemint_realracing_MainActivity_onViewRenderJNI");
        if (renderExport == 0) {
            return 0;
        }
    }
    // The slot is inside the guest image, so it is mapped for the whole run.
    const uintptr_t slot = renderExport + kTouchSingletonSlotDelta;
    if (!guestImageContains(slot, sizeof(uint64_t))) {
        return 0;
    }
    uint64_t holder = 0;
    if (!touchReadWord(slot, &holder) || holder == 0) {
        return 0;
    }
    uint64_t activityWord = 0;
    if (!touchReadWord(static_cast<uintptr_t>(holder), &activityWord) ||
        activityWord == 0) {
        return 0;
    }
    return static_cast<uintptr_t>(activityWord);
}

TouchState readTouchState() {
    TouchState state{};
    const uintptr_t engine = readEngineObject();
    if (engine == 0) {
        return state;
    }
    state.engine = engine;
    state.engineReadable = true;

    uint8_t paused = 0;
    uint8_t focus = 0;
    if (!touchReadByte(state.engine + kEnginePaused, &paused)) {
        return state;
    }
    if (!touchReadByte(state.engine + kEngineFocus, &focus)) {
        return state;
    }
    state.paused = paused;
    state.focus = focus;

    uint64_t touchObject = 0;
    if (!touchReadWord(state.engine + kEngineTouchObject, &touchObject) ||
        touchObject == 0) {
        return state;
    }
    state.touchObject = static_cast<uintptr_t>(touchObject);
    state.objectReadable = true;

    uint8_t enabled = 0;
    uint8_t down = 0;
    uint32_t pointerId = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    if (!touchReadByte(state.touchObject + kTouchObjEnabled, &enabled)) {
        return state;
    }
    if (!touchReadByte(state.touchObject + kTouchObjDown, &down)) {
        return state;
    }
    if (!touchReadDword(state.touchObject + kTouchObjPointerId, &pointerId)) {
        return state;
    }
    if (!touchReadDword(state.touchObject + kTouchObjX, &x)) {
        return state;
    }
    if (!touchReadDword(state.touchObject + kTouchObjY, &y)) {
        return state;
    }
    state.enabled = enabled;
    state.down = down;
    state.pointerId = static_cast<int>(pointerId);
    state.x = static_cast<int>(x);
    state.y = static_cast<int>(y);
    state.valid = true;
    return state;
}

constexpr const char* kTouchControlPath =
    "sdmc:/switch/rr3-native-rehost/rr3-touch.txt";
constexpr const char* kTouchLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// Java: void onTouchBeginJNI(int id, float x, float y)
using TouchBeginFn = void (*)(void* env, void* self, int id, float x, float y);
// Java: void onTouchMoveJNI(int id, float x, float y)
using TouchMoveFn = void (*)(void* env, void* self, int id, float x, float y);
// Java: void onTouchEndJNI(int id, float x, float y, boolean last)
using TouchEndFn = void (*)(void* env, void* self, int id, float x, float y,
                            unsigned char last);
// Java: void onTouchCancelJNI()
using TouchCancelFn = void (*)(void* env, void* self);

TouchBeginFn g_begin = nullptr;
TouchMoveFn g_move = nullptr;
TouchEndFn g_end = nullptr;
TouchCancelFn g_cancel = nullptr;
bool g_ready = false;
bool g_armed = false;

// ---- the control file -------------------------------------------------------

constexpr int kActionDown = 0;
constexpr int kActionMove = 1;
constexpr int kActionUp = 2;

struct TouchEvent {
    int x;
    int y;
    int action;
    int id;
    // The pump counter this event becomes due at, or -1 for "as soon as it
    // is read". The pump counter is the port's own frame counter: the frame
    // loop calls pumpGuestControllerInput once per iteration, before the
    // guest's render call, so it tracks the guest's frame number to within
    // one.
    int dueAt;
};

// The queue is deliberately small and fixed: a control file is a script, not
// a data stream, and a bounded queue cannot be made to allocate on the frame
// loop.
constexpr int kQueueCapacity = 64;
TouchEvent g_queue[kQueueCapacity];
int g_queueHead = 0;
int g_queueTail = 0;

// How often the control file is read. NOT every frame: the card is emulated
// and every read is a card access on the frame-loop thread.
constexpr unsigned kControlPollEveryFrames = 3;
// How many polls to wait before looking for the control file again when it
// is not there. The file is OPTIONAL, so a failed open is the normal case and
// must not cost a card access on every poll.
constexpr unsigned kControlRetryEveryPolls = 5;
// Bytes read per poll. A control file is a script; this is a safety cap, not
// a real limit.
constexpr size_t kControlReadCap = 64 * 1024;
// The head of the file, kept so a REWRITTEN file can be told from an
// APPENDED one. A rewrite that keeps the same first bytes is
// indistinguishable from an append -- and for the first bytes it is one.
constexpr size_t kControlSignatureBytes = 64;

unsigned g_controlPolls = 0;
unsigned g_controlMisses = 0;
size_t g_controlConsumed = 0;
unsigned char g_controlSignature[kControlSignatureBytes] = {0};
bool g_controlHaveSignature = false;
// Whether the control file is there at the last poll. The hardware touch
// path stands down while it is: the file control owns the guest's touch path
// while its script is on the card.
bool g_controlFilePresent = false;
// Whether the file control injected an event in the frame that is being
// pumped RIGHT NOW. The hardware path is pumped right after the file
// control's, in the same frame, so without this the file control could
// inject a press and the hardware path could inject a second one in the
// same frame.
bool g_controlInjectedThisFrame = false;
// Whether the file control has looked at the card at least once. Until it
// has, it cannot say whether a control file is there, and the hardware touch
// path -- which is pumped right after it, in the same frame -- stands down.
// MEASURED: the file control's first poll is on the third pump, and without
// this a synthetic finger injected on pump 2 reached the guest while a
// control file was on the card.
bool g_controlPolledOnce = false;
// The port's frame counter. The frame loop pumps input once per iteration,
// so this counts the guest's frames to within one.
unsigned g_pumpCount = 0;
// How often the guest's own touch state is read back and logged. Cheap (one
// svcQueryMemory per read) and it is the instrument that says whether the
// guest's touch path is awake at all.
constexpr unsigned kStateProbeEveryPumps = 300;

bool queueEvent(const TouchEvent& event) {
    const int next = (g_queueTail + 1) % kQueueCapacity;
    if (next == g_queueHead) {
        return false;
    }
    g_queue[g_queueTail] = event;
    g_queueTail = next;
    return true;
}

const char* actionName(int action) {
    switch (action) {
    case kActionDown:
        return "down";
    case kActionMove:
        return "move";
    case kActionUp:
        return "up";
    default:
        return "?";
    }
}

bool parseAction(const char* text, int* action) {
    if (std::strcmp(text, "down") == 0) {
        *action = kActionDown;
        return true;
    }
    if (std::strcmp(text, "move") == 0) {
        *action = kActionMove;
        return true;
    }
    if (std::strcmp(text, "up") == 0) {
        *action = kActionUp;
        return true;
    }
    return false;
}

void logControl(const char* phase, const char* details) {
    appendGuestBootLine(kTouchLogPath, phase, 940, 0, details);
}

// Parses one control line. Returns false for a line that is not a usable
// event; a malformed line is logged and skipped rather than silently
// dropped, because a script that writes a typo would otherwise look like a
// port that ignores touches.
//
// GRAMMAR:
//
//     [at <frame>] <x> <y> <down|move|up> [pointerId]
//
// The optional "at <frame>" makes the event wait for the port's frame
// counter to reach <frame> before it is injected. It is what makes the
// control reproducible WITHOUT a live host->port channel: the whole script
// can be written to the card before the run starts and still land on the
// frame it names.
bool parseLine(const char* line, TouchEvent* event, char* why,
               size_t whyCapacity) {
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
        ++line;
    }
    if (line[0] == '\0' || line[0] == '#') {
        return false;
    }
    int dueAt = -1;
    if (line[0] == 'a' && line[1] == 't' &&
        (line[2] == ' ' || line[2] == '\t')) {
        const char* rest = line + 2;
        int parsed = 0;
        if (std::sscanf(rest, " %d", &parsed) != 1 || parsed < 0) {
            std::snprintf(why, whyCapacity, "'at' needs a frame number");
            return false;
        }
        dueAt = parsed;
        while (*rest == ' ' || *rest == '\t') {
            ++rest;
        }
        while (*rest != '\0' && *rest != ' ' && *rest != '\t') {
            ++rest;
        }
        line = rest;
    }
    int x = 0;
    int y = 0;
    char action[16] = {0};
    int id = 0;
    const int fields = std::sscanf(line, "%d %d %15s %d", &x, &y, action, &id);
    if (fields < 3) {
        std::snprintf(why, whyCapacity, "expected '<x> <y> <down|move|up>'");
        return false;
    }
    int actionCode = 0;
    if (!parseAction(action, &actionCode)) {
        std::snprintf(why, whyCapacity, "unknown action '%s'", action);
        return false;
    }
    event->x = x;
    event->y = y;
    event->action = actionCode;
    event->id = id;
    event->dueAt = dueAt;
    return true;
}

// Reads whatever has been appended to the control file since the last poll
// and queues the events it finds.
//
// The file is APPEND-ONLY: the host appends lines, the port injects every
// line exactly once. The file is REOPENED on every poll rather than held
// open, because the host may delete and recreate it (a delete/recreate is a
// new file, and a held handle would keep reading the old one).
void drainControlFile() {
    g_controlPolledOnce = true;
    std::FILE* file = std::fopen(kTouchControlPath, "rb");
    if (file == nullptr) {
        g_controlMisses++;
        g_controlFilePresent = false;
        return;
    }
    g_controlFilePresent = true;
    if (!g_armed) {
        g_armed = true;
        char detail[320];
        std::snprintf(detail, sizeof(detail),
                      "path=%s begin=%llx move=%llx end=%llx cancel=%llx",
                      kTouchControlPath,
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(g_begin)),
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(g_move)),
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(g_end)),
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(g_cancel)));
        logControl("touch-control-armed", detail);
    }
    g_controlMisses = 0;

    // The head of the file decides whether this is an append or a rewrite.
    unsigned char signature[kControlSignatureBytes] = {0};
    const size_t signatureRead =
        std::fread(signature, 1, sizeof(signature), file);
    if (g_controlHaveSignature && signatureRead > 0 &&
        std::memcmp(signature, g_controlSignature, signatureRead) != 0) {
        logControl("touch-control-rewritten", "control file replaced");
        g_controlConsumed = 0;
    }
    if (signatureRead > 0) {
        std::memcpy(g_controlSignature, signature, signatureRead);
        g_controlHaveSignature = true;
    }

    if (std::fseek(file, static_cast<long>(g_controlConsumed), SEEK_SET) != 0) {
        std::fclose(file);
        return;
    }
    char buffer[kControlReadCap];
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (read == 0) {
        return;
    }
    buffer[read] = '\0';

    // Only COMPLETE lines are consumed. A line the host is still writing is
    // left for the next poll, so a half-written event is never injected.
    size_t complete = 0;
    for (size_t i = 0; i < read; ++i) {
        if (buffer[i] == '\n') {
            complete = i + 1;
        }
    }
    if (complete == 0) {
        return;
    }

    size_t cursor = 0;
    while (cursor < complete) {
        const char* line = buffer + cursor;
        const char* newline = std::strchr(line, '\n');
        size_t length = newline != nullptr
                            ? static_cast<size_t>(newline - line)
                            : complete - cursor;
        char text[256];
        if (length >= sizeof(text)) {
            length = sizeof(text) - 1;
        }
        std::memcpy(text, line, length);
        text[length] = '\0';
        cursor += length + 1;

        TouchEvent event{};
        char why[96] = {0};
        if (!parseLine(text, &event, why, sizeof(why))) {
            if (why[0] != '\0') {
                char detail[384];
                std::snprintf(detail, sizeof(detail), "line='%s' %s", text, why);
                logControl("touch-control-badline", detail);
            }
            continue;
        }
        if (!queueEvent(event)) {
            logControl("touch-control-overflow", "event queue full");
            break;
        }
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "queued x=%d y=%d action=%s id=%d due=%d", event.x,
                      event.y, actionName(event.action), event.id,
                      event.dueAt);
        logControl("touch-control-queued", detail);
    }
    g_controlConsumed += complete;
}

}  // namespace

bool prepareGuestTouchInput() {
    g_begin = reinterpret_cast<TouchBeginFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_MainActivity_onTouchBeginJNI"));
    g_move = reinterpret_cast<TouchMoveFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_MainActivity_onTouchMoveJNI"));
    g_end = reinterpret_cast<TouchEndFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_MainActivity_onTouchEndJNI"));
    g_cancel = reinterpret_cast<TouchCancelFn>(resolveMainImageExport(
        "Java_com_firemint_realracing_MainActivity_onTouchCancelJNI"));
    g_ready = g_begin != nullptr && g_move != nullptr && g_end != nullptr;
    g_armed = false;
    g_controlPolls = 0;
    g_controlMisses = 0;
    g_controlConsumed = 0;
    g_controlHaveSignature = false;
    g_controlFilePresent = false;
    g_controlPolledOnce = false;
    g_pumpCount = 0;
    g_queueHead = 0;
    g_queueTail = 0;
    return g_ready;
}

bool guestTouchInputReady() {
    return g_ready;
}

const char* guestTouchControlPath() {
    return kTouchControlPath;
}

// ---- the shared injection primitive -----------------------------------------

namespace {

const char* guestTouchActionName(GuestTouchAction action) {
    switch (action) {
    case GuestTouchAction::Begin:
        return "down";
    case GuestTouchAction::Move:
        return "move";
    case GuestTouchAction::End:
        return "up";
    case GuestTouchAction::Release:
        return "release";
    }
    return "?";
}

}  // namespace

GuestTouchState readGuestTouchState() {
    GuestTouchState out{};
    const TouchState state = readTouchState();
    out.valid = state.valid;
    out.down = state.down != 0;
    out.pointerId = state.pointerId;
    out.x = state.x;
    out.y = state.y;
    out.paused = state.paused;
    out.focus = state.focus;
    return out;
}

void injectGuestTouchEvent(void* jniEnvironment, void* activityObject,
                           GuestTouchAction action, int pointerId, int x, int y,
                           bool last, const char* phase) {
    if (!g_ready || jniEnvironment == nullptr) {
        return;
    }
    const char* fn = "?";
    uintptr_t fnAddress = 0;
    unsigned long long elfVa = kGuestOnTouchBeginVa;
    switch (action) {
    case GuestTouchAction::Begin:
        g_begin(jniEnvironment, activityObject, pointerId,
                static_cast<float>(x), static_cast<float>(y));
        fn = "onTouchBeginJNI";
        fnAddress = reinterpret_cast<uintptr_t>(g_begin);
        break;
    case GuestTouchAction::Move:
        g_move(jniEnvironment, activityObject, pointerId,
               static_cast<float>(x), static_cast<float>(y));
        fn = "onTouchMoveJNI";
        fnAddress = reinterpret_cast<uintptr_t>(g_move);
        elfVa = kGuestOnTouchMoveVa;
        break;
    case GuestTouchAction::End:
    case GuestTouchAction::Release:
        // `last` = 1 is what Android's ACTION_UP produces: GLView.java:290
        // passes motionEvent.getActionMasked() == 1, which is true for
        // ACTION_UP and false for ACTION_POINTER_UP.
        g_end(jniEnvironment, activityObject, pointerId, static_cast<float>(x),
              static_cast<float>(y), last ? 1 : 0);
        fn = "onTouchEndJNI";
        fnAddress = reinterpret_cast<uintptr_t>(g_end);
        elfVa = kGuestOnTouchEndVa;
        break;
    }

    char detail[288];
    const TouchState state = readTouchState();
    std::snprintf(detail, sizeof(detail),
                  "x=%d y=%d action=%s id=%d fn=%s elfva=%llx frame=%u "
                  "guest{valid=%d eng=%llx obj=%llx paused=%u focus=%u "
                  "enabled=%u gid=%d gx=%d gy=%d down=%u}",
                  x, y, guestTouchActionName(action), pointerId, fn, elfVa,
                  g_pumpCount, state.valid ? 1 : 0,
                  static_cast<unsigned long long>(state.engine),
                  static_cast<unsigned long long>(state.touchObject),
                  state.paused, state.focus, state.enabled, state.pointerId,
                  state.x, state.y, state.down);
    appendGuestBootLine(kTouchLogPath,
                        phase != nullptr ? phase : "touch-inject", 941,
                        static_cast<unsigned long long>(fnAddress), detail);
}

bool guestTouchControlFilePresent() {
    return g_controlFilePresent;
}

bool guestTouchControlHasPending() {
    return g_queueHead != g_queueTail;
}

bool guestTouchControlInjectedThisFrame() {
    return g_controlInjectedThisFrame;
}

bool guestTouchControlHasPolled() {
    return g_controlPolledOnce;
}

void pumpGuestTouchInput(void* jniEnvironment, void* activityObject) {
    // Cleared first, and before the early-out below: this flag says "the
    // file control injected in THIS frame", and the hardware path is pumped
    // right after this one, in the same frame.
    g_controlInjectedThisFrame = false;
    if (!g_ready || jniEnvironment == nullptr) {
        return;
    }
    ++g_pumpCount;
    ++g_controlPolls;
    const bool pollDue = (g_controlPolls % kControlPollEveryFrames) == 0;
    if (pollDue) {
        // A missing file is the OFF state. Do not pay for a failed open on
        // every poll.
        if (g_controlHaveSignature || g_controlMisses % kControlRetryEveryPolls == 0) {
            drainControlFile();
        } else {
            ++g_controlMisses;
        }
    }

    // A periodic reading of the guest's own touch state. The engine's touch
    // handler REFUSES a touch when the touch object's "enabled" byte
    // (touch object + 0xa) is zero -- it returns before it stores the point
    // and before it calls the menu layer -- so this byte says whether the
    // guest's touch path is awake at all, and the coordinates say what the
    // guest last received.
    if (g_pumpCount % kStateProbeEveryPumps == 0) {
        const TouchState state = readTouchState();
        char detail[288];
        std::snprintf(detail, sizeof(detail),
                      "frame=%u valid=%d eng=%llx obj=%llx paused=%u focus=%u "
                      "enabled=%u gid=%d gx=%d gy=%d down=%u",
                      g_pumpCount, state.valid ? 1 : 0,
                      static_cast<unsigned long long>(state.engine),
                      static_cast<unsigned long long>(state.touchObject),
                      state.paused, state.focus, state.enabled,
                      state.pointerId, state.x, state.y, state.down);
        appendGuestBootLine(kTouchLogPath, "touch-state", 942, 0, detail);
    }

    // Find the first event that is DUE. An event with "at <frame>" waits for
    // the port's frame counter; everything else is due immediately.
    int dueIndex = -1;
    for (int i = 0; i < kQueueCapacity; ++i) {
        const int index = (g_queueHead + i) % kQueueCapacity;
        if (index == g_queueTail) {
            break;
        }
        const TouchEvent& candidate = g_queue[index];
        if (candidate.dueAt < 0 ||
            static_cast<unsigned>(candidate.dueAt) <= g_pumpCount) {
            dueIndex = index;
            break;
        }
    }
    if (dueIndex < 0) {
        return;
    }
    // ONE event per frame. A down and its up must be two separate frames so
    // the guest's menu layer sees the button held, exactly as two Android
    // MotionEvents separated by a frame would.
    const TouchEvent event = g_queue[dueIndex];
    // Remove it, keeping the queue order of the rest: shift the tail back
    // over the removed slot and shrink the tail.
    {
        int index = dueIndex;
        while (index != g_queueTail) {
            const int next = (index + 1) % kQueueCapacity;
            g_queue[index] = g_queue[next];
            index = next;
        }
        g_queueTail = (g_queueTail + kQueueCapacity - 1) % kQueueCapacity;
    }

    // The guest call and the log line are the SHARED ones: the hardware
    // touch path makes the same calls through the same function, so a touch
    // from a finger and a touch from a script are indistinguishable to the
    // guest and to whoever reads the log.
    switch (event.action) {
    case kActionDown:
        // RELEASE A STUCK POINTER FIRST.
        //
        // The guest's touch handler stores a new point ONLY when no finger is
        // down (ELF 0x12b4624: ldrb w8, [x20, #0x20] ; cbz w8, <store>), and
        // the guest's touch object ships with down=1 and x=y=-1. A "down"
        // that arrives while the guest still believes a finger is down is
        // therefore dropped by the guest, and MEASURED: the first injection
        // of a run changed nothing in the guest's touch state, while the
        // same injection preceded by an "up" left the guest holding
        // gx=652 gy=567 down=1.
        //
        // A real touch stack does the same thing: Android sends the
        // ACTION_UP for a pointer it still believes is down before the next
        // ACTION_DOWN. So the port asks the GUEST which pointer is down
        // (its own state, read back above) and releases exactly that one.
        {
            const TouchState before = readTouchState();
            if (before.valid && before.down != 0) {
                g_end(jniEnvironment, activityObject, before.pointerId,
                      static_cast<float>(before.x),
                      static_cast<float>(before.y), 0);
                char release[192];
                std::snprintf(release, sizeof(release),
                              "released stale pointer id=%d at x=%d y=%d "
                              "before the press",
                              before.pointerId, before.x, before.y);
                appendGuestBootLine(kTouchLogPath, "touch-release-stale", 943,
                                    static_cast<unsigned long long>(
                                        reinterpret_cast<uintptr_t>(g_end)),
                                    release);
            }
        }
        injectGuestTouchEvent(jniEnvironment, activityObject,
                              GuestTouchAction::Begin, event.id, event.x,
                              event.y, false, "touch-inject");
        break;
    case kActionMove:
        injectGuestTouchEvent(jniEnvironment, activityObject,
                              GuestTouchAction::Move, event.id, event.x,
                              event.y, false, "touch-inject");
        break;
    case kActionUp:
        // `last` = 1 is what Android's ACTION_UP produces: GLView.java:290
        // passes motionEvent.getActionMasked() == 1, which is true for
        // ACTION_UP and false for ACTION_POINTER_UP.
        injectGuestTouchEvent(jniEnvironment, activityObject,
                              GuestTouchAction::End, event.id, event.x,
                              event.y, true, "touch-inject");
        break;
    default:
        break;
    }
    // The event is in the guest now. The hardware path is pumped right after
    // this one, in the same frame, and it must not add a second pointer.
    g_controlInjectedThisFrame = true;
}

}  // namespace rr3
