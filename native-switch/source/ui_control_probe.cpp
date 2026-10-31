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
#include <rr3/tls_probe.hpp>

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

// ---- opening a screen by its name -----------------------------------------
//
// The guest's own FrontEnd2::Manager::OpenScreen, at ELF 0x14a4820.  The
// recipe is in the header; the short version is that it takes the manager,
// the screen INSTANCE and a flags word, and it is the only thing in the
// guest that pushes a screen onto the front end's stack.
//
// The guest's text ends here (the vtables start at 0x1e1aae0), so a function
// pointer inside [bias, bias + 0x1e19ae0) is a CODE pointer.  The check is
// what keeps a corrupt address from turning into a jump into the data
// segment; the port's named-command mechanism uses the same bound.
constexpr uintptr_t kOpenScreenVa = 0x14A4820ULL;
constexpr uintptr_t kGuestTextEndVa = 0x1E19AE0ULL;
// The guest's VTABLES are in .data.rel.ro, which is ABOVE the executable
// region:  .text 0x988000..0x1e17af0, .plt 0x1e17af0..0x1e19ae0,
// .data.rel.ro 0x1e1aae0..0x1f6cd80.
constexpr uintptr_t kVtableSectionLo = 0x1E1AAE0ULL;
constexpr uintptr_t kVtableSectionHi = 0x1F6CD80ULL;

