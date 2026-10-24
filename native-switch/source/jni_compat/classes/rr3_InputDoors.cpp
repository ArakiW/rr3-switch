//
// The guest's input doors: read the guest's own gamepad state back, and run
// the scripted virtual gamepad.
//
// The addresses and the layouts in this file were read out of
// reverse/native/arm64/libRealRacing3.so, not out of the Java and not out of
// a port note. The relevant guest code is quoted next to each constant.
//
// WHY A SEPARATE TU. guest_input.cpp is the controller bridge and stays that
// way; this file is the instrument that says whether the bridge reached the
// guest, and the script that makes an emulator run reproducible. Keeping the
// reading of guest memory out of the bridge means a fault in the instrument
// cannot be mistaken for a fault in the bridge.
//
#include "rr3/guest_input_doors.hpp"

#include "rr3/elf_preflight.hpp"
#include "rr3/guest_frame_capture.hpp"
#include "rr3/guest_touch.hpp"
#include "rr3/recovery_guard.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace {

// ---- the guest's layout, from the guest's own code -----------------------
//
// Java_com_firemint_realracing_ControllerManager_SetJoystickValueJNI
// (ELF 0x12aa604):
//   0x12aa628: adrp x8, #0x1f6d000
//   0x12aa62c: ldr  x8, [x8, #0x5d8]   ; the GOT slot -> the .bss CELL
//   0x12aa630: ldr  x8, [x8]           ; the cell    -> G (the CGlobal)
//   0x12aa638: ldr  x0, [x8, #0x1818]  ; the joystick manager
//   0x12aa660: bl   __dynamic_cast     ; -> fmJoystickManagerAndroid
//   0x12aa66c: ldr  x10, [x9, #0x78]!  ; the device-id map
//   0x12aa678: ldr  w11, [x10, #0x1c]  ; the map key
//   0x12aa6f8: ldrsw x8, [x8, #0x20]   ; the map value (the vector index)
//   0x12aa6fc: ldr  x9, [x0, #0xa0]    ; the controller vector
//   0x12aa708: ldr  x0, [x9, x8, lsl #3]
//   0x12aa70c: bl   0x12a9334
//
// 0x12a9334 (the axis setter):
//   add x8, x0, w1, sxtw #2 ; str s0, [x8, #0x84]
//
// 0x12a934c (the button setter):
//   and w8, w2, #1 ; add x9, x0, w1, sxtw #2 ; str w8, [x9, #0x1c]
constexpr uintptr_t kSetJoystickExportVa = 0x12aa604ULL;
// The GOT slot that holds the .bss cell: 0x1f6d5d8. The delta from the
// export is COMPUTED, never hand-added: two hand-added versions of one such
// subtraction were both wrong before it was computed.
constexpr uintptr_t kCGlobalGotSlotVa = 0x1f6d5d8ULL;
// G + 0x1818 is the joystick manager.
constexpr uintptr_t kCGlobalJoystickManagerOff = 0x1818ULL;
// The device-id map (std::map<int,int>) starts at manager+0x78; the vector of
// controllers at manager+0xa0.
constexpr uintptr_t kManagerMapOff = 0x78ULL;
constexpr uintptr_t kManagerVectorOff = 0xa0ULL;
// The map node, read out of the guest's own lookup loop.
constexpr uintptr_t kMapLeftOff = 0x00ULL;
constexpr uintptr_t kMapRightOff = 0x08ULL;
constexpr uintptr_t kMapKeyOff = 0x1cULL;
constexpr uintptr_t kMapValueOff = 0x20ULL;
// The controller object.
constexpr uintptr_t kCtrlButtonBase = 0x1cULL;  // + 4 * button ordinal
constexpr uintptr_t kCtrlAxisBase = 0x84ULL;    // + 4 * axis ordinal
constexpr uintptr_t kCtrlPrimaryFlag = 0x18ULL;  // byte: the manager's player
constexpr uintptr_t kCtrlPlayerId = 0xc4ULL;     // int
constexpr uintptr_t kCtrlType = 0xccULL;         // int
// The vtable of fmJoystickManagerAndroid (typeinfo 0x1ec9388 -> vtable
// 0x1ec92c8 -> the first virtual function at +0x10). The object's vptr must
// be this address: that is the positive control for "the object at
// G+0x1818 really is the joystick manager".
constexpr uintptr_t kJoystickManagerVtableVa = 0x1ec92d8ULL;

