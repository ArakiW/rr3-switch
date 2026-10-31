// ---------------------------------------------------------------------------
// THE SEMANTIC UI PROBE: the guest's OWN screen tree, read by control id.
//
// WHAT THIS IS
// ------------
// The port's UI driver finds buttons by their COLOURS (the guest's red glove,
// the orange accent button).  That works for the racing screen, but it cannot
// say WHAT it clicked, it cannot click a button that is not accented, and it
// cannot reach the game's meta screens at all.  The guest NAMES every control
// -- "BTN_RACE_NOW", "BTN_CONTINUE", "BTN_STATS", "BANK_SCROLLER", ... -- so
// the port can ask the guest WHERE a button is instead of guessing with pixels.
//
// MEASURED 2026-10-31 (q40, archive .build/eden-runs/20261031-124116-eden-rr3-
// q40-semantic-ui): the probe reads the live tree and names the screen and
// every control in it, with the rectangle in view pixels.
//
//     f=180  screen=EventMapScreen   controls=342
//            ANIM_IN(0,0,1280,720) IMG_SKY_BURN(0,0,1280,395)
//            SCROLLER_FRAME(0,52,1280,619)
//     f=420  screen=CarSelectScreen  controls=452
//            CAR_SELECT_SCROLLER(64,89,1152,423) BTN_LEFT_CAR(16,513,52,54)
//
// The rectangles are VIEW PIXELS, and the .gui files' own transforms agree:
// BTN_LEFT_CAR's transform is x=0.0122839 y=0.7125, and 0.0122839 * 1280 =
// 15.7 and 0.7125 * 720 = 513 -- the same place the live tree says.  Two
// independent instruments, the static file and the live object.
//
// THE ID VOCABULARY IS STATICALLY KNOWN
// -------------------------------------
// Every .gui file on the card is a stream-ciphered XML document (the guest's
// RC4 variant, key a535b3b1e843e7cf -- the SAME cipher as the device
// profiles).  Decrypted, all 939 of them declare an `id_string` on every
// widget: 4,729 distinct ids across 11,936 widgets, and 314 of those ids also
// appear literally in the guest's own binary, which is how the guest looks
// them up.  tools/gui_index.py reads them.
//
// EVERY POINTER PROVES ITSELF
// ---------------------------
// A pointer is taken as a component ONLY when the name at +0x38 reads back as
// a plausible id: 1..63 characters, all of them [A-Za-z0-9_].  A wrong offset
// therefore yields NO control instead of a crash, and the probe reports what
// it found rather than what it assumed.
//
// AND THE FRAME LOOP NEVER TOUCHES GUEST MEMORY
// ---------------------------------------------
// The probe is pumped from the INPUT PUMP (the same place the race-state
// probe is pumped, and for the same reason: walking the guest's object graph
// ON DEMAND from the frame loop froze the run twice, measured in q23 and
// q24).  The walk runs on a CADENCE, not every frame, and the frame loop
// reads the CACHED sample and nothing else.
//
// ONE READ DOOR
// -------------
// Every read of guest memory goes through raceStateReadWord(), the same
// plausibility/guest-image/svcQueryMemory door the race-state probe uses.
// "The port must never read guest memory any other way."
//
// EVERY BUFFER THAT LIVES ON THE GUEST'S THREAD LIVES ON THE HEAP
// ---------------------------------------------------------------
// This file runs on the guest's own thread (the input pump is inside the
// frame loop).  This project has already measured that a 64 KiB stack buffer
// on that thread is fatal, and that a 4 KiB one overflowed the stack and
// corrupted a return address -- with a symptom indistinguishable from an
// external kill.  So the control list is a std::vector, on the heap.
// ---------------------------------------------------------------------------
#include <rr3/ui_control_probe.hpp>