// THE SCREEN'S XML LOADER: vtable slot 69.
//
// MEASURED 2026-10-31 (q48): opening a screen that had never been built
// CRASHED the guest -- writes to 0x30/0x34/0x48 of a null pointer, i.e. the
// guest walked a null component root while building the screen.  The missing
// step is the screen's own loader, and the guest calls it through this
// virtual:
//
//     0xdb8c90  EventMapScreen's slot 69:
//         0xdb8cb4  adrp x8, #0x1f6d000
//         0xdb8cb8  ldr  x8, [x8, #0x840]      ; the manager
//         0xdb8cbc  ldr  x8, [x8]
//         0xdb8cc0  mov  x0, x8
//         0xdb8cc4  bl  0x13833cc              ; the front-end gate
//         0xdb8cc8  tbz w0, #0, ...
//         0xdb8cd4  adrp x1, #0x449000          ; "EventMapScreen.xml"
//         0xdb8cd8  add  x1, x1, #0x765
//
// So the loader takes NO argument: each screen's own override knows its own
// XML file, and it checks the front-end gate before loading.  Slot 73 (the
// vtable entry at +0x248) is the screen's NAME getter, which the manager
// uses to find a screen by name.
//
// Slot 69 is at vtable + 69*8 = vtable + 0x228.
constexpr uintptr_t kScreenLoadSlot = 69;
constexpr uintptr_t kScreenNameSlot = 73;

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
bool g_displayNamesLogged = false;

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
    // The buffer holds every pressable control the walk found, because a
    // round is five minutes and the point of the log is to say WHICH
    // controls a driver could use.  Listing only the first ten made the
    // search for a target a guessing game.
    char detail[3072];
    int pressable = 0;
    for (int i = 0; i < sample.count; ++i) {
        if (isPressable(sample.items[i])) {
            ++pressable;
        }
    }
    int written = std::snprintf(detail, sizeof(detail),
                                "f=%llu screen=%s controls=%d pressable=%d "
                                "screens=%d live=%d vp=%dx%d",
                                frame, sample.screen[0] ? sample.screen : "-",
                                sample.count, pressable,
                                sample.screensRegistered, sample.screensLive,
                                sample.viewportW, sample.viewportH);
    // The PRESSABLE controls come first: they are the ones a driver can
    // actually use, so they are what a reader needs to see.
    int shown = 0;
    for (int pass = 0; pass < 2 && shown < 44; ++pass) {
        for (int i = 0; i < sample.count && shown < 44; ++i) {
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

// Finds one registered screen's INSTANCE by its name.
//
// The registry node holds the name at +0x20 and the instance at +0x38, so
// the walk compares the name it reads against the one asked for.  The walk
// stops at the first match, and the caller gets the instance the guest
// itself published -- not one the port guessed.
uintptr_t findRegistryInstance(uintptr_t node, const char* wanted,
                               int depth) {
    if (node == 0 || depth > 32 || wanted == nullptr || wanted[0] == '\0') {
        return 0;
    }
    char name[64] = {0};
    if (readGuestString(node + kTreeNameOff, name, sizeof(name)) &&
        std::strcmp(name, wanted) == 0) {
        uint64_t instance = 0;
        if (readU64(node + kTreeInstanceOff, &instance) &&
            pointerSane(static_cast<uintptr_t>(instance))) {
            return static_cast<uintptr_t>(instance);
        }
        return 0;
    }
    uint64_t left = 0;
    uint64_t right = 0;
    if (readU64(node + kTreeLeftOff, &left)) {
        const uintptr_t found =
            findRegistryInstance(static_cast<uintptr_t>(left), wanted, depth + 1);
        if (found != 0) {
            return found;
        }
    }
    if (readU64(node + kTreeRightOff, &right)) {
        return findRegistryInstance(static_cast<uintptr_t>(right), wanted,
                                    depth + 1);
    }
    return 0;
}

// THE SCREEN'S OWN NAME (vtable slot 73).
//
// It returns a plain `const char*` -- EventMapScreen's returns the literal
// "Stream Selection", which is NOT the name it is registered under.  The
// guest's own bookkeeping (ELF 0x14a44b8) takes this name, measures it
// (the PLT call at 0x1e17bf0 is a string-length function) and requires it to
// be SHORTER THAN 23 CHARACTERS, because it copies it into a 24-byte
// short-string buffer on the stack.
//
// So this is not a name lookup and not a table index: it is a LENGTH CHECK.
// And it is why a screen that has built its component tree can still take the
// guest down on the way in.
//
// The getter takes no argument and has no side effects, so calling it is
// safe; the copy is bounded and stops at the first byte that is not a
// printable character.
bool readScreenDisplayName(uintptr_t instance, uintptr_t vptr, char* out,
                           size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return false;
    }
    out[0] = '\0';
    uint64_t slot = 0;
    if (!readU64(vptr + kScreenNameSlot * 8, &slot) || slot == 0) {
        return false;
    }
    const uintptr_t bias = resolveMainImageLoadBias();
    if (bias == 0 || static_cast<uintptr_t>(slot) < bias ||
        static_cast<uintptr_t>(slot) >= bias + kGuestTextEndVa) {
        return false;
    }
    const char* text = nullptr;
    {
        using NameFn = const char* (*)(uintptr_t);
        GuestTlsScope tls;
        text = reinterpret_cast<NameFn>(static_cast<uintptr_t>(slot))(instance);
    }
    if (text == nullptr || !pointerSane(reinterpret_cast<uintptr_t>(text))) {
        return false;
    }
    size_t n = 0;
    for (; n + 1 < capacity; ++n) {
        uint64_t byte = 0;
        if (!readU64(reinterpret_cast<uintptr_t>(text) + n, &byte)) {
            break;
        }
        const char c = static_cast<char>(byte & 0xFF);
        if (c == '\0') {
            break;
        }
        if (c < 0x20 || c > 0x7E) {
            break;
        }
        out[n] = c;
    }
    out[n] = '\0';
    return n > 0;
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


// THE DISPLAY NAMES OF EVERY REGISTERED SCREEN, ONCE PER RUN.
//
// The front end's bookkeeping measures the name a screen calls itself (its
// vtable slot 73) and requires it to be shorter than 23 characters, so the
// port has to know those names to know which screens can be opened at all.
// One line, once, so a single round answers the question.
void logScreenDisplayNames(const ScreenEntry* screens, int screenCount,
                           uintptr_t manager, unsigned long long frame) {
    char line[3072];
    int written = std::snprintf(line, sizeof(line),
                                "f=%llu screens=%d mgr=0x%llx names=",
                                frame, screenCount,
                                static_cast<unsigned long long>(manager));
    for (int i = 0; i < screenCount && written > 0 &&
                    written < static_cast<int>(sizeof(line)) - 60; ++i) {
        uint64_t vptr = 0;
        if (!readU64(screens[i].instance, &vptr)) {
            continue;
        }
        char display[64] = {0};
        if (!readScreenDisplayName(screens[i].instance,
                                   static_cast<uintptr_t>(vptr), display,
                                   sizeof(display))) {
            continue;
        }
        written += std::snprintf(line + written,
                                 sizeof(line) - static_cast<size_t>(written),
                                 "%s=%s(%d);", screens[i].name, display,
                                 static_cast<int>(std::strlen(display)));
    }
    appendGuestBootLine(kGuestViewLogPath, "ui-names", 142, 0, line);
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

    // WHICH SCREEN IS LIVE, AND WHICH CONTROLS ARE IN PLAY.
    //
    // The registry holds every screen the guest knows, and MORE THAN ONE of
    // them is up at a time: the front end has the event map AND the main
    // menu AND the status icon bar, each with its own component tree.
    //
    // The first version of this probe walked only ONE tree -- the one with
    // the most named controls -- and that is why the main menu's toolbar
    // (BTN_TOOLBAR_COMMUNITY, BTN_TOOLBAR_PROFILE, ...) was never found:
    // the toolbar lives in the SettingsToolbar screen's tree, and the event
    // map's tree was bigger.  Measured in q45.
    //
    // So every live tree is walked and the controls are MERGED.  Merging is
    // safe because the pressable test is applied to each control on its own
    // (the centre has to be inside the view), so a control from a screen
    // that is not on top simply is not pressable.
    int liveScreens = 0;
    char liveName[64] = {0};
    int liveMost = 0;
    std::vector<UiControl> merged;
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
        if (static_cast<int>(controls.size()) > liveMost) {
            liveMost = static_cast<int>(controls.size());
            std::memcpy(liveName, screens[i].name, sizeof(liveName));
        }
        merged.insert(merged.end(), controls.begin(), controls.end());
    }
    sample.screensLive = liveScreens;
    if (!g_displayNamesLogged && liveScreens > 0) {
        g_displayNamesLogged = true;
        logScreenDisplayNames(screens, screenCount,
                              static_cast<uintptr_t>(manager), frame);
    }
    if (merged.empty()) {
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

    g_controls.swap(merged);
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

// ---------------------------------------------------------------------------
// OPENING A SCREEN BY ITS NAME
// ---------------------------------------------------------------------------
bool uiScreenOpen(const char* name, bool allowOpen) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const uintptr_t g = raceStateResolveGlobal();
    if (g == 0) {
        return false;
    }
    uint64_t manager = 0;
    if (!readU64(g + kManagerOff, &manager) ||
        !pointerSane(static_cast<uintptr_t>(manager))) {
        return false;
    }
    uint64_t root = 0;
    if (!readU64(static_cast<uintptr_t>(manager) + kRegistryRootOff, &root) ||
        root == 0) {
        return false;
    }
    const uintptr_t instance =
        findRegistryInstance(static_cast<uintptr_t>(root), name, 0);
    if (instance == 0) {
        char details[192];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" refused=1 reason=not-in-the-registry",
                      name);
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    // THE INSTANCE PROVES ITSELF BEFORE IT IS CALLED WITH.
    //
    // A screen object starts with its vtable pointer, and the guest's
    // vtables live in .data.rel.ro (0x1e1aae0 .. 0x1f6cd80 in the image).
    // So a first word that is NOT in that range is not a screen and is
    // refused here rather than called with.  This is a pure read.
    //
    // MEASURED 2026-10-31: the FIRST version of this check used the guest's
    // TEXT end (0x1e19ae0) as the vtable section's upper bound, which is
    // BELOW the vtable section's start -- an EMPTY range, so every instance
    // was refused and the reason line said "not a screen object" for screens
    // that are.  The sections of the image are:
    //
    //     .text        0x988000 .. 0x1e17af0
    //     .plt         0x1e17af0 .. 0x1e19ae0     <- the executable region ends
    //     .data.rel.ro 0x1e1aae0 .. 0x1f6cd80     <- THE VTABLES ARE HERE
    //
    // A predicate that is always true is a predicate that has never been
    // true, and it refuses everything.
    const uintptr_t bias0 = resolveMainImageLoadBias();
    uint64_t vptr = 0;
    if (bias0 == 0 || !readU64(instance, &vptr) ||
        static_cast<uintptr_t>(vptr) < bias0 + kVtableSectionLo ||
        static_cast<uintptr_t>(vptr) >= bias0 + kVtableSectionHi) {
        char details[224];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" inst=0x%llx refused=1 "
                      "reason=the-instance-is-not-a-screen-object",
                      name, static_cast<unsigned long long>(instance));
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    const uintptr_t bias = bias0;

    // BUILD THE SCREEN FIRST.
    //
    // The guest's own vtable slot 69 is the screen's XML loader, and a
    // screen that has never been built has no components.  Opening it
    // without building it walks a null root and kills the guest (measured in
    // q48), so the build comes first and it is VERIFIED: if the screen still
    // has no component tree afterwards, the open is refused rather than
    // attempted.
    uint64_t loadSlot = 0;
    if (!readU64(static_cast<uintptr_t>(vptr) + kScreenLoadSlot * 8, &loadSlot) ||
        static_cast<uintptr_t>(loadSlot) < bias ||
        static_cast<uintptr_t>(loadSlot) >= bias + kGuestTextEndVa) {
        char details[224];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" inst=0x%llx refused=1 "
                      "reason=no-usable-xml-loader-slot",
                      name, static_cast<unsigned long long>(instance));
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    {
        GuestTlsScope tls;
        using LoadScreenFn = void (*)(uintptr_t);
        reinterpret_cast<LoadScreenFn>(static_cast<uintptr_t>(loadSlot))(instance);
    }
    // AND CHECK THAT THE BUILD HAPPENED.
    uintptr_t builtChildren = 0;
    int builtCount = 0;
    if (!readChildren(instance, &builtChildren, &builtCount) || builtCount == 0) {
        char details[224];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" inst=0x%llx refused=1 "
                      "reason=the-screen-did-not-build",
                      name, static_cast<unsigned long long>(instance));
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    // AND THE NAME THE SCREEN CALLS ITSELF.
    //
    // The guest's own bookkeeping copies this name into a 24-byte short-string
    // buffer, so a name of 23 characters or more takes the error path.  That
    // is a length the port can measure BEFORE it calls anything.
    char display[64] = {0};
    const bool named = readScreenDisplayName(
        instance, static_cast<uintptr_t>(vptr), display, sizeof(display));
    const int displayLen = static_cast<int>(std::strlen(display));
    char built[320];
    std::snprintf(built, sizeof(built),
                  "screen=\"%.80s\" inst=0x%llx built=1 children=%d open=%d "
                  "display=\"%.60s\" displayLen=%d",
                  name, static_cast<unsigned long long>(instance), builtCount,
                  allowOpen ? 1 : 0, named ? display : "-", displayLen);
    appendGuestBootLine(kGuestViewLogPath, "ui-open", 141, 0, built);
    if (!named) {
        char details[224];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" refused=1 reason=no-display-name",
                      name);
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    if (displayLen >= 23) {
        char details[256];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" display=\"%.60s\" displayLen=%d "
                      "refused=1 reason=the-display-name-is-too-long-for-the-"
                      "guests-own-buffer",
                      name, display, displayLen);
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }

    // AND ONLY NOW, WITH THE CARD'S EXPLICIT "open=1", IS THE SCREEN OPENED.
    //
    // MEASURED 2026-10-31 (q51): with the screen built, OpenScreen was
    // called and the guest died.  So the open is still not proven, and it
    // must not be one keystroke away from a screen name.
    if (!allowOpen) {
        return true;
    }
    const uintptr_t fn = bias + kOpenScreenVa;
    if (bias == 0 || fn < bias || fn >= bias + kGuestTextEndVa) {
        char details[192];
        std::snprintf(details, sizeof(details),
                      "screen=\"%.80s\" refused=1 reason=not-in-the-guest-text",
                      name);
        appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, details);
        return false;
    }
    // The line BEFORE the call, so a call that takes the guest down still
    // leaves a record of WHICH screen was being opened.
    char before[256];
    std::snprintf(before, sizeof(before),
                  "screen=\"%.80s\" inst=0x%llx fn=0x%llx calling=1",
                  name, static_cast<unsigned long long>(instance),
                  static_cast<unsigned long long>(fn));
    appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, before);
    {
        // The guest's own thread and the guest's own TLS: this is a call INTO
        // the guest, exactly like the port's named-command mechanism in
        // source/guest_action.cpp.
        GuestTlsScope tls;
        using OpenScreenFn = void (*)(uintptr_t, uintptr_t, int);
        reinterpret_cast<OpenScreenFn>(fn)(static_cast<uintptr_t>(manager),
                                           instance, 0);
    }
    char after[288];
    std::snprintf(after, sizeof(after), "%s called=1", before);
    appendGuestBootLine(kGuestViewLogPath, "ui-open", 140, 0, after);
    return true;
}