constexpr const char* kPadControlPath =
    "sdmc:/switch/rr3-native-rehost/rr3-pad.txt";
constexpr const char* kGuestViewLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// The guest's ControllerButtons ordinals that matter here. The values are
// the Java enum's, and the guest indexes its button array by them, so the
// bit index in the script's mask IS the ordinal.
enum GuestButtonOrdinal : int {
    kOrdA = 0,
    kOrdB = 1,
    kOrdX = 2,
    kOrdY = 3,
    kOrdL1 = 4,
    kOrdR1 = 5,
    kOrdL2 = 6,
    kOrdR2 = 7,
    kOrdDpadLeft = 8,
    kOrdDpadRight = 9,
    kOrdDpadUp = 10,
    kOrdDpadDown = 11,
    kOrdSelect = 12,
    kOrdStart = 13,
    kOrdDpadCenter = 14,
    kOrdCount = 15,
};

constexpr int kAxisCount = 6;

// ---- safe reads ----------------------------------------------------------
//
// A pointer the guest published can point at a page that is no longer there.
// A diagnostic that can crash the run it is measuring is worse than no
// diagnostic, so every read that is not inside the guest image goes through
// svcQueryMemory first. The page cache is a copy of guest_touch.cpp's: the
// per-frame touch path reads three distinct pages in a fixed order, and a
// one-entry cache misses on every one of them.
constexpr uintptr_t kPlausibleLo = 0x10000ULL;
constexpr uintptr_t kPlausibleHi = 0x0000800000000000ULL;

bool pointerPlausible(uintptr_t address) {
    return address >= kPlausibleLo && address < kPlausibleHi;
}

bool memoryReadable(uintptr_t address) {
    constexpr uintptr_t kPageMask = ~static_cast<uintptr_t>(0xFFF);
    const uintptr_t page = address & kPageMask;
    static uintptr_t s_page[8] = {0};
    static bool s_readable[8] = {false};
    static bool s_valid[8] = {false};
    for (int i = 0; i < 8; ++i) {
        if (s_valid[i] && page == s_page[i]) {
            return s_readable[i];
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
    for (int i = 1; i < 8; ++i) {
        if (!s_valid[i]) {
            slot = i;
            break;
        }
    }
    s_page[slot] = page;
    s_readable[slot] = readable;
    s_valid[slot] = true;
    return readable;
}

bool rangeReadable(uintptr_t address, size_t length) {
    if (length == 0) {
        return true;
    }
    return memoryReadable(address) && memoryReadable(address + length - 1);
}

bool readWord(uintptr_t address, uint64_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint64_t))) {
        if (!pointerPlausible(address) ||
            !rangeReadable(address, sizeof(uint64_t))) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint64_t*>(address);
    return true;
}

bool readDword(uintptr_t address, uint32_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, sizeof(uint32_t))) {
        if (!pointerPlausible(address) ||
            !rangeReadable(address, sizeof(uint32_t))) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint32_t*>(address);
    return true;
}