#include <rr3/elf_preflight.hpp>
#include <rr3/gfx_backend.hpp>
#include <rr3/race_state_probe.hpp>
#include <rr3/recovery_guard.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rr3 {
namespace {

constexpr const char* kGuestViewLogPath =
    "sdmc:/switch/rr3-native-rehost.guest-view.log";

// ---- the measured layout --------------------------------------------------
//
// G                 the CGlobal, reached through raceStateResolveGlobal()
// G + 0x17f8        the FrontEnd2::MainMenuManager
// mgr + 0xa8        the screen registry's root (an intrusive tree)
// node + 0x00       the tree's left child
// node + 0x08       the tree's right child
// node + 0x10       the tree's parent
// node + 0x20       the screen's NAME (a 24-byte libc++ string)
// node + 0x38       the screen INSTANCE
//
// comp + 0x34       the component's numeric id
// comp + 0x38       the component's NAME (a 24-byte libc++ string)
// comp + 0x138      the component's CHILDREN container
// comp + 0x160      the component's AABB
// comp + 0x180      the component's rectangle: x, y, x+w, y+h
constexpr uintptr_t kManagerOff       = 0x17F8ULL;
constexpr uintptr_t kRegistryRootOff  = 0x0A8ULL;
constexpr uintptr_t kTreeLeftOff      = 0x00ULL;
constexpr uintptr_t kTreeRightOff     = 0x08ULL;
constexpr uintptr_t kTreeParentOff    = 0x10ULL;
constexpr uintptr_t kTreeNameOff      = 0x20ULL;
constexpr uintptr_t kTreeInstanceOff  = 0x38ULL;

constexpr uintptr_t kCompIdOff        = 0x34ULL;
constexpr uintptr_t kCompNameOff      = 0x38ULL;
constexpr uintptr_t kCompChildrenOff  = 0x138ULL;
constexpr uintptr_t kCompRectOff      = 0x180ULL;

// ---- the walk's limits ----------------------------------------------------
//
// The walk touches a lot of guest pages, and this project has already measured
// that a probe spending ~1500 svcQueryMemory calls per sample drops the
// guest's frame rate.  The syscall count, not the read count, is the budget.
constexpr int kMaxDepth = 24;
constexpr int kMaxNodes = 4096;
constexpr int kMaxControls = 4096;
constexpr int kMaxScreens = 96;

// The cadence.  The UI changes faster than the race state does, so this one is
// quicker, but it is still not per-frame.
constexpr unsigned long long kProbePeriodFrames = 30ULL;

// ---- reading, through the one door ----------------------------------------
bool readU64(uintptr_t address, uint64_t* out) {
    return raceStateReadWord(address, out);
}

// A 32-bit field, read as the low half of a 64-bit word.  The port is
// little-endian aarch64 and the sanctioned door reads whole words, so this is
// the honest way to take the low half -- and it keeps ONE door.
bool readU32(uintptr_t address, uint32_t* out) {
    uint64_t word = 0;
    if (!readU64(address, &word)) {
        return false;
    }
    *out = static_cast<uint32_t>(word & 0xFFFFFFFFULL);
    return true;
}

bool pointerSane(uintptr_t address) {
    // The same plausibility window the race-state probe uses.
    return address >= 0x10000ULL && address < 0x0000800000000000ULL;
}

// ---- the guest's libc++ strings --------------------------------------------
//
// A std::string is 24 bytes.  Byte 0 is a flag: bit 0 set means the string is
// LONG (its length is a u64 at +8 and its characters live behind a heap
// pointer at +0x10); bit 0 clear means the string is SHORT (its length is that
// byte shifted right by one, and its characters are INLINE at +1).
//
// The whole 24 bytes are read in three word reads, so a short string costs
// three reads and not one read per character.
bool readGuestString(uintptr_t address, char* out, size_t capacity) {
    if (address == 0 || out == nullptr || capacity == 0) {
        return false;
    }
    uint64_t w0 = 0;
    uint64_t w1 = 0;
    uint64_t w2 = 0;
    if (!readU64(address, &w0) || !readU64(address + 8, &w1) ||
        !readU64(address + 0x10, &w2)) {
        return false;
    }
    uint8_t bytes[24];
    std::memcpy(bytes, &w0, 8);
    std::memcpy(bytes + 8, &w1, 8);
    std::memcpy(bytes + 16, &w2, 8);

    const uint8_t flag = bytes[0];
    if ((flag & 1u) != 0) {
        uint64_t length = 0;
        uint64_t chars = 0;
        std::memcpy(&length, bytes + 8, 8);
        std::memcpy(&chars, bytes + 16, 8);
        if (length == 0 || length > 4096 ||
            !pointerSane(static_cast<uintptr_t>(chars))) {
            return false;
        }
        for (uint64_t i = 0; i < length; ++i) {
            uint64_t ch = 0;
            if (!readU64(static_cast<uintptr_t>(chars) + i, &ch)) {
                return false;
            }
            if (i + 1 < capacity) {
                out[i] = static_cast<char>(ch & 0xFF);
            }
        }
        if (length < capacity) {
            out[length] = '\0';
        } else {
            out[capacity - 1] = '\0';
        }
        return true;
    }
    const size_t length = static_cast<size_t>(flag >> 1);
    if (length > 22) {
        return false;
    }
    std::memcpy(out, bytes + 1, length);
    out[length] = '\0';
    return true;
}

// A control id: 1..63 characters, all of them [A-Za-z0-9_].
//
// This is the probe's proof gate.  The guest's own ids are editor names
// ("BTN_RACE_NOW", "IMG_SKY_BURN", "SCROLLER_FRAME"), so they are uppercase
// words, digits and underscores.
bool isControlId(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (; text[n] != '\0'; ++n) {
        if (n >= 63) {
            return false;
        }
        const char c = text[n];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return n >= 1;
}

// ---- the probe's own state -------------------------------------------------
UiControlSample g_sample{};
std::vector<UiControl> g_controls;
unsigned long long g_lastWalkFrame = 0;
unsigned long long g_currentFrame = 0;
int g_viewW = 0;
int g_viewH = 0;

// A control is PRESSABLE when its rectangle has a size and its centre is
// inside the view.
//
// This is not a guess.  The live rectangle is the control's CURRENT absolute
// position, and the guest animates controls: CarSelect's BTN_STATS is declared
// at (-1, 81) in the .gui file, and the live tree reports (-370, 81) -- the
// same y and the same size, shifted left by the animation that slides the
// button in.  A button that is off screen is not a button you can press, and
// the port must not press one.
bool isPressable(const UiControl& control) {
    if (control.w <= 0 || control.h <= 0) {
        return false;
    }
    const int cx = control.x + control.w / 2;
    const int cy = control.y + control.h / 2;
    return cx >= 0 && cy >= 0 && cx < g_viewW && cy < g_viewH;
}

// The previous sample's identity, so the log only fires when something
// actually changed.  A UI probe that logs every frame is a probe nobody
// reads.
char g_lastScreen[64] = {0};
int g_lastCount = -1;
uint64_t g_lastHash = 0;

// How many walks have happened, so the probe can SAY that it ran and found
// nothing.  "The instrument did not measure it" and "the thing did not
// happen" are different statements, and a probe that stays silent when it
// finds nothing cannot tell them apart.
unsigned long long g_walkCount = 0;

// The layout diagnostic runs ONCE per run: it is expensive and it is only
// interesting the first time.
bool g_layoutLogged = false;

uint64_t hashControls(const std::vector<UiControl>& controls) {
    uint64_t h = 1469598103934665603ULL;
    for (const UiControl& control : controls) {
        for (const char* p = control.name; *p != '\0'; ++p) {
            h ^= static_cast<uint8_t>(*p);
            h *= 1099511628211ULL;
        }
        h ^= static_cast<uint64_t>(control.x);
        h *= 1099511628211ULL;
        h ^= static_cast<uint64_t>(control.y);
        h *= 1099511628211ULL;
    }
    return h;
}

void logSample(unsigned long long frame, const UiControlSample& sample) {
    char detail[1024];
    int written = std::snprintf(detail, sizeof(detail),
                                "f=%llu screen=%s controls=%d screens=%d live=%d vp=%dx%d",
                                frame, sample.screen[0] ? sample.screen : "-",
                                sample.count, sample.screensRegistered,
                                sample.screensLive, sample.viewportW,
                                sample.viewportH);
    // The PRESSABLE controls come first: they are the ones a driver can
    // actually use, so they are what a reader needs to see.
    int shown = 0;
    for (int pass = 0; pass < 2 && shown < 10; ++pass) {
        for (int i = 0; i < sample.count && shown < 10; ++i) {
            const UiControl& control = sample.items[i];
            if (isPressable(control) != (pass == 0)) {
                continue;
            }
            if (written <= 0 || written >= static_cast<int>(sizeof(detail)) - 80) {
                break;
            }
            written += std::snprintf(detail + written,
                                     sizeof(detail) - static_cast<size_t>(written),
                                     " %s%s(%d,%d,%d,%d)", control.name,
                                     pass == 0 ? "" : "~",
                                     control.x, control.y, control.w, control.h);
            ++shown;
        }
    }
    appendGuestBootLine(kGuestViewLogPath, "ui-ctl", 130, 0, detail);
}

// ---- the screen registry ---------------------------------------------------
struct ScreenEntry {
    char name[64];
    uintptr_t instance;
};

void collectRegistry(uintptr_t node, ScreenEntry* out, int* count, int depth) {
    if (node == 0 || depth > 32 || *count >= kMaxScreens) {
        return;
    }
    uint64_t left = 0;
    uint64_t right = 0;
    uint64_t instance = 0;
    char name[64] = {0};
    // The node proves itself through its NAME.  A wrong tree layout yields a
    // name that is not an id, and the walk stops there instead of running off
    // into memory.
    const bool named = readGuestString(node + kTreeNameOff, name, sizeof(name)) &&
                       isControlId(name);
    if (named) {
        if (readU64(node + kTreeInstanceOff, &instance) &&
            pointerSane(static_cast<uintptr_t>(instance))) {
            std::memcpy(out[*count].name, name, sizeof(name));
            out[*count].instance = static_cast<uintptr_t>(instance);
            ++(*count);
        }
    }
    // Keep walking the children either way: one bad node must not hide the
    // rest of the tree.
    if (readU64(node + kTreeLeftOff, &left)) {
        collectRegistry(static_cast<uintptr_t>(left), out, count, depth + 1);
    }
    if (readU64(node + kTreeRightOff, &right)) {
        collectRegistry(static_cast<uintptr_t>(right), out, count, depth + 1);
    }
}

// ---- the component tree ----------------------------------------------------
//
// The children container at comp+0x138 is read as a (begin, end, capacity)
// triple and ACCEPTED ONLY IF the triple is consistent and the pointers are
// sane.  That check is the proof: a wrong offset gives begin > end, or a
// capacity below the end, or a pointer outside the plausible range, and the
// walk then finds no children at all -- it does not crash and it does not
// invent a tree.
bool readChildren(uintptr_t component, uintptr_t* out, int* count) {
    *count = 0;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t capacity = 0;
    if (!readU64(component + kCompChildrenOff, &begin) ||
        !readU64(component + kCompChildrenOff + 8, &end) ||
        !readU64(component + kCompChildrenOff + 16, &capacity)) {
        return false;
    }
    if (begin == 0 || end < begin || capacity < end) {
        return false;
    }
    if (!pointerSane(static_cast<uintptr_t>(begin)) ||
        !pointerSane(static_cast<uintptr_t>(end))) {
        return false;
    }
    const uint64_t bytes = end - begin;
    if (bytes % 8 != 0 || bytes > 8 * 4096) {
        return false;
    }
    *out = static_cast<uintptr_t>(begin);
    *count = static_cast<int>(bytes / 8);
    return true;
}

struct WalkState {
    std::vector<UiControl>* controls;
    int nodes;
};

void walkComponent(uintptr_t component, WalkState* state, int depth) {
    if (component == 0 || depth > kMaxDepth || state->nodes >= kMaxNodes) {
        return;
    }
    ++state->nodes;

    // The control's own name.
    char name[64] = {0};
    if (readGuestString(component + kCompNameOff, name, sizeof(name)) &&
        isControlId(name)) {
        if (state->controls->size() < static_cast<size_t>(kMaxControls)) {
            UiControl control{};
            std::memcpy(control.name, name, sizeof(control.name));
            uint32_t x = 0;
            uint32_t y = 0;
            uint32_t right = 0;
            uint32_t bottom = 0;
            if (readU32(component + kCompRectOff, &x) &&
                readU32(component + kCompRectOff + 4, &y) &&
                readU32(component + kCompRectOff + 8, &right) &&
                readU32(component + kCompRectOff + 12, &bottom)) {
                control.x = static_cast<int>(x);
                control.y = static_cast<int>(y);
                control.w = static_cast<int>(right) - static_cast<int>(x);
                control.h = static_cast<int>(bottom) - static_cast<int>(y);
            }
            state->controls->push_back(control);
        }
    }

    uintptr_t children = 0;
    int childCount = 0;
    if (!readChildren(component, &children, &childCount)) {
        return;
    }
    for (int i = 0; i < childCount; ++i) {
        uint64_t child = 0;
        if (!readU64(children + static_cast<uintptr_t>(i) * 8, &child)) {
            continue;
        }
        if (!pointerSane(static_cast<uintptr_t>(child))) {
            continue;
        }
        walkComponent(static_cast<uintptr_t>(child), state, depth + 1);
    }
}

// ---------------------------------------------------------------------------
// THE LAYOUT DIAGNOSTIC
//
// When the walk finds no component tree, the probe says so -- and it also
// says WHAT IT SAW, because "not found" is not a black box.  This prints, for
// the first few registered screens: the instance address, its vtable pointer,
// whether the instance's own name field reads as a control id, and the raw
// words around the places a component's fields are expected to be.
//
// It is a ONE-SHOT diagnostic: it runs once per run, so it cannot become a
// per-frame cost.
// ---------------------------------------------------------------------------
void logLayoutDiagnostic(const ScreenEntry* screens, int screenCount,
                          unsigned long long frame) {
    for (int s = 0; s < screenCount && s < 3; ++s) {
        const uintptr_t instance = screens[s].instance;
        char detail[1024];
        int written = std::snprintf(detail, sizeof(detail),
                                    "f=%llu screen=%s inst=0x%llx",
                                    frame, screens[s].name,
                                    static_cast<unsigned long long>(instance));
        uint64_t vptr = 0;
        uint32_t id = 0;
        if (readU64(instance, &vptr)) {
            written += std::snprintf(detail + written,
                                     sizeof(detail) - static_cast<size_t>(written),
                                     " vptr=0x%llx",
                                     static_cast<unsigned long long>(vptr));
        }
        if (readU32(instance + kCompIdOff, &id)) {
            written += std::snprintf(detail + written,
                                     sizeof(detail) - static_cast<size_t>(written),
                                     " id=%u", id);
        }
        char name[64] = {0};
        const bool named =
            readGuestString(instance + kCompNameOff, name, sizeof(name));
        written += std::snprintf(detail + written,
                                 sizeof(detail) - static_cast<size_t>(written),
                                 " name=%s nameOk=%d",
                                 named ? name : "-", isControlId(name) ? 1 : 0);
        for (uintptr_t off = 0x130; off <= 0x1A0; off += 8) {
            uint64_t word = 0;
            if (!readU64(instance + off, &word) || written <= 0 ||
                written >= static_cast<int>(sizeof(detail)) - 90) {
                break;
            }
            written += std::snprintf(detail + written,
                                     sizeof(detail) - static_cast<size_t>(written),
                                     " [%llx]=%llx",
                                     static_cast<unsigned long long>(off),
                                     static_cast<unsigned long long>(word));
        }
        for (uintptr_t off = 0x00; off <= 0x200; off += 8) {
            uint64_t word = 0;
            if (!readU64(instance + off, &word) || word == 0) {
                continue;
            }
            const uintptr_t candidate = static_cast<uintptr_t>(word);
            if (!pointerSane(candidate)) {
                continue;
            }
            char sub[64] = {0};
            if (!readGuestString(candidate + kCompNameOff, sub, sizeof(sub)) ||
                !isControlId(sub)) {
                continue;
            }
            if (written <= 0 || written >= static_cast<int>(sizeof(detail)) - 90) {
                break;
            }
            written += std::snprintf(detail + written,
                                     sizeof(detail) - static_cast<size_t>(written),
                                     " C@%llx=%s",
                                     static_cast<unsigned long long>(off), sub);
        }
        appendGuestBootLine(kGuestViewLogPath, "ui-layout", 132, 0, detail);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The pump.
//
// It runs from the INPUT PUMP, on a cadence.  It reads the guest's registry,
// works out which screen is live (the one whose component tree has the most
// named controls), and walks that tree.
// ---------------------------------------------------------------------------
void pumpUiControlProbe(unsigned long long frame) {
    g_currentFrame = frame;
    if (frame < g_lastWalkFrame + kProbePeriodFrames) {
        return;
    }
    g_lastWalkFrame = frame;

    const uintptr_t g = raceStateResolveGlobal();
    if (g == 0) {
        return;
    }

    UiControlSample sample{};
    sample.frame = frame;
    // The coordinate space the rectangles are in: the VIEW.  The port knows
    // its own view size, and the rectangles are in it (measured: the live
    // BTN_LEFT_CAR rectangle matches the .gui transform times the view).
    sample.viewportW = gfxViewWidth();
    sample.viewportH = gfxViewHeight();
    g_viewW = sample.viewportW;
    g_viewH = sample.viewportH;

    // The manager and its screen registry.
    uint64_t manager = 0;
    if (!readU64(g + kManagerOff, &manager) ||
        !pointerSane(static_cast<uintptr_t>(manager))) {
        return;
    }
    uint64_t root = 0;
    if (!readU64(static_cast<uintptr_t>(manager) + kRegistryRootOff, &root) ||
        root == 0) {
        return;
    }

    ScreenEntry screens[kMaxScreens];
    int screenCount = 0;
    collectRegistry(static_cast<uintptr_t>(root), screens, &screenCount, 0);
    sample.screensRegistered = screenCount;
    if (screenCount == 0) {
        return;
    }

    // WHICH SCREEN IS LIVE.
    //
    // The registry holds every screen the guest knows, but only the ones that
    // are up have a component tree with children.  So each screen's CHILDREN
    // container is read first (three words each, so the whole sweep is cheap)
    // and only the trees that have any are walked.
    int liveScreens = 0;
    char liveName[64] = {0};
    std::vector<UiControl> bestControls;
    for (int i = 0; i < screenCount; ++i) {
        uintptr_t children = 0;
        int childCount = 0;
        if (!readChildren(screens[i].instance, &children, &childCount) ||
            childCount == 0) {
            continue;
        }
        ++liveScreens;
        std::vector<UiControl> controls;
        WalkState state{};
        state.controls = &controls;
        walkComponent(screens[i].instance, &state, 0);
        if (controls.empty()) {
            continue;
        }
        if (controls.size() > bestControls.size()) {
            bestControls.swap(controls);
            std::memcpy(liveName, screens[i].name, sizeof(liveName));
        }
    }
    sample.screensLive = liveScreens;
    if (bestControls.empty()) {
        ++g_walkCount;
        if (g_walkCount % 10ULL == 1ULL) {
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                          "f=%llu screen=- controls=0 screens=%d live=%d "
                          "vp=%dx%d why=no-live-component-tree",
                          frame, sample.screensRegistered, liveScreens,
                          sample.viewportW, sample.viewportH);
            appendGuestBootLine(kGuestViewLogPath, "ui-ctl", 130, 0, detail);
        }
        // The one-shot layout diagnostic: what the probe actually saw.
        if (!g_layoutLogged) {
            g_layoutLogged = true;
            logLayoutDiagnostic(screens, screenCount, frame);
        }
        return;
    }

    g_controls.swap(bestControls);
    sample.count = static_cast<int>(g_controls.size());
    sample.items = g_controls.data();
    std::memcpy(sample.screen, liveName, sizeof(sample.screen));
    sample.ok = true;
    g_sample = sample;

    // LOG ONLY WHEN SOMETHING CHANGED.
    const uint64_t hash = hashControls(g_controls);
    const bool changed = (std::strcmp(g_lastScreen, sample.screen) != 0) ||
                         (g_lastCount != sample.count) || (g_lastHash != hash);
    if (changed) {
        std::memcpy(g_lastScreen, sample.screen, sizeof(g_lastScreen));
        g_lastCount = sample.count;
        g_lastHash = hash;
        logSample(frame, sample);
    }
}

const UiControlSample& uiControlSample() {
    return g_sample;
}

bool uiControlFind(const char* name, int* x, int* y, int* w, int* h) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    // A PRESSABLE match wins over any match: a control the guest has
    // animated off screen is not one the port may press.
    for (int pass = 0; pass < 2; ++pass) {
        for (const UiControl& control : g_controls) {
            if (std::strcmp(control.name, name) != 0) {
                continue;
            }
            if (pass == 0 && !isPressable(control)) {
                continue;
            }
            if (x != nullptr) { *x = control.x; }
            if (y != nullptr) { *y = control.y; }
            if (w != nullptr) { *w = control.w; }
            if (h != nullptr) { *h = control.h; }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// THE CARD'S REQUESTS
//
// rr3-ui.txt is read ONCE per run, the way rr3-action.txt is.  The port only
// ever presses a control the card NAMED.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kUiRequestPath =
    "sdmc:/switch/rr3-native-rehost/rr3-ui.txt";
constexpr int kMaxRequests = 32;

struct UiRequest {
    unsigned long long at{};
    char name[64]{};
    bool served{};
};

UiRequest g_requests[kMaxRequests];
int g_requestCount = 0;
bool g_requestsRead = false;

bool isNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

void readRequestFile() {
    g_requestsRead = true;
    g_requestCount = 0;
    FILE* file = std::fopen(kUiRequestPath, "rb");
    if (file == nullptr) {
        return;
    }
    char line[256];
    while (g_requestCount < kMaxRequests &&
           std::fgets(line, sizeof(line), file) != nullptr) {
        char* text = line;
        char* hash = std::strchr(text, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        for (char* p = text; *p != '\0'; ++p) {
            if (*p == '\r' || *p == '\n') {
                *p = '\0';
                break;
            }
        }
        // The optional "at=<frame>" token may sit before OR after the name.
        unsigned long long at = 0;
        char* atText = std::strstr(text, "at=");
        if (atText != nullptr) {
            at = std::strtoull(atText + 3, nullptr, 10);
            char* end = atText;
            while (*end != '\0' && *end != ' ' && *end != '\t') {
                ++end;
            }
            char* dst = atText;
            const char* src = end;
            while (*src != '\0') {
                *dst++ = *src++;
            }
            *dst = '\0';
        }
        char* name = text;
        while (*name == ' ' || *name == '\t') {
            ++name;
        }
        char* end = name + std::strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        *end = '\0';
        if (name[0] == '\0') {
            continue;
        }
        bool good = true;
        for (const char* p = name; *p != '\0'; ++p) {
            if (!isNameChar(*p)) {
                good = false;
                break;
            }
        }
        if (!good) {
            continue;
        }
        UiRequest& request = g_requests[g_requestCount];
        request.at = at;
        request.served = false;
        std::memcpy(request.name, name, sizeof(request.name));
        ++g_requestCount;
    }
    std::fclose(file);
}

}  // namespace

bool uiControlNextRequest(UiControl* out) {
    if (!g_requestsRead) {
        readRequestFile();
    }
    if (g_requestCount == 0) {
        return false;
    }
    for (int i = 0; i < g_requestCount; ++i) {
        UiRequest& request = g_requests[i];
        if (request.served || g_currentFrame < request.at) {
            continue;
        }
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        if (!uiControlFind(request.name, &x, &y, &w, &h)) {
            continue;
        }
        if (out != nullptr) {
            std::memcpy(out->name, request.name, sizeof(out->name));
            out->x = x;
            out->y = y;
            out->w = w;
            out->h = h;
        }
        return true;
    }
    return false;
}

void uiControlServeRequest(const char* name) {
    if (name == nullptr) {
        return;
    }
    for (int i = 0; i < g_requestCount; ++i) {
        if (std::strcmp(g_requests[i].name, name) == 0) {
            g_requests[i].served = true;
            return;
        }
    }
}

}  // namespace rr3