namespace {

// ---- the card's screen list ------------------------------------------------
constexpr const char* kScreenRequestPath =
    "sdmc:/switch/rr3-native-rehost/rr3-open.txt";
constexpr int kMaxScreenRequests = 16;

struct ScreenRequest {
    char name[64]{};
    bool done{};
};

ScreenRequest g_screenRequests[kMaxScreenRequests];
int g_screenRequestCount = 0;
bool g_screenRequestsRead = false;
bool g_screenOpenAllowed = false;

void readScreenRequestFile() {
    g_screenRequestsRead = true;
    g_screenRequestCount = 0;
    FILE* file = std::fopen(kScreenRequestPath, "rb");
    if (file == nullptr) {
        return;
    }
    char line[256];
    while (g_screenRequestCount < kMaxScreenRequests &&
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
        char* name = text;
        while (*name == ' ' || *name == '\t') {
            ++name;
        }
        // THE "open=1" LINE IS WHAT TURNS THE OPEN ON.  Without it the port
        // only BUILDS the named screens and reports whether the build
        // worked, which is proven safe (q51); with it the port also calls the
        // guest's OpenScreen, which is NOT proven (q51 killed the guest).
        if (std::strncmp(name, "open=", 5) == 0) {
            g_screenOpenAllowed = (std::strcmp(name + 5, "1") == 0);
            continue;
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
        std::memcpy(g_screenRequests[g_screenRequestCount].name, name,
                    sizeof(g_screenRequests[g_screenRequestCount].name));
        g_screenRequests[g_screenRequestCount].done = false;
        ++g_screenRequestCount;
    }
    std::fclose(file);
}

}  // namespace

void pumpUiScreenOpen(unsigned long long frame) {
    if (!g_screenRequestsRead) {
        readScreenRequestFile();
    }
    if (g_screenRequestCount == 0) {
        return;
    }
    // THE FRONT END HAS TO BE UP.  Opening a screen while the guest is still
    // loading would push a screen onto a stack that does not exist yet, and
    // the probe's own sample says whether the front end is up: a live
    // component tree means it is.
    if (!g_sample.ok) {
        return;
    }
    // ONE SCREEN EVERY COUPLE OF SECONDS.  The guest has to BUILD the screen
    // it was given -- load its .gui, build its controls, lay it out -- and a
    // screen opened on top of the one before it has finished is a screen
    // that never appears.
    static unsigned long long lastOpenFrame = 0;
    constexpr unsigned long long kOpenPeriodFrames = 120ULL;
    if (frame < lastOpenFrame + kOpenPeriodFrames) {
        return;
    }
    lastOpenFrame = frame;
    for (int i = 0; i < g_screenRequestCount; ++i) {
        if (g_screenRequests[i].done) {
            continue;
        }
        if (!uiScreenOpen(g_screenRequests[i].name, g_screenOpenAllowed)) {
            // A screen that is not in the registry will never be there, so
            // the request is finished rather than retried forever.
            g_screenRequests[i].done = true;
            continue;
        }
        g_screenRequests[i].done = true;
        return;
    }
}

}  // namespace rr3