bool readFloat(uintptr_t address, float* out) {
    uint32_t bits = 0;
    if (!readDword(address, &bits)) {
        return false;
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
}

bool readByte(uintptr_t address, uint8_t* out) {
    if (address == 0 || out == nullptr) {
        return false;
    }
    if (!guestImageContains(address, 1)) {
        if (!pointerPlausible(address) || !memoryReadable(address)) {
            return false;
        }
    }
    *out = *reinterpret_cast<const volatile uint8_t*>(address);
    return true;
}

// ---- the CGlobal chain ---------------------------------------------------
//
// The GOT slot lives inside the guest image, so it is mapped for the whole
// run and can be read directly. Its value is the .bss CELL (a guest
// pointer), and the cell holds G.
//
// The export address comes from the caller: the bridge already resolves the
// guest's SetJoystickValueJNI export, and the Java_* name literal belongs to
// the bridge, not to the instrument.
uintptr_t cGlobalObject(uintptr_t exportAddress) {
    static uintptr_t s_export = 0;
    if (exportAddress != 0) {
        s_export = exportAddress;
    }
    if (s_export == 0) {
        return 0;
    }
    const uintptr_t slot = s_export + (kCGlobalGotSlotVa - kSetJoystickExportVa);
    if (!guestImageContains(slot, sizeof(uint64_t))) {
        return 0;
    }
    const uint64_t cell =
        *reinterpret_cast<const volatile uint64_t*>(slot);
    if (cell == 0 || !pointerPlausible(static_cast<uintptr_t>(cell))) {
        return 0;
    }
    uint64_t g = 0;
    if (!readWord(static_cast<uintptr_t>(cell), &g) || g == 0) {
        return 0;
    }
    return static_cast<uintptr_t>(g);
}

// ---- the virtual gamepad script -----------------------------------------
struct PadStep {
    unsigned long long at;
    VirtualPadState state;
};

constexpr int kMaxPadSteps = 64;

PadStep g_padSteps[kMaxPadSteps];
int g_padStepCount = 0;
bool g_padFilePresent = false;
unsigned long long g_padParsedAtFrame = 0;
bool g_padParsed = false;

float parseFloat(const char* text, float fallback) {
    if (text == nullptr || *text == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text) {
        return fallback;
    }
    return value;
}

// Parses one line: "[at <frame>] lx=<f> ... b=<mask>".
//
// The mask is parsed as hex, because the ordinals are bit positions and a
// decimal reading of "0x100" is exactly the mistake this format invites.
bool parsePadLine(const char* line, PadStep* out) {
    if (line == nullptr || out == nullptr) {
        return false;
    }
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    if (*line == '\0' || *line == '#' || *line == '\r' || *line == '\n') {
        return false;
    }
    PadStep& step = *out;
    step = PadStep{};
    step.state.active = true;

    const char* p = line;
    if (p[0] == '[') {
        const char* at = std::strstr(p, "at ");
        if (at == nullptr) {
            return false;
        }
        step.at = std::strtoull(at + 3, nullptr, 10);
        const char* close = std::strchr(p, ']');
        if (close == nullptr) {
            return false;
        }
        p = close + 1;
    }

    bool sawAny = false;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        const char* key = p;
        // The '=' ends the key. It has to: a key scan that runs past it
        // produces "lx=" and then no key ever matches, which is exactly the
        // bug this parser had (the script parsed, and every line was
        // dropped).
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n' && *p != '=') {
            ++p;
        }
        const size_t keyLen = static_cast<size_t>(p - key);
        if (keyLen == 0) {
            break;
        }
        if (*p == '=') {
            ++p;
        }
        const char* value = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            ++p;
        }
        const size_t valueLen = static_cast<size_t>(p - value);
        char keyBuf[16];
        char valueBuf[32];
        if (keyLen >= sizeof(keyBuf) || valueLen >= sizeof(valueBuf)) {
            continue;
        }
        std::memcpy(keyBuf, key, keyLen);
        keyBuf[keyLen] = '\0';
        std::memcpy(valueBuf, value, valueLen);
        valueBuf[valueLen] = '\0';

        if (std::strcmp(keyBuf, "lx") == 0) {
            step.state.leftX = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "ly") == 0) {
            step.state.leftY = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "rx") == 0) {
            step.state.rightX = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "ry") == 0) {
            step.state.rightY = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "lt") == 0) {
            step.state.leftTrigger = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "rt") == 0) {
            step.state.rightTrigger = parseFloat(valueBuf, 0.0f);
        } else if (std::strcmp(keyBuf, "b") == 0) {
            step.state.buttons = static_cast<unsigned int>(
                std::strtoul(valueBuf, nullptr, 16));
        } else {
            continue;
        }
        sawAny = true;
    }
    return sawAny;
}

void parsePadFile() {
    g_padStepCount = 0;
    std::FILE* file = std::fopen(kPadControlPath, "rb");
    if (file == nullptr) {
        g_padFilePresent = false;
        g_padParsed = true;
        return;
    }
    g_padFilePresent = true;
    char line[256];
    while (g_padStepCount < kMaxPadSteps &&
           std::fgets(line, sizeof(line), file) != nullptr) {
        PadStep step{};
        if (!parsePadLine(line, &step)) {
            continue;
        }
        g_padSteps[g_padStepCount++] = step;
    }
    std::fclose(file);
    g_padParsed = true;
}

// ---- the probe ------------------------------------------------------------

struct GuestGamepadState {
    bool managerReadable;
    uintptr_t manager;
    uintptr_t managerVptr;
    bool vtableMatches;
    uintptr_t mapRoot;
    int controllers;
    int keys[8];
    int indices[8];
    uintptr_t controller;
    int controllerIndex;
    bool controllerReadable;
    float axes[kAxisCount];
    int buttons[kOrdCount];
    unsigned int primaryFlag;
    int playerId;
    int type;
};

void readGuestGamepad(GuestGamepadState* state, uintptr_t exportAddress) {
    std::memset(state, 0, sizeof(*state));

    const uintptr_t g = cGlobalObject(exportAddress);
    if (g == 0) {
        return;
    }
    uint64_t manager = 0;
    if (!readWord(g + kCGlobalJoystickManagerOff, &manager) || manager == 0) {
        return;
    }
    state->manager = static_cast<uintptr_t>(manager);
    state->managerReadable = true;

    uint64_t vptr = 0;
    if (readWord(state->manager, &vptr) && vptr != 0) {
        state->managerVptr = static_cast<uintptr_t>(vptr);
        const uintptr_t bias = resolveMainImageLoadBias();
        state->vtableMatches =
            (bias != 0) && (state->managerVptr == bias + kJoystickManagerVtableVa);
    }

    uint64_t root = 0;
    if (!readWord(state->manager + kManagerMapOff, &root) || root == 0) {
        return;
    }
    state->mapRoot = static_cast<uintptr_t>(root);

    // Walk the map. The node layout is the guest's own: left +0x00,
    // right +0x08, key +0x1c, value +0x20. The walk is an in-order
    // traversal with an explicit stack and a depth bound, because a corrupt
    // tree must not turn a diagnostic into a hang, and because the right
    // spine alone would miss every controller but the last.
    uintptr_t stack[32];
    int sp = 0;
    uintptr_t node = state->mapRoot;
    while ((node != 0 || sp > 0) && sp < 32) {
        while (node != 0 && sp < 32) {
            uint64_t left = 0;
            if (!readWord(node + kMapLeftOff, &left)) {
                left = 0;
            }
            stack[sp++] = node;
            node = static_cast<uintptr_t>(left);
        }
        if (sp == 0) {
            break;
        }
        node = stack[--sp];
        uint32_t key = 0;
        uint32_t value = 0;
        if (readDword(node + kMapKeyOff, &key) &&
            readDword(node + kMapValueOff, &value)) {
            if (state->controllers < 8) {
                state->keys[state->controllers] = static_cast<int>(key);
                state->indices[state->controllers] = static_cast<int>(value);
                ++state->controllers;
            }
        }
        uint64_t right = 0;
        if (!readWord(node + kMapRightOff, &right)) {
            right = 0;
        }
        node = static_cast<uintptr_t>(right);
    }
    if (state->controllers == 0) {
        return;
    }

    // The first controller in the map is the one the port drives (device 1).
    uint64_t vector = 0;
    if (!readWord(state->manager + kManagerVectorOff, &vector) || vector == 0) {
        return;
    }
    const int index = state->indices[0];
    uint64_t controller = 0;
    if (!readWord(static_cast<uintptr_t>(vector) +
                      static_cast<uintptr_t>(index) * sizeof(uint64_t),
                  &controller) ||
        controller == 0) {
        return;
    }
    state->controller = static_cast<uintptr_t>(controller);
    state->controllerIndex = index;
    state->controllerReadable = true;

    for (int a = 0; a < kAxisCount; ++a) {
        if (!readFloat(state->controller + kCtrlAxisBase +
                           static_cast<uintptr_t>(a) * sizeof(float),
                       &state->axes[a])) {
            state->axes[a] = 0.0f;
        }
    }
    for (int b = 0; b < kOrdCount; ++b) {
        uint32_t value = 0;
        if (!readDword(state->controller + kCtrlButtonBase +
                          static_cast<uintptr_t>(b) * sizeof(uint32_t),
                      &value)) {
            value = 0;
        }
        state->buttons[b] = static_cast<int>(value);
    }
    uint8_t primary = 0;
    if (readByte(state->controller + kCtrlPrimaryFlag, &primary)) {
        state->primaryFlag = primary;
    }
    uint32_t playerId = 0;
    if (readDword(state->controller + kCtrlPlayerId, &playerId)) {
        state->playerId = static_cast<int>(playerId);
    }
    uint32_t type = 0;
    if (readDword(state->controller + kCtrlType, &type)) {
        state->type = static_cast<int>(type);
    }
}

unsigned int dpadMask(const int* buttons) {
    // The guest's own d-pad encoding (fmJoystickAndroid vtable slot 20):
    // bit 0 up, bit 1 right, bit 2 down, bit 3 left.
    unsigned int mask = 0;
    if (buttons[kOrdDpadUp] != 0) {
        mask |= 1u;
    }
    if (buttons[kOrdDpadRight] != 0) {
        mask |= 2u;
    }
    if (buttons[kOrdDpadDown] != 0) {
        mask |= 4u;
    }
    if (buttons[kOrdDpadLeft] != 0) {
        mask |= 8u;
    }
    return mask;
}

// ---- the before/after picture --------------------------------------------
//
// The port's own capture schedule (30, 120, 300, 600, 1000, 3000, ...) is
// far too coarse to show a steering change: a car that turns needs the two
// pictures to be seconds apart, not minutes. So the instrument takes its own
// pair of pictures around every change of the pushed input: one when the
// input changes and one a dozen frames later.
//
// THE FRAME IS THE PREVIOUS ONE. This runs before the guest's render call in
// the frame loop, so the back buffer holds the picture the guest drew last.
//
// The file name is frame-9NNNNN.png. The port's own captures are
// frame-<schedule>.png, and the archive only collects frame-*.png, so the
// name has to start with "frame-". The 9 prefix keeps these out of the
// port's schedule (30, 120, 300, 600, 1000, 3000, 7000, ...), so one never
// overwrites the other.
//
// glReadPixels is called only when an EGL context is current on this thread.
// Without that check a readback on a thread with no context is a crash, and
// a diagnostic that can crash the run it is measuring is worse than no
// diagnostic.
void captureFrame(unsigned long long frame) {
    static unsigned char* pixels = nullptr;
    if (pixels == nullptr) {
        pixels = static_cast<unsigned char*>(
            std::malloc(static_cast<size_t>(1280) * 720u * 4u));
        if (pixels == nullptr) {
            return;
        }
    }
    if (eglGetCurrentContext() == nullptr) {
        return;
    }
    std::memset(pixels, 0, static_cast<size_t>(1280) * 720u * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 1280, 720, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    char path[128];
    std::snprintf(path, sizeof(path),
                  "sdmc:/switch/rr3-native-rehost/frame-9%05llu.png",
                  static_cast<unsigned long long>(frame) % 100000ULL);
    const bool wrote = rr3::writeFramePng(path, pixels, 1280, 720);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "frame=%llu png=%s path=%s",
                  static_cast<unsigned long long>(frame),
                  wrote ? "ok" : "FAILED", path);
    appendGuestBootLine(kGuestViewLogPath, "input-door-capture", 946, 0, detail);
}

}  // namespace

bool virtualPadControlFilePresent() {
    return g_padFilePresent;
}

// How many script states the last parse produced. Zero with the file present
// means the file was found and nothing in it parsed, which is a different
// fault from "no file", so the probe reports the two separately.
int virtualPadStepCount() {
    return g_padStepCount;
}

bool virtualPadForFrame(unsigned long long frame, VirtualPadState* out) {
    if (out == nullptr) {
        return false;
    }
    // The script is re-read every 30 frames (about three times a second at
    // the guest's frame rate). The file lives on the card and the card is
    // slow, but the point of the script is that it can be changed WHILE the
    // run is going, so the cadence has to be short enough to be useful.
    if (!g_padParsed || frame >= g_padParsedAtFrame + 30ULL) {
        parsePadFile();
        g_padParsedAtFrame = frame;
    }
    if (g_padStepCount == 0) {
        return false;
    }
    const PadStep* chosen = nullptr;
    for (int i = 0; i < g_padStepCount; ++i) {
        if (g_padSteps[i].at <= frame) {
            chosen = &g_padSteps[i];
        } else {
            break;
        }
    }
    if (chosen == nullptr) {
        return false;
    }
    *out = chosen->state;
    out->active = true;
    return true;
}

void probeGuestInputDoors(unsigned long long frame,
                          const GuestPushedInput* pushed,
                          uintptr_t setJoystickExport) {
    // Rate limited: one line every 30 frames, plus one whenever the pushed
    // state changes (so a steering change is never missed).
    static unsigned long long s_lastLoggedFrame = 0;
    static unsigned int s_lastPushedButtons = 0;
    static bool s_lastValid = false;
    static float s_lastAxes[kAxisCount] = {0};

    // The picture pair is driven by its OWN copy of the pushed state. Sharing
    // the log's copy would make "changed" true on every frame until the next
    // log line, and the instrument would take a picture every frame.
    static unsigned int s_captureButtons = 0;
    static float s_captureAxes[kAxisCount] = {0};
    static bool s_captureValid = false;
    // Three pictures per change: the moment the input changes (the picture
    // before it takes effect), a moment later, and a moment after that. A car
    // turning needs SECONDS to show it in the picture, and the guest renders
    // at about seven frames a second, so a twelve-frame gap is two seconds
    // and shows nothing.
    static unsigned long long s_captureAt[3] = {0, 0, 0};
    static int s_capturePending = 0;

    bool changed = false;
    if (pushed != nullptr) {
        if (!s_lastValid || pushed->buttons != s_lastPushedButtons) {
            changed = true;
        }
        for (int a = 0; a < kAxisCount; ++a) {
            const float delta = pushed->axes[a] - s_lastAxes[a];
            if (delta > 0.01f || delta < -0.01f) {
                changed = true;
            }
        }
    }

    bool inputChanged = false;
    if (pushed != nullptr) {
        if (!s_captureValid || pushed->buttons != s_captureButtons) {
            inputChanged = true;
        }
        for (int a = 0; a < kAxisCount; ++a) {
            const float delta = pushed->axes[a] - s_captureAxes[a];
            if (delta > 0.01f || delta < -0.01f) {
                inputChanged = true;
            }
        }
        if (inputChanged) {
            s_captureButtons = pushed->buttons;
            for (int a = 0; a < kAxisCount; ++a) {
                s_captureAxes[a] = pushed->axes[a];
            }
            s_captureValid = true;
        }
    }

    // The pictures are taken even when the LINE is suppressed: the picture is
    // the evidence, the line is only the narration.
    if (inputChanged && frame >= 600ULL) {
        captureFrame(frame);
        s_captureAt[0] = frame + 45ULL;
        s_captureAt[1] = frame + 135ULL;
        s_captureAt[2] = frame + 300ULL;
        s_capturePending = 3;
    } else if (s_capturePending > 0 && frame >= s_captureAt[0]) {
        captureFrame(frame);
        for (int i = 0; i < 2; ++i) {
            s_captureAt[i] = s_captureAt[i + 1];
        }
        s_captureAt[2] = 0;
        --s_capturePending;
    }

    if (!changed && frame < s_lastLoggedFrame + 30ULL) {
        return;
    }
    s_lastLoggedFrame = frame;
    if (pushed != nullptr) {
        s_lastPushedButtons = pushed->buttons;
        for (int a = 0; a < kAxisCount; ++a) {
            s_lastAxes[a] = pushed->axes[a];
        }
        s_lastValid = true;
    }

    GuestGamepadState state{};
    readGuestGamepad(&state, setJoystickExport);

    const GuestTouchState touch = readGuestTouchState();

    char axisText[128];
    char pushedAxisText[128];
    char buttonText[96];
    std::size_t used = 0;
    for (int a = 0; a < kAxisCount; ++a) {
        const int written = std::snprintf(
            axisText + used, sizeof(axisText) - used, "%s%.3f",
            a == 0 ? "" : ",", static_cast<double>(state.axes[a]));
        if (written <= 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
        if (used >= sizeof(axisText)) {
            break;
        }
    }
    used = 0;
    for (int a = 0; a < kAxisCount; ++a) {
        const float value = pushed != nullptr ? pushed->axes[a] : 0.0f;
        const int written = std::snprintf(
            pushedAxisText + used, sizeof(pushedAxisText) - used, "%s%.3f",
            a == 0 ? "" : ",", static_cast<double>(value));
        if (written <= 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
        if (used >= sizeof(pushedAxisText)) {
            break;
        }
    }
    used = 0;
    for (int b = 0; b < kOrdCount; ++b) {
        const int written = std::snprintf(
            buttonText + used, sizeof(buttonText) - used, "%s%d",
            b == 0 ? "" : ",", state.buttons[b]);
        if (written <= 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
        if (used >= sizeof(buttonText)) {
            break;
        }
    }

    unsigned int pushedButtons = pushed != nullptr ? pushed->buttons : 0;
    unsigned int guestButtons = 0;
    for (int b = 0; b < kOrdCount; ++b) {
        if (state.buttons[b] != 0) {
            guestButtons |= 1u << b;
        }
    }
    const bool buttonsMatch = (pushedButtons == guestButtons);
    bool axesMatch = true;
    for (int a = 0; a < kAxisCount; ++a) {
        const float want = pushed != nullptr ? pushed->axes[a] : 0.0f;
        const float delta = state.axes[a] - want;
        if (delta > 0.01f || delta < -0.01f) {
            axesMatch = false;
        }
    }

    char detail[900];
    std::snprintf(
        detail, sizeof(detail),
        "f=%llu mgr=%llx mgr_ok=%d mgrvt=%llx vt_ok=%d root=%llx n=%d "
        "key0=%d idx0=%d ctrl=%llx ctrl_ok=%d prim=%u pid=%d type=%d "
        "ax=[%s] btn=[%s] dpad=0x%x "
        "inj_ax=[%s] inj_btn=0x%x push=%llu "
        "ax_match=%d btn_match=%d "
        "touch{valid=%d paused=%u focus=%u down=%u gid=%d gx=%d gy=%d} "
        "pad_script=%d pad_steps=%d",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(state.manager),
        state.managerReadable ? 1 : 0,
        static_cast<unsigned long long>(state.managerVptr),
        state.vtableMatches ? 1 : 0,
        static_cast<unsigned long long>(state.mapRoot),
        state.controllers,
        state.controllers > 0 ? state.keys[0] : -1,
        state.controllers > 0 ? state.indices[0] : -1,
        static_cast<unsigned long long>(state.controller),
        state.controllerReadable ? 1 : 0,
        state.primaryFlag, state.playerId, state.type,
        axisText, buttonText, dpadMask(state.buttons),
        pushedAxisText, pushedButtons,
        pushed != nullptr ? pushed->pushes : 0ULL,
        axesMatch ? 1 : 0, buttonsMatch ? 1 : 0,
        touch.valid ? 1 : 0, touch.paused, touch.focus, touch.down ? 1u : 0u,
        touch.pointerId, touch.x, touch.y,
        virtualPadControlFilePresent() ? 1 : 0, virtualPadStepCount());

    appendGuestBootLine(kGuestViewLogPath, "input-door", 945, 0, detail);
}

}  // namespace rr3
