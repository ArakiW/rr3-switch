// Backend selection, accounting and shader capture for the null graphics
// backend. See include/rr3/gfx_backend.hpp for the contract.
//
// Nothing here is on the Mesa path except one resolved-once boolean test.
//
// ---------------------------------------------------------------------------
// sdmc:/switch/rr3-gfx.conf -- the emulator opt-in file
// ---------------------------------------------------------------------------
// One line per key, "key=value", '#' starts a comment, blank lines ignored.
// A line with no '=' is the backend name, so the original one-word file
// ("null") still works exactly as before.
//
//   null                     -> null backend (the historical spelling)
//   backend=null | gfx=null  -> same
//   zeropage=1               -> EXPERIMENT: map one page at guest VA 0
//   zpmode=data | zpmode=ret -> fill the page with zpfill bytes, or with the
//                               AArch64 `ret` encoding (see below). Default:
//                               data, i.e. the original behaviour.
//   zp8=<n>                  -> 64-bit value to place at VA 0x8
//   zp1c0=<n>                -> 64-bit value to place at VA 0x1C0
//   zpfill=<byte>            -> fill the whole 4 KiB page with this byte first
//   zpstub=1                 -> point the word(s) at a real `ret` + log line
//                               compiled into the NRO's own text, instead of
//                               at an offset in the page. The page at VA 0
//                               cannot be made executable (see below), so
//                               this is the only landing pad that is known to
//                               be fetchable. Applies to whichever word has no
//                               explicit zp8 / zp1c0 value.
//   zpperm=1                 -> DANGEROUS: also try to add execute permission
//                               to the page. OFF by default because it kills
//                               the guest thread on Eden: both
//                               svcSetProcessMemoryPermission and
//                               svcSetMemoryPermission trip
//                               "assert virt_addr != 0" for VA 0, after which
//                               the main thread is gone (threads=1,
//                               total_imports=0, no LR ring). Only useful to
//                               reproduce that failure.
//   render-thread=main       -> the default, and the historical behaviour: the
//                               main thread calls onViewRenderJNI once per
//                               frame. Any value other than "worker" means
//                               this.
//   render-thread=worker     -> EXPERIMENT: the frame loop runs on a guest
//                               worker thread created through
//                               guestPthreadCreate (so the guest sees the
//                               right TLS and the census attributes it), and
//                               the main thread only waits. The question it
//                               answers is whether the never-returning second
//                               render call belongs to the MAIN THREAD or to
//                               the CALL. OFF by default: absent file, absent
//                               key and every other spelling keep the main
//                               thread on the loop, byte for byte.
//
// <n> is decimal or 0x-hex. The zero-page experiment is OFF unless the file
// says zeropage=1, so real hardware -- which has no such file -- is untouched.
// The two mapped words default to whatever zpfill left there, so
// "zeropage=1" + "zpfill=0xff" is the all-ones page and "zeropage=1" alone is
// the all-zero control page.
//
// zpmode=ret exists because the sweep proved the guest *calls through* the
// value it reads at +0x8 / +0x1C0: 0xffffffff there makes it branch to
// 0xffffffffffffffff. A page of `ret` (0xd65f03c0 little-endian, one per
// 4-byte slot) is therefore a valid landing pad at every offset in the page,
// and it is also what makes a pointer *into* the page -- "zp8=0x100" -- a
// working callback instead of a jump into unmapped memory. In ret mode the two
// words are forced to 0 unless the matching key is present, because the raw
// `ret` pattern would otherwise be read back as a 0xd65f03c0 code pointer.
//
// zpmode=ret still cannot be made executable on Eden, which is what zpstub
// exists to work around: svcSetProcessMemoryPermission rejects any perm above
// 0x5 (so RWX is unobtainable) and Eden's svcSetMemoryPermission path asserts
// on virt_addr == 0. The NRO's own .text needs no permission change at all.
//
// The page is only mapped by main.cpp (source/main.cpp, mapGuestZeroPage) and
// only when this file opts in; the mapping itself is never on a default path.

#include "rr3/gfx_backend.hpp"
#include "rr3/recovery_guard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr3 {
namespace {

// The guest-view log is the artefact Test-OnEden.ps1 archives, so every gfx
// line goes there rather than into a private file the harness would drop.
constexpr const char* kLog = "sdmc:/switch/rr3-native-rehost.guest-view.log";
constexpr const char* kGfxConf = "sdmc:/switch/rr3-gfx.conf";
constexpr const char* kDumpDir = "sdmc:/switch/";

// The rehost maps every guest image above 4 GiB (see the rebase breadcrumb,
// newBias=0x2106...), the NRO's own text sits at 0x08... Anything called from
// guest code is therefore reachable only through a pointer the guest holds.

GfxCounters s_counters;
GfxBackend s_backend = GfxBackend::kMesa;
const char* s_backendSource = "default";
bool s_resolved = false;
uint32_t s_nextObjectId = 0;
uint32_t s_nextLocation = 0;
uint32_t s_nextGlslIndex = 0;
uint32_t s_nextBinIndex = 0;

// ---- shader bookkeeping ---------------------------------------------------
struct ShaderInfo {
    uint32_t type = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
};
constexpr uint32_t kMaxShaders = 256;
ShaderInfo s_shaders[kMaxShaders];

// Bounded arena: the arena is only there to answer glGetShaderSource, every
// full source is already on disk by then.
constexpr size_t kArenaBytes = 192 * 1024;
char s_arena[kArenaBytes];
size_t s_arenaUsed = 0;

struct UnknownName {
    char name[48];
    uint32_t count;
};
constexpr uint32_t kMaxUnknown = 32;
UnknownName s_unknown[kMaxUnknown];
uint32_t s_unknownCount = 0;
uint32_t s_unknownOverflow = 0;

// Distinct glShaderSource return addresses. The guest's own shaders arrive
// through guestViewTracedGlShaderSource; the host's magenta-quad helper is the
// other possible caller, and telling them apart from the address is the only
// way to keep the dump honest about whose GLSL it is.
struct CallerSite {
    uintptr_t address;
    uint32_t count;
};
constexpr uint32_t kMaxCallers = 4;
CallerSite s_callers[kMaxCallers];
uint32_t s_callerCount = 0;
uint32_t s_callerOverflow = 0;

uint32_t noteCaller(uintptr_t address) {
    for (uint32_t i = 0; i < s_callerCount; ++i) {
        if (s_callers[i].address == address) {
            s_callers[i].count += 1;
            return i;
        }
    }
    if (s_callerCount < kMaxCallers) {
        s_callers[s_callerCount].address = address;
        s_callers[s_callerCount].count = 1;
        s_callerCount += 1;
        return s_callerCount - 1;
    }
    s_callerOverflow += 1;
    return kMaxCallers;
}

void writeLine(const char* phase, unsigned long long ordinal,
               unsigned long long target, const char* details) {
    appendGuestBootLine(kLog, phase, ordinal, target, details);
}

// ---- conf file (sdmc:/switch/rr3-gfx.conf) ---------------------------------
// Read once, by whoever asks first, and never logged from here: the
// gfx-backend line must keep its old position (first graphics call) so the
// default path stays exactly as it was.
struct ConfState {
    bool resolved = false;
    char backend[32] = {0};
    const char* backendSource = "default";
    char renderThread[16] = {0};
    // "nofb=1" -- never create the console framebuffer on the default NWindow.
    // Experiment knob for the Mesa eglInitialize wedge; see native_ui.cpp.
    bool skipFramebuffer = false;
    // "nogo=1" -- never bring the socket stack up, so every outbound
    // connection fails at once instead of hanging. See gfxOfflineNetwork().
    bool offlineNetwork = false;
    // "menugate=1" -- set Cloudcell stub byte 0x4C1 to 1. See gfxMenuGateOpen().
    bool menuGate = false;
    // "ccdone=1" -- call the captured Cloudcell continuation. See above.
    bool cloudcellComplete = false;
    // "ccmode=now|bg" -- WHERE the captured continuation is invoked from. See
    // gfxCloudcellCompleteMode() for why the default has to stay empty.
    char ccMode[8] = {0};
    // "ccdisp=0|1|2" -- how far the JNI call slots go toward the ManagedMethod
    // table. 0 (default) = no dispatch at all, the pre-2026-10-19 behaviour.
    // 1 = resolve the class and method and LOG the match without calling it.
    // 2 = actually invoke it.
    //
    // Default 0 and the middle rung both exist because the managed stubs blr
    // back into guest code (aam_login runs LoginCompleteCallback through
    // cloudcell_defer), and a run on 2026-10-04 recorded entered_total=1 with
    // drained_total=0 -- a callback that went in and never came back. Level 1
    // answers "which methods would fire, and would the name match" for a full
    // 300 s run without ever executing one.
    int ccDispatch = 0;
    // "objid=1" -- hand out one real handle per NewObjectV call instead of the
    // single shared &g_fakeObjectObject. Default 0, which is the pre-2026-10-19
    // behaviour and the reason two separate defects exist:
    //
    //   * jniGetObjectClass answers the class sentinel for every non-Font
    //     object, so instance methods (login, setUserIdentifier) arrive with
    //     no class at all and only reach a facade through a name-only guess.
    //   * any ManagedMethod that writes instance fields writes into that ONE
    //     byte-wide global. aam_constructor stores two jlongs, so executing it
    //     would put 16 bytes into 1 byte of host globals.
    //
    // This changes every object address the guest sees. jni_bridge.cpp records
    // at line ~1290 that the guest spins on repeated identical returns, so this
    // is measured A/B rather than assumed safe, and ccdisp=2 refuses to run
    // while this is off.
    bool objectIdentity = false;
    // "mkdiridem=1" -- guest mkdir returns success when the directory already
    // exists. The guest logs "[FileManager::CreateDir] Error creating
    // directory: .../.depot/CC_Data" because the HOST staged that directory
    // from the data packs, where on real hardware the game would create it
    // itself and succeed. Default 0: plain POSIX mkdir, EEXIST and all.
    bool mkdirIdempotent = false;
    // "ccloginok=1" -- the Cloudcell account manager answers LoginCompleteCallback
    // with SUCCESS rather than with an honest failure.
    //
    // MEASURED 2026-10-19, archive 20261019-210729-exec1. With the failure
    // answer the guest stopped complaining "Invalid DeviceId!" -- so the
    // callback is reached and is understood -- but then it spun on
    // "Sub Sync Queued: 10765" and the run died at frame 4. It retries the
    // queue because the login failed, not because the callback was missing.
    //
    // The user authorised a simulated Cloudcell backend ("只要能玩，没有限制",
    // "授权，直接开做") and EA's service is gone, so a local success is the
    // only outcome the guest will accept. Off by default.
    bool cloudcellLoginOk = false;
    // "ccsocialok=1" -- the social-media session reports itself valid.
    //
    // MEASURED 2026-10-19, archive 20261019-212333-loginok2. The guest's
    // Cloudcell object asks whether the social-media session is valid and the
    // answer is FacebookWorker.GetSessionValid() /
    // GooglePlayWorker.GetSessionValid(), which are registered in
    // cc_services.cpp and return false. The guest then logs "Unable to send
    // Sync, Social Media not initialised." and spins its sync queue to 10765.
    //
    // In the guest's own Java, GetSessionValid() is "!GetAccessToken().isEmpty()"
    // (FacebookWorker.java:129) -- there is no real Facebook session here and
    // never will be. Off by default.
    bool cloudcellSocialOk = false;
    // "rookiehub=1" -- answer the guest's Firebase Remote Config query
    // Platform.GetBooleanValueFromRemoteConfig("enable_rookie") with TRUE.
    //
    // WHY THIS IS THE MAIN-MENU GATE, from the guest binary and nothing else:
    //
    //   FrontEnd2::EventMapScreen's screen-construction routine
    //   (FDE [0xdacf08,0xdad624)) picks between two roots for the same slot:
    //
    //     root A  main_menu/main_menu_twosquare.xml   loaded at 0xdacf84
    //     root B  main_menu/main_menu_career_page.xml loaded at 0xdad680, from
    //              the caller at 0xdac5a4-0xdac5d4, when the predicate is FALSE
    //
    // and the predicate is 0x13833cc([this+0x4c0]):
    //
    //     0x13833d4  ldrb w8, [x0, #8]     ; != 0
    //     0x13833dc  ldr  w8, [x0, #0xc]   ; != 5
    //     0x13833e8  bl   0x10ab9a8       ; G + 0x478
    //     0x13833ec  ldr  w8, [x0, #4]    ; *(G+0x47c) != 0x14
    //
    // The object at this+0x4c0 is the cc::RookieChampionshipManager singleton
    // (published into .bss 0x1f95cd8 by its own constructor at 0x1382a74; the
    // EventMapScreen constructor at 0xdac038 reads the same GOT slot 0x1f6d840).
    //
    // Its +8 byte is written by its own initialiser at 0x1382c50, and THAT is
    // where the Remote Config key comes in:
    //
    //     0x1382ce4  "Rookie_Enable"       -> native map lookup (0x9a2530)
    //     0x1382d30  +8 = (lookup == 0)
    //     0x1382d40  if (lookup != 0) skip the JNI query
    //     0x1382d48  "enable_rookie"
    //     0x1382d70  bl 0x12b9db0 -> Platform.GetBooleanValueFromRemoteConfig
    //     0x1382d78  +8 = (that answer) & 1
    //
    // So +8 is 1 exactly when the guest's own Remote Config answer for
    // "enable_rookie" is TRUE, and the predicate's other two terms are already
    // satisfied (measured: mgr0c = 0 and *(G+0x47c) = 4). Off by default:
    // with the switch off the port answers FALSE for every remote-config
    // boolean, which is what it has always done.
    //
    // MEASURED 2026-10-24, AND IT IS NOT A WORKING LEVER YET: with
    // rookiehub=1 the hub still does not open, because THE GUEST NEVER ASKS.
    // The guest's own native map (the one 0x1382ce4 looks up first) already
    // holds "Rookie_Enable", so 0x1382d40 skips the JNI query entirely and
    // sets +8 = 0 at 0x1382d3c. In one measured run the guest asked
    // GetBooleanValueFromRemoteConfig for six DIFFERENT keys
    // (override_client_unlock_levels 10162 times, enable_mtx_tile,
    // enable_dynamic_ads, enable_optimized_logic, enable_experimental_logic,
    // enable_event_logs_android) and never once for enable_rookie.
    //
    // So this switch answers a question the guest is not asking. It is kept
    // because it is the correct implementation of the JNI contract and costs
    // nothing, but it is NOT a way to open the hub. The hub's root is chosen
    // by the predicate at 0x13833cc, and the thing that decides the predicate
    // is the guest's own Remote Config map, which the host does not feed.
    // See the note at jni_bridge.cpp's GetStringValueFromRemoteConfig: the
    // guest's config parse failure is internal to the guest.
    bool rookieHub = false;
    // "ccsynth=1" -- install the host's synthetic Cloudcell object. Default is
    // 0, which LETS THE GUEST BUILD ITS OWN. The guest's constructor
    // (0x1cbe03c) allocates 0x4d0 bytes, publishes the object into this very
    // global at 0x1cbe094, sets its own first word to the cc::ICloudcell
    // vtable 0x1f63b58 at 0x1cbe290, and sets its +0x4c1 availability byte to
    // 1 at 0x1cbe218. Replacing that with a hand-built table means every
    // entry the host guessed wrong is a null or a mis-typed pointer, and three
    // successive fixes for that each cost a 5-minute emulator run and each
    // was wrong. See docs/compose/spec/unblock-menu-loading.md S4.
    bool cloudcellSynthetic = false;
    // "mlstub=1" -- take menu-loader vtable slot 21 even when the guest's own
    // vtable is legal. See guest_view_probe.cpp for the measurement.
    // "glctxearly=1" -- bring up a GL context BEFORE the guest's early
    // initialisation, because the guest samples glGetString(GL_VERSION) during
    // initializeGuestDependencies (main.cpp:1110) and the host did not have a
    // current context until runGuestViewProbe (main.cpp:1176) -- 66 lines too
    // late. With no context Mesa returns NULL, sscanf never runs, and the
    // guest's GLES major version stays 0 for the rest of the process.
    //
    // The early context is surfaceless (EGL_NO_SURFACE), which the guest's own
    // EGL_KHR_surfaceless_context extension list says is supported. That is the
    // point of doing it this way: no surface is taken on the NWindow, so the
    // view probe's own ladder is untouched and cannot end up with two surfaces
    // on one window.
    bool earlyGlContext = true;
    // "gpuid=powervr|adreno|native" -- which GL identity the guest is told it
    // is running on. Default powervr because that is the one whose guest-side id
    // is 4, and the render loop at 0x12af190 does `cmp #4 / b.ne` and will not
    // advance the game state until the id reaches 4. See gfx_gles.cpp for the
    // substring table and for what the substitution does and does not claim.
    GpuIdProfile gpuIdProfile = GpuIdProfile::kPowerVR;
    // "mlstub=1" -- take menu-loader vtable slot 21 even when the guest's own
    // vtable is legal. See guest_view_probe.cpp for the measurement.
    bool forceMenuStub = false;
    bool zeroPage = false;
    unsigned long long zeroPageAt8 = 0;
    unsigned long long zeroPageAt1c0 = 0;
    unsigned int zeroPageFill = 0;
    bool zeroPageRet = false;
    bool zeroPageStub = false;
    bool zeroPagePerm = false;
    bool hasAt8 = false;
    bool hasAt1c0 = false;
    bool hasFill = false;
};
ConfState s_conf;

// ---- the screen size the port reports (rr3-screen.txt) ----------------------
//
// A SEPARATE FILE, for the reason gfx_text_probe.cpp gives for its own file:
// rr3-gfx.conf is read ONCE into a 4096-byte buffer and the card's copy is
// already 4087 bytes long, so one more line there would be dropped in
// silence -- the exact failure this project has already paid for once.
//
// AN ABSENT FILE MEANS "TODAY'S BEHAVIOUR".  That is the whole contract: a
// run with no rr3-screen.txt is byte-for-byte the run that was measured
// before, so this knob can never change a run that did not ask for it.
//
// WHY THE SCREEN SIZE IS A QUALITY KNOB AT ALL
// --------------------------------------------
// MEASURED 2026-10-28, archive 20261028-044415-eden-f1n (the guest's own log):
//
//   Searching for settings for device: AndroidHigh
//   Loaded settings file: AndroidHigh.plist
//   Res base: 1280x720
//   Res order:
//   (960.000000, 640.000000) - 1.500000 - gui_assets/960x640/
//   (480.000000, 320.000000) - 1.500000 - gui_assets/480x320/
//
// The 2D UI is therefore drawn from a 960x640 atlas into a 1280x720 frame:
// a 1.33x upscale, which is what "the picture is soft" is.  The HD volume
// ships a complete gui_assets/2048x1536 tree and the guest never picks it.
//
// THE REASON IS THE GUEST'S OWN FILTER, not the port.  In the guest's
// cc::ImageResManager (ELF 0x10f8cf8) every folder in the gui_assets listing
// is parsed with sscanf("%ux%u") and then KEPT only if
//
//     0x10f8ec8  ldur w8, [x29, #-0x54]   ; the width
//     0x10f8ecc  cmp  w8, w20             ; the SCREEN width
//     0x10f8ed0  b.ls #0x10f8ee0         ; width <= screenW  -> keep
//     0x10f8ed4  ldur w8, [x29, #-0x58]   ; the height
//     0x10f8ed8  cmp  w8, w21             ; the SCREEN height
//     0x10f8edc  b.hi  #0x10f8e8c        ; height > screenH  -> drop
//
// -- i.e. a folder survives iff width <= screenW OR height <= screenH.
// 2048x1536 fails both against a 1280x720 screen (2048 > 1280 and
// 1536 > 720) and is dropped before the sort, so the HD tree is never even a
// candidate.  The port cannot change that from the filesystem side: the
// folder is in the listing (measured: the port injects it, and the guest
// lists it) and it is still dropped.
//
// THE SCREEN SIZE IS THE ONLY LEVER, and it is shared with the 3D: the same
// w20/w21 are what the guest prints as "Res base" and what its screen manager
// creates ("Adding screen: 0, (1280 x 720)").  So raising it makes the 3D
// render target bigger too -- that is the cost, and it is why this is a
// measured A/B and not a silent change.
//
// The file takes "platform=WxH" (Platform.getScreenWidth/Height) and
// "ui=WxH" (CCUserInterfaceManager.getScreenWidth/Height), and
// "view=WxH" (the size the port tells the GUEST its view is).  All three
// default to 0, which means "answer the constant this port has always
// answered".
//
// WHY THERE IS A "view" KEY AT ALL
// -------------------------------
// MEASURED 2026-10-28: the guest's GUI atlas filter (ELF 0x10f8ec8) keeps a
// folder only when `width <= screenW || height <= screenH`, and the guest's
// screen comes from the size the port hands it in onViewChangedJNI (a
// hardcoded 1280x720 at source/guest_view_probe.cpp:2505).  With that size
// the 2048x1536 GUI tree on the HD volume fails BOTH tests and is dropped
// before the sort, so the 2D UI is a 960x640 atlas scaled up to 1280x720.
//
// A "view" larger than 1280x720 therefore changes what the guest reads: at
// 2048 wide the 2048x1536 tree passes the filter (2048 <= 2048).  The 3D
// render target follows the same number, and the present path already
// renders into the guest's own framebuffer and blits it out, so the blit
// becomes a scaled one.
constexpr const char* kScreenConf = "sdmc:/switch/rr3-native-rehost/rr3-screen.txt";

struct ScreenSizeState {
    bool resolved = false;
    int platformWidth = 0;
    int platformHeight = 0;
    int uiWidth = 0;
    int uiHeight = 0;
    int viewWidth = 0;
    int viewHeight = 0;
};
ScreenSizeState s_screen;

char* trimText(char* text);

// Parses "WxH". Returns false for anything else, so a typo leaves the
// default in place instead of answering a width of zero.
bool parseScreenSize(const char* value, int* width, int* height) {
    if (value == nullptr || width == nullptr || height == nullptr) {
        return false;
    }
    char* end = nullptr;
    const long w = std::strtol(value, &end, 10);
    if (end == nullptr || *end != 'x' || w <= 0 || w > 8192) {
        return false;
    }
    const char* rest = end + 1;
    char* end2 = nullptr;
    const long h = std::strtol(rest, &end2, 10);
    if (end2 == nullptr || *end2 != '\0' || h <= 0 || h > 8192) {
        return false;
    }
    *width = static_cast<int>(w);
    *height = static_cast<int>(h);
    return true;
}

void readScreenConf() {
    s_screen = ScreenSizeState{};
    s_screen.resolved = true;
    std::FILE* file = std::fopen(kScreenConf, "rb");
    if (file == nullptr) {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* eq = std::strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char* key = trimText(line);
        char* value = trimText(eq + 1);
        int width = 0;
        int height = 0;
        if (std::strcmp(key, "platform") == 0) {
            if (parseScreenSize(value, &width, &height)) {
                s_screen.platformWidth = width;
                s_screen.platformHeight = height;
            }
        } else if (std::strcmp(key, "ui") == 0) {
            if (parseScreenSize(value, &width, &height)) {
                s_screen.uiWidth = width;
                s_screen.uiHeight = height;
            }
        } else if (std::strcmp(key, "view") == 0) {
            if (parseScreenSize(value, &width, &height)) {
                s_screen.viewWidth = width;
                s_screen.viewHeight = height;
            }
        }
    }
    std::fclose(file);
    // The run has to be able to say WHICH sizes it answered, or the log
    // cannot tell "the override did not apply" from "the override applied and
    // changed nothing".
    char details[128];
    std::snprintf(details, sizeof(details),
                  "platform=%dx%d ui=%dx%d file=%s",
                  s_screen.platformWidth, s_screen.platformHeight,
                  s_screen.uiWidth, s_screen.uiHeight, kScreenConf);
    writeLine("screen-override", 0, 0, details);
}

char* trimText(char* text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        ++text;
    }
    char* end = text + std::strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return text;
}

bool parseNumber(const char* text, unsigned long long* out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char* stop = nullptr;
    const unsigned long long value = std::strtoull(text, &stop, 0);
    if (stop == text) {
        return false;
    }
    *out = value;
    return true;
}

bool parseSwitch(const char* text) {
    return std::strcmp(text, "1") == 0 || std::strcmp(text, "on") == 0 ||
           std::strcmp(text, "true") == 0 || std::strcmp(text, "yes") == 0;
}

// "ccmode" is the only key here whose value is a word instead of a boolean, and
// its vocabulary is closed, so an unrecognised value has to RESOLVE rather than
// be ignored: a typo in a file that lives on an SD card and is edited by hand
// is otherwise indistinguishable from the default in the log.
//
// The value is lowercased first because the key parser above lowercases keys
// but never values, and "NOW" is the same answer as "now". Anything else --
// including an empty value -- resolves to "off", which is the behaviour from
// before this key existed: the capture site does nothing and the frame-loop
// site still runs, under "ccdone=1".
void parseCompleteMode(const char* text, char* out, size_t capacity) {
    char lowered[8] = {0};
    size_t n = 0;
    if (text != nullptr) {
        for (; n + 1 < sizeof(lowered) && text[n] != '\0'; ++n) {
            const char c = text[n];
            lowered[n] = (c >= 'A' && c <= 'Z')
                             ? static_cast<char>(c - 'A' + 'a')
                             : c;
        }
    }
    const bool known = std::strcmp(lowered, "now") == 0 ||
                       std::strcmp(lowered, "bg") == 0;
    std::snprintf(out, capacity, "%s", known ? lowered : "off");
}

// Set by readConf() when the file has bytes past the buffer. Reported in the
// gfx-backend summary line so a truncated conf is visible in the log rather
// than being a switch that quietly did nothing.
static bool s_confTruncated = false;
// Bytes actually consumed from the file. Echoed in the summary line so "the key
// I set had no effect" can be told apart from "the key never arrived" without
// having to go and measure the SD card by hand.
static size_t s_confBytes = 0;

void readConf() {
    if (s_conf.resolved) {
        return;
    }
    s_conf.resolved = true;

    // Eden does not reliably hand the host environment to the guest process, so
    // the file is the only opt-in that reaches the emulator. Absent file =
    // absent override = Mesa and no zero page, exactly as before.
    //
    // The original one-word file must be written WITHOUT a trailing newline and
    // must contain exactly "null": every ordinary way of creating it
    // (`Set-Content`, `echo`, a GUI editor) adds a newline, and the previous
    // parser only stripped *leading* whitespace, so every one of those silently
    // fell back to Mesa. Both ends are stripped here, per line, so the opt-in
    // cannot be a trap.
    std::FILE* f = std::fopen(kGfxConf, "rb");
    if (f == nullptr) {
        return;
    }
    // SIZE, because 256 was a silent landmine. This file is read ONCE into a
    // fixed buffer, so anything past the end is discarded without a word: a
    // conf carrying a documented header (the real one is 2.7 KB, almost all
    // comment) put every setting below byte 256 into the void, and two A/B runs
    // on 2026-10-16 measured the SAME configuration twice because the switch
    // being tested sat at offset 674. A switch that silently does nothing is
    // indistinguishable from a switch that does nothing useful, and that is the
    // one failure mode this parser must never have again -- hence the explicit
    // truncation flag below, which is reported in the gfx-backend summary line.
    char buffer[4096] = {0};
    const size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    // Anything still queued after the last byte we took means the tail was cut.
    s_confTruncated = (std::fgetc(f) != EOF);
    s_confBytes = read;
    std::fclose(f);
    buffer[read] = '\0';

    char* cursor = buffer;
    while (cursor != nullptr && cursor[0] != '\0') {
        char* line = cursor;
        char* next = std::strchr(line, '\n');
        if (next != nullptr) {
            *next = '\0';
            cursor = next + 1;
        }
        else {
            cursor = nullptr;
        }
        char* begin = trimText(line);
        if (begin[0] == '\0' || begin[0] == '#') {
            continue;
        }
        char* eq = std::strchr(begin, '=');
        char* value = begin;
        char* key = nullptr;
        if (eq != nullptr) {
            *eq = '\0';
            value = trimText(eq + 1);
            key = trimText(begin);
            for (char* c = key; *c != '\0'; ++c) {
                if (*c >= 'A' && *c <= 'Z') {
                    *c = static_cast<char>(*c - 'A' + 'a');
                }
            }
        }
        if (key == nullptr || std::strcmp(key, "backend") == 0 ||
            std::strcmp(key, "gfx") == 0 || std::strcmp(key, "rr3_gfx") == 0) {
            // "gfx=null" / "RR3_GFX=null" / "backend=null" / bare "null".
            if (value[0] != '\0' && s_conf.backend[0] == '\0') {
                std::snprintf(s_conf.backend, sizeof(s_conf.backend), "%s", value);
                s_conf.backendSource = "file";
            }
        }
        else if (std::strcmp(key, "nofb") == 0 ||
                 std::strcmp(key, "no-fb") == 0 ||
                 std::strcmp(key, "skip_framebuffer") == 0) {
            s_conf.skipFramebuffer = parseSwitch(value);
        }
        else if (std::strcmp(key, "nogo") == 0 ||
                 std::strcmp(key, "no-go") == 0 ||
                 std::strcmp(key, "offline") == 0) {
            s_conf.offlineNetwork = parseSwitch(value);
        }
        else if (std::strcmp(key, "menugate") == 0 ||
                 std::strcmp(key, "menu-gate") == 0) {
            s_conf.menuGate = parseSwitch(value);
        }
        else if (std::strcmp(key, "mlstub") == 0 ||
                 std::strcmp(key, "menu-stub") == 0) {
            s_conf.forceMenuStub = parseSwitch(value);
        }
        else if (std::strcmp(key, "ccdone") == 0 ||
                 std::strcmp(key, "cc-done") == 0) {
            s_conf.cloudcellComplete = parseSwitch(value);
        }
        else if (std::strcmp(key, "ccsynth") == 0 ||
                 std::strcmp(key, "cc-synth") == 0) {
            s_conf.cloudcellSynthetic = parseSwitch(value);
        }
        else if (std::strcmp(key, "glctxearly") == 0 ||
                 std::strcmp(key, "gl-context-early") == 0) {
            s_conf.earlyGlContext = parseSwitch(value);
        }
        else if (std::strcmp(key, "gpuid") == 0 ||
                 std::strcmp(key, "gpu-id") == 0 ||
                 std::strcmp(key, "gpu-profile") == 0) {
            if (std::strcmp(value, "native") == 0 || std::strcmp(value, "real") == 0) {
                s_conf.gpuIdProfile = GpuIdProfile::kNative;
            } else if (std::strcmp(value, "adreno") == 0) {
                s_conf.gpuIdProfile = GpuIdProfile::kAdreno;
            } else {
                s_conf.gpuIdProfile = GpuIdProfile::kPowerVR;
            }
        }
        else if (std::strcmp(key, "ccmode") == 0 ||
                 std::strcmp(key, "cc-mode") == 0 ||
                 std::strcmp(key, "ccdeliver") == 0 ||
                 std::strcmp(key, "cc-deliver") == 0) {
            parseCompleteMode(value, s_conf.ccMode, sizeof(s_conf.ccMode));
        }
        else if (std::strcmp(key, "ccdisp") == 0 ||
                 std::strcmp(key, "cc-dispatch") == 0) {
            // Closed vocabulary 0/1/2 and an unrecognised value RESOLVES to 0
            // rather than being ignored, for the reason parseCompleteMode gives:
            // a typo in a hand-edited SD-card file is otherwise
            // indistinguishable from the default in the log.
            if (std::strcmp(value, "2") == 0) {
                s_conf.ccDispatch = 2;
            } else if (std::strcmp(value, "1") == 0) {
                s_conf.ccDispatch = 1;
            } else {
                s_conf.ccDispatch = 0;
            }
        }
        else if (std::strcmp(key, "objid") == 0 ||                 std::strcmp(key, "object-id") == 0 ||
                 std::strcmp(key, "obj-identity") == 0) {
            s_conf.objectIdentity = parseSwitch(value);
        }
        else if (std::strcmp(key, "mkdiridem") == 0 ||
                 std::strcmp(key, "mkdir-idempotent") == 0 ||
                 std::strcmp(key, "mkdirx") == 0) {
            s_conf.mkdirIdempotent = parseSwitch(value);
        }
        else if (std::strcmp(key, "ccloginok") == 0 ||
                 std::strcmp(key, "cc-login-ok") == 0 ||
                 std::strcmp(key, "loginsuccess") == 0) {
            s_conf.cloudcellLoginOk = parseSwitch(value);
        }
        else if (std::strcmp(key, "ccsocialok") == 0 ||
                 std::strcmp(key, "cc-social-ok") == 0 ||
                 std::strcmp(key, "socialok") == 0) {
            s_conf.cloudcellSocialOk = parseSwitch(value);
        }
        else if (std::strcmp(key, "rookiehub") == 0 ||
                 std::strcmp(key, "rookie-hub") == 0 ||
                 std::strcmp(key, "rookieenable") == 0) {
            s_conf.rookieHub = parseSwitch(value);
        }
        else if (std::strcmp(key, "zeropage") == 0) {
            s_conf.zeroPage = parseSwitch(value);
        }
        else if (std::strcmp(key, "zpmode") == 0) {
            s_conf.zeroPageRet = std::strcmp(value, "ret") == 0;
        }
        else if (std::strcmp(key, "zpstub") == 0) {
            s_conf.zeroPageStub = parseSwitch(value);
        }
        else if (std::strcmp(key, "zpperm") == 0) {
            s_conf.zeroPagePerm = parseSwitch(value);
        }
        else if (std::strcmp(key, "zp8") == 0) {
            s_conf.hasAt8 = parseNumber(value, &s_conf.zeroPageAt8);
        }
        else if (std::strcmp(key, "zp1c0") == 0) {
            s_conf.hasAt1c0 = parseNumber(value, &s_conf.zeroPageAt1c0);
        }
        else if (std::strcmp(key, "zpfill") == 0) {
            unsigned long long byte = 0;
            if (parseNumber(value, &byte) && byte <= 0xFFU) {
                s_conf.zeroPageFill = static_cast<unsigned int>(byte);
                s_conf.hasFill = true;
            }
        }
        else if (std::strcmp(key, "render-thread") == 0 ||
                 std::strcmp(key, "render_thread") == 0 ||
                 std::strcmp(key, "rr3_render_thread") == 0) {            if (value[0] != '\0' && s_conf.renderThread[0] == '\0') {
                std::snprintf(s_conf.renderThread, sizeof(s_conf.renderThread),
                              "%s", value);
            }
        }
    }
}

void resolveBackend() {
    if (s_resolved) {
        return;
    }
    s_resolved = true;
    readConf();

    char chosen[32] = {0};
    const char* source = nullptr;
    if (const char* env = std::getenv("RR3_GFX")) {
        if (env[0] != '\0') {
            std::snprintf(chosen, sizeof(chosen), "%s", env);
            source = "env";
        }
    }
    if (chosen[0] == '\0' && s_conf.backend[0] != '\0') {
        std::snprintf(chosen, sizeof(chosen), "%s", s_conf.backend);
        source = s_conf.backendSource;
    }

    // Anything that is not an explicit opt-in keeps the Mesa path: real
    // hardware must never be switched to the null backend by accident.
    //
    // HISTORY, because both readings of this line were wrong once.
    //
    // 2026-10-11, "null is load-bearing": sdmc:/switch/rr3-gfx.conf held
    // backend=null and a four-run A/B (gui2/gui3 null vs gui4/gui5 mesa) said
    // Mesa killed the guest -- threads 6 -> 1, total_imports 28541 -> 385.
    // That was correct, and "deleting the leftover backend=null line" was the
    // single most damaging thing anyone did to this port that day.
    //
    // 2026-10-11, LATER: it was NOT Mesa. Mesa was hanging in eglInitialize
    // because nativeUiInit()'s framebufferCreate on the default NWindow, closed
    // by nativeUiExit(), poisons that NWindow for the rest of the process --
    // see the nofb key below and archive 20261011-1313*mesa2. With nofb=1:
    //
    //     view-egl-init-begin -> init-ok -> config-ok -> surface-ok
    //                          -> context-ok -> current-ok
    //     threads=6 moving=3 total_imports=28551 mutexes_known=75
    //
    // i.e. Mesa is now indistinguishable from the old null baseline, except
    // that null only counted GL calls and never forwarded them, so nothing
    // ever reached a GPU. The card now runs "backend=mesa" + "nofb=1".
    //
    // So: an A/B that says "X breaks it" does not say "not X is safe". Two
    // variables were being blamed in turn here and only one was guilty.
    const bool null = chosen[0] != '\0' &&
                      (std::strcmp(chosen, "null") == 0 || std::strcmp(chosen, "0") == 0 ||
                       std::strcmp(chosen, "on") == 0);
    s_backend = null ? GfxBackend::kNull : GfxBackend::kMesa;
    s_backendSource = source != nullptr ? source : "default";

    char details[256];
    std::snprintf(details, sizeof(details),
                  " backend=%s source=%s value=%s dumpdir=%s zeropage=%d"
                  " ccmode=%s menugate=%d ccdone=%d ccdisp=%d conf_trunc=%d conf_bytes=%zu",
                  gfxBackendName(), s_backendSource,
                  chosen[0] != '\0' ? chosen : "(unset)", kDumpDir,
                  s_conf.zeroPage ? 1 : 0,
                  s_conf.ccMode[0] != '\0' ? s_conf.ccMode : "off",
                  s_conf.menuGate ? 1 : 0,
                  s_conf.cloudcellComplete ? 1 : 0,
                  s_conf.ccDispatch,
                  s_confTruncated ? 1 : 0, s_confBytes);
    writeLine("gfx-backend", 300, static_cast<unsigned long long>(s_backend), details);
}

void ensureResolved() {
    if (!s_resolved) {
        resolveBackend();
    }
}

void hexDump(char* out, size_t capacity, const void* data, size_t length) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    size_t used = 0;
    for (size_t i = 0; i < length && used + 4 < capacity; ++i) {
        const int written = std::snprintf(out + used, capacity - used, "%02x", bytes[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    out[used < capacity ? used : capacity - 1] = '\0';
}

const char* shaderStageSuffix(uint32_t shader) {
    if (shader < kMaxShaders) {
        const uint32_t type = s_shaders[shader].type;
        if (type == 0x8B31) return "vert"; // GL_VERTEX_SHADER
        if (type == 0x8B30) return "frag"; // GL_FRAGMENT_SHADER
    }
    return "unknown";
}

void rememberDumpName(const char* path) {
    const char* leaf = std::strrchr(path, '/');
    std::snprintf(s_counters.lastDump, sizeof(s_counters.lastDump), "%s",
                  leaf != nullptr ? leaf + 1 : path);
}

// Both format breakdowns are small fixed tables: enough to answer "which
// compressed families does this game actually ship" (the S3TC/ATC/PVRTC
// decode plan) without a map on a device with 1 GB of RAM.
void noteFormat(uint32_t* formats, uint64_t* counts, int slots, uint32_t format) {
    for (int i = 0; i < slots; ++i) {
        if (formats[i] == 0) {
            formats[i] = format != 0 ? format : 1;
            counts[i] = 1;
            return;
        }
        if (formats[i] == format) {
            counts[i] += 1;
            return;
        }
    }
}

void recordCompressedFormat(uint32_t format) {
    s_counters.compressedTex += 1;
    noteFormat(s_counters.compFmt, s_counters.compFmtCount, 6, format);
}

} // namespace

// ---- public API ------------------------------------------------------------

GfxBackend gfxBackend() {
    ensureResolved();
    return s_backend;
}

bool gfxNullActive() {
    ensureResolved();
    return s_backend == GfxBackend::kNull;
}

const char* gfxBackendName() {
    return gfxBackend() == GfxBackend::kNull ? "null" : "mesa";
}

const char* gfxBackendSource() {
    ensureResolved();
    return s_backendSource;
}

// Experiment knob for the Mesa eglInitialize wedge: when true, the console
// never creates a framebuffer on the default NWindow. See native_ui.cpp for
// the measurement this isolates. The conf file is parsed once, on first use.
bool gfxSkipFramebufferUi() {
    ensureResolved();
    return s_conf.skipFramebuffer;
}

// "nogo=1": never initialise the socket stack, so every outbound connection the
// guest attempts fails immediately instead of hanging.
//
// Measured 2026-10-12 (archive 20261012-022344-draw1). The guest makes exactly
// ONE network request for the whole run and never issues another, with no
// timeout and no retry:
//     CC: [1] POST request [https://connect-dev-ss.cloudcell.com:443/time]
//     CC: Invalid DeviceId! Have we connected to cloudcell yet?     (repeats)
// while it renders a flat loading background 46,630 times and splashExit stays
// 0. It also reports "Connectivity::May use 3G? - true" -- it believes it has a
// network. So it blocks on a response that never arrives.
//
// The fix is to make the failure HONEST and IMMEDIATE, not to fake a success.
// Refusing to create sockets is the opposite of forging authentication: the
// guest is told there is no network, and can take whatever offline path it has.
// Nothing here impersonates Cloudcell or invents a session.
//
// This cannot live in symbol_resolver.cpp -- that file is frozen byte-for-byte,
// and censusGetaddrinfo/censusConnect live there. main.cpp owns the decision to
// bring the socket stack up at all, so the switch lives next to it.
//
// Acceptance, fixed before running:
//   guest logs a DNS/connect failure and splashExit advances -> this was it
//   nothing changes                                 -> the gate is elsewhere and
//                                                    the single POST is a red
//                                                    herring
bool gfxOfflineNetwork() {
    ensureResolved();
    return s_conf.offlineNetwork;
}

// "menugate=1": report byte 0x4C1 of the host's Cloudcell stub object as 1.
// This is the main-menu loader gate at guest 0x12aefd8. Off by default because
// the SAME byte gates async requests at 0xab2c30 and 0 is what keeps that one
// from wedging -- see the long note at guest_view_probe.cpp:2327.
bool gfxMenuGateOpen() {
    ensureResolved();
    return s_conf.menuGate;
}

// "ccdone=1": invoke the Cloudcell continuation the guest handed to the
// endpoint stub, which is the only path that clears `pending` at 0x1f970c0
// (0xab36f8 has zero direct callers). Off by default because the record it
// decodes carries an assert (`cmp w21,#2`) whose accepted shape is not yet
// confirmed, so this is an experiment, not a shipped behaviour.
bool gfxCloudcellCompleteRequests() {
    ensureResolved();
    return s_conf.cloudcellComplete;
}

// "ccdisp=0|1|2" -- how far the JNI call slots reach into the ManagedMethod
// table. See the ConfState field for why the middle rung exists at all.
int gfxCloudcellDispatch() {
    ensureResolved();
    return s_conf.ccDispatch;
}

// "objid=1": one real handle per NewObjectV, carrying its class and instance
// space. Gated separately from ccdisp because it changes guest-visible
// addresses, and because ccdisp=2 is unsafe without it.
bool gfxObjectIdentity() {
    ensureResolved();
    return s_conf.objectIdentity;
}

// "mkdiridem=1": guest mkdir treats an existing directory as success.
bool gfxMkdirIdempotent() {
    ensureResolved();
    return s_conf.mkdirIdempotent;
}

// "ccloginok=1": the Cloudcell account manager reports a successful login.
bool gfxCloudcellLoginOk() {
    ensureResolved();
    return s_conf.cloudcellLoginOk;
}

// "ccsocialok=1": the social-media session reports itself valid.
bool gfxCloudcellSocialOk() {
    ensureResolved();
    return s_conf.cloudcellSocialOk;
}

// "rookiehub=1": the guest's Firebase Remote Config answer for "enable_rookie"
// is TRUE, which is what makes the front end build the main_menu_twosquare
// root instead of the career page. See the ConfState field for the guest
// addresses that make the chain readable end to end.
bool gfxRookieHubEnabled() {
    ensureResolved();
    return s_conf.rookieHub;
}

// "ccmode=now|bg": WHICH CONTEXT invokes the Cloudcell continuation the guest
// hands to the endpoint stub, as opposed to WHETHER it is invoked (ccdone).
//
// The two questions are separate because the one delivery site that existed --
// the frame loop at guest_view_probe.cpp:2952 -- is structurally unreachable
// for the run this exists to fix: the guest blocks inside frame 0's first
// renderTarget(...) call, and that site is downstream of the very call the
// guest is stuck in. The continuation is captured twice during Cloudcell init
// and then nothing can ever run it. So this key moves the delivery point UP to
// the capture, and picks the thread it runs on:
//
//   off (default) -- unchanged. Nothing happens at the capture site and the
//                    frame-loop site is still the only one, so a run with no
//                    key behaves exactly as it did before this key existed.
//   now           -- invoke it synchronously, inside cloudcellEndpointStubRequest,
//                    right after the capture. Cheapest, and it removes the
//                    circularity completely: the guest is still inside the call
//                    that started it. The price is that guest code runs
//                    RE-ENTRANTLY inside Cloudcell init, which is exactly the
//                    invariant cloudcell_defer.h exists to protect, so this is
//                    the diagnostic mode, not the safe one.
//   bg            -- cloudcell_defer() it and let the existing alert-pump thread
//                    (jni_bridge.cpp:2508 calls cloudcell_pump("alert-pump",
//                    false) every ~80 ms, for the life of the process) drain the
//                    queue from a thread that IS running while the game thread is
//                    blocked. No new queue and no new thread, and it preserves
//                    the rule that no guest callback runs inline inside a
//                    Cloudcell lock.
//
// Answers "off" for an absent file, an absent key and every unrecognised value,
// so the default is this port's pre-switch behaviour and not an accident.
// Read it from any thread; readConf() is one-shot and resolves under the same
// guard as the backend selection.
// "ccsynth=0|1": whether the host installs its own Cloudcell object over the
// one the guest builds for itself. Default 0 -- let the guest build it.
//
// The guest's constructor publishes a fully constructed object into the same
// global at 0x1cbe094, with the real cc::ICloudcell vtable at its first word
// (0x1f63b58, confirmed by the RTTI typeinfo naming N2cc10ICloudcellE) and its
// own +0x4c1 availability byte already set to 1 at 0x1cbe218. Every entry in
// that table is the guest's own code, so nothing can be null and nothing can
// be mis-typed -- which is exactly the class of bug the synthetic table kept
// producing. Set 1 to restore the old substitution for comparison.
bool gfxCloudcellSynthetic() {
    ensureResolved();
    return s_conf.cloudcellSynthetic;
}

// "glctxearly=1": bring a GL context up before the guest's early init. Default
// 1 -- the guest samples glGetString(GL_VERSION) during
// initializeGuestDependencies and needs a current context to do it.
GpuIdProfile gfxGpuIdProfile() {
    ensureResolved();
    return s_conf.gpuIdProfile;
}

bool gfxEarlyGlContext() {
    ensureResolved();
    return s_conf.earlyGlContext;
}

// The screen size the port reports to the guest, or 0 for "answer the
// constant this port has always answered".  See the long note at
// readScreenConf() for why the screen size is the only lever on the GUI
// atlas resolution.
int gfxScreenWidthOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.platformWidth;
}

int gfxScreenHeightOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.platformHeight;
}

int gfxUiScreenWidthOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.uiWidth;
}

int gfxUiScreenHeightOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.uiHeight;
}

// The size the port tells the GUEST its view is (onViewChangedJNI).  0 means
// "the 1280x720 this port has always said".  See the long note at
// kScreenConf for why this is the only lever on the GUI atlas tree.
int gfxViewWidthOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.viewWidth;
}

int gfxViewHeightOverride() {
    if (!s_screen.resolved) {
        readScreenConf();
    }
    return s_screen.viewHeight;
}

// The size of the surface the guest actually renders into: the 1280x720
// window.  (On the Switch the framebuffer is 1280x720; on Eden the guest's
// window is that same framebuffer, and the emulator is the one that scales it
// into whatever size the host window is.)
constexpr int kRealSurfaceWidth = 1280;
constexpr int kRealSurfaceHeight = 720;

// The view size, clamped to the real surface, and SAID OUT LOUD when it is
// clamped.
//
// The guest asks the surface how big it is (eglQuerySurface) and sets its
// viewport to the answer, so a view bigger than the buffer draws only the
// part that fits: the picture lands in a corner and the rest is never drawn.
// MEASURED, run q67 (archive 20261101-020018-eden-rr3-q67-hd2048): with
// view=2048x1152 the guest's content occupies only the bottom left 1280x720
// of the buffer and the rest of it is blank.
//
// The port owns no offscreen target of the view size yet -- it does not own an
// FBO, and the guest renders into the default framebuffer (the guest binds
// fb=0 once at start-up and never binds an FBO again; see the fbo-bind lines
// in a guest-view log).  Until the port owns one, a bigger view is a broken
// picture, not a sharper one, so the view is CLAMPED.
//
// The clamp is LOUD on purpose: a silent clamp is the "silent fallback" that
// hides the difference between "the picture is 720p because that is what the
// hardware is" and "the picture is 720p because the config asked for more and
// the port could not deliver it".
void gfxViewSize(int* width, int* height) {
    int w = gfxViewWidthOverride() > 0 ? gfxViewWidthOverride() : kRealSurfaceWidth;
    int h = gfxViewHeightOverride() > 0 ? gfxViewHeightOverride() : kRealSurfaceHeight;
    if (w > kRealSurfaceWidth || h > kRealSurfaceHeight) {
        static bool s_loggedClamp = false;
        if (!s_loggedClamp) {
            s_loggedClamp = true;
            char details[224];
            __builtin_snprintf(
                details, sizeof(details),
                "view %dx%d clamped to %dx%d: the port owns no offscreen "
                "target of the view size, and a viewport bigger than the buffer "
                "draws the picture in a corner (measured, q67)",
                w, h, kRealSurfaceWidth, kRealSurfaceHeight);
            writeLine("view-clamped", 0, 0, details);
        }
        if (w > kRealSurfaceWidth) w = kRealSurfaceWidth;
        if (h > kRealSurfaceHeight) h = kRealSurfaceHeight;
    }
    if (width != nullptr) *width = w;
    if (height != nullptr) *height = h;
}

// The view size with the historical default applied.
int gfxViewWidth() {
    int w = kRealSurfaceWidth;
    int h = kRealSurfaceHeight;
    gfxViewSize(&w, &h);
    return w;
}

int gfxViewHeight() {
    int w = kRealSurfaceWidth;
    int h = kRealSurfaceHeight;
    gfxViewSize(&w, &h);
    return h;
}

// FIT, not FILL.
//
// The guest's view is scaled until it fits inside the output and the leftover
// space is left as bars.  A 4:3 view on a 16:9 output becomes 960x720
// centred in 1280x720; a 16:9 view on a 16:9 output is the whole screen and
// changes nothing about the historical path.
//
// The geometry itself is the inline gfxFitRect() in the header, so a host
// test can prove it without a screen.
GfxPresentRect gfxPresentRect(int outputWidth, int outputHeight) {
    return gfxFitRect(outputWidth, outputHeight, gfxViewWidth(),
                      gfxViewHeight());
}

const char* gfxCloudcellCompleteMode() {
    ensureResolved();
    return s_conf.ccMode[0] != '\0' ? s_conf.ccMode : "off";
}

bool gfxForceMenuLoaderStub() {
    ensureResolved();
    return s_conf.forceMenuStub;
}

// Opt-in frame-loop thread experiment. Declared here, next to the conf parser,
// rather than in the shared header: this key is an emulator-only thread-placement
// experiment, not part of the graphics contract (same rule as
// gfxZeroPageRequest below).
//
// gfxRenderThreadName() answers "main" for an absent file, an absent key and
// every value that is not "worker", so the default really is today's behaviour.
// Call it once, from the thread that owns the frame loop dispatch -- readConf()
// is the same one-shot parser the backend selection uses and is not reentrant.
const char* gfxRenderThreadName() {
    readConf();
    return s_conf.renderThread[0] != '\0' ? s_conf.renderThread : "main";
}

bool gfxRenderThreadIsWorker() {
    return std::strcmp(gfxRenderThreadName(), "worker") == 0;
}

// Opt-in zero-page experiment. Returns false (and fills the outputs with the
// parsed values, which are 0 by default) unless sdmc:/switch/rr3-gfx.conf
// says "zeropage=1". Declared in source/main.cpp rather than in the shared
// header: this key belongs to the emulator-only memory experiment and must
// not look like part of the graphics contract.
//
// `flags` is a bitmask because the two "was the key present at all" answers
// are what distinguish "zpmode=ret" (which forces both words to a real 0 so
// the guest still sees the natural null) from "zp8=0" (which is a deliberate
// request to write the null), and neither can be carried in the value alone:
// both spell 0 in the value.
enum GfxZeroPageFlag {
    kZeroPageHasAt8 = 0x1U,
    kZeroPageHasAt1c0 = 0x2U,
    kZeroPageRet = 0x4U,
    kZeroPageStub = 0x8U,
    kZeroPagePerm = 0x10U,
};

bool gfxZeroPageRequest(unsigned long long* at8, unsigned long long* at1c0,
                        unsigned int* fill, unsigned int* flags) {
    readConf();
    if (at8 != nullptr) {
        *at8 = s_conf.zeroPageAt8;
    }
    if (at1c0 != nullptr) {
        *at1c0 = s_conf.zeroPageAt1c0;
    }
    if (fill != nullptr) {
        *fill = s_conf.zeroPageFill;
    }
    if (flags != nullptr) {
        unsigned int bits = 0;
        if (s_conf.hasAt8) {
            bits |= kZeroPageHasAt8;
        }
        if (s_conf.hasAt1c0) {
            bits |= kZeroPageHasAt1c0;
        }
        if (s_conf.zeroPageRet) {
            bits |= kZeroPageRet;
        }
        if (s_conf.zeroPageStub) {
            bits |= kZeroPageStub;
        }
        if (s_conf.zeroPagePerm) {
            bits |= kZeroPagePerm;
        }
        *flags = bits;
    }
    return s_conf.zeroPage;
}

void gfxNoteVoidCall() {
    s_counters.calls += 1;
    // The swap-driven cadence never fires if the guest hangs before its first
    // frame, which is exactly the window worth looking at. A call-count
    // trigger costs one line per 64k calls and reports from anywhere.
    if ((s_counters.calls & 0xFFFFU) == 0) {
        gfxLogCounters("calls");
    }
}

void gfxNoteUnknownProc(const char* name) {
    if (name == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < s_unknownCount; ++i) {
        if (std::strcmp(s_unknown[i].name, name) == 0) {
            s_unknown[i].count += 1;
            return;
        }
    }
    bool fresh = false;
    if (s_unknownCount < kMaxUnknown) {
        std::snprintf(s_unknown[s_unknownCount].name,
                      sizeof(s_unknown[s_unknownCount].name), "%s", name);
        s_unknown[s_unknownCount].count = 1;
        s_unknownCount += 1;
        fresh = true;
    } else {
        s_unknownOverflow += 1;
    }
    if (fresh) {
        // First sighting is the whole point: the 321 string-only names sit
        // behind a glGetString(GL_EXTENSIONS) guard, so what the engine
        // actually asks for is the only evidence of which extensions it needs.
        // One line per new name, bounded by kMaxUnknown.
        char details[96];
        std::snprintf(details, sizeof(details), " name=%s answer=generic-stub", name);
        writeLine("gfx-proc", 304, s_unknownCount, details);
    }
}

void gfxNoteShaderCreated(uint32_t shader, uint32_t type) {
    ensureResolved();
    s_counters.shaders += 1;
    if (shader < kMaxShaders) {
        s_shaders[shader].type = type;
    }
}

void gfxRecordShaderSource(uint32_t shader, const char* joined, size_t length,
                           uintptr_t caller) {
    ensureResolved();
    s_counters.shaderSource += 1;
    s_counters.glslBytes += length;
    const uint32_t callerIndex = noteCaller(caller);
    s_counters.guestShaderSource = s_callerCount; // distinct call sites seen

    if (shader < kMaxShaders) {
        ShaderInfo& info = s_shaders[shader];
        if (s_arenaUsed + length + 1 <= kArenaBytes) {
            std::memcpy(s_arena + s_arenaUsed, joined, length);
            s_arena[s_arenaUsed + length] = '\0';
            info.offset = static_cast<uint32_t>(s_arenaUsed);
            info.length = static_cast<uint32_t>(length);
            s_arenaUsed += length + 1;
        }
    }

    char path[96];
    std::snprintf(path, sizeof(path), "%srr3-glsl-%03u-%s.glsl", kDumpDir,
                  s_nextGlslIndex, shaderStageSuffix(shader));
    s_nextGlslIndex += 1;

    bool wrote = false;
    if (std::FILE* f = std::fopen(path, "wb")) {
        wrote = std::fwrite(joined, 1, length, f) == length;
        std::fclose(f);
    }
    if (wrote) {
        s_counters.glslDumps += 1;
        rememberDumpName(path);
    }

    // The first few get a line of their own: a shader arriving at all is rare
    // and load-bearing, and 116 lines of glsl would drown the log.
    if (s_counters.shaderSource <= 4 || !wrote) {
        char head[64] = {0};
        if (joined != nullptr) {
            std::snprintf(head, sizeof(head), "%.48s", joined);
        }
        char details[256];
        std::snprintf(details, sizeof(details),
                      " backend=%s idx=%u shader=%u bytes=%zu caller=%u/%016llx "
                      "file=%s wrote=%d head=%s",
                      gfxBackendName(), s_nextGlslIndex - 1, shader, length,
                      callerIndex, (unsigned long long)caller,
                      wrote ? (std::strrchr(path, '/') + 1) : "(none)",
                      wrote ? 1 : 0, head);
        writeLine("gfx-glsl", 302, shader, details);
    }
}

void gfxRecordShaderBinary(uint32_t count, uint32_t format, const void* binary,
                           size_t length, uintptr_t caller) {
    ensureResolved();
    s_counters.shaderBinary += 1;
    s_counters.binBytes += length;
    if (s_counters.shaderBinary == 1) {
        s_counters.binFormat = format;
        if (binary != nullptr && length > 0) {
            hexDump(s_counters.firstBinMagic, sizeof(s_counters.firstBinMagic),
                    binary, length < 32 ? length : 32);
        }
    }

    char path[96];
    std::snprintf(path, sizeof(path), "%srr3-glslbin-%03u-fmt%04x.bin", kDumpDir,
                  s_nextBinIndex, format);
    s_nextBinIndex += 1;

    bool wrote = false;
    if (binary != nullptr && length > 0 && length <= 4u * 1024u * 1024u) {
        if (std::FILE* f = std::fopen(path, "wb")) {
            wrote = std::fwrite(binary, 1, length, f) == length;
            std::fclose(f);
        }
    }
    if (wrote) {
        s_counters.binDumps += 1;
        rememberDumpName(path);
    }

    char magic[80] = {0};
    if (binary != nullptr && length > 0) {
        hexDump(magic, sizeof(magic), binary, length < 32 ? length : 32);
    }
    char details[320];
    std::snprintf(details, sizeof(details),
                  " backend=%s idx=%u count=%u format=0x%04x bytes=%zu "
                  "caller=%016llx ptr=%016llx wrote=%d file=%s "
                  "magic=%s",
                  gfxBackendName(), s_nextBinIndex - 1, count, format, length,
                  (unsigned long long)caller,
                  (unsigned long long)(uintptr_t)binary, wrote ? 1 : 0,
                  wrote ? (std::strrchr(path, '/') + 1) : "(none)", magic);
    writeLine("gfx-shaderbin", 303, format, details);
}

const char* gfxShaderSource(uint32_t shader, size_t* length) {
    if (length != nullptr) {
        *length = 0;
    }
    if (shader >= kMaxShaders) {
        return nullptr;
    }
    const ShaderInfo& info = s_shaders[shader];
    if (info.length == 0 || s_arenaUsed == 0) {
        return nullptr;
    }
    if (length != nullptr) {
        *length = info.length;
    }
    return s_arena + info.offset;
}

uint32_t gfxNextObjectId() {
    s_nextObjectId += 1;
    if (s_nextObjectId == 0) {
        s_nextObjectId = 1;
    }
    return s_nextObjectId;
}

void gfxNoteProgramCreated() {
    s_counters.programs += 1;
}

void gfxNoteTexturesCreated(uint64_t count) {
    s_counters.textures += count;
}

void gfxNoteDrawArrays() {
    s_counters.drawArrays += 1;
}

void gfxNoteDrawElements() {
    s_counters.drawElements += 1;
}

void gfxNoteCompile() {
    s_counters.compileShader += 1;
}

void gfxNoteLink() {
    s_counters.linkProgram += 1;
}

void gfxNoteBufferBytes(uint64_t bytes) {
    s_counters.bufferBytes += bytes;
}

void gfxNoteTexImage(uint64_t width, uint64_t height, uint32_t internalFormat) {
    s_counters.texImage2D += 1;
    s_counters.texImagePixels += width * height;
    // Its own table, deliberately: the compressed table is the decode plan's
    // input and must not be diluted by uncompressed uploads.
    noteFormat(s_counters.texFmt, s_counters.texFmtCount, 4, internalFormat);
}

void gfxNoteCompressedTex(uint32_t internalFormat, uint64_t width, uint64_t height,
                          uint64_t bytes) {
    s_counters.compressedTexBytes += bytes;
    s_counters.compressedTexPixels += width * height;
    recordCompressedFormat(internalFormat);
}

void gfxNoteReadPixels(void) {
    s_counters.readPixels += 1;
}

uint32_t gfxNextLocation() {
    s_nextLocation += 1;
    if (s_nextLocation == 0) {
        s_nextLocation = 1;
    }
    return s_nextLocation;
}

uint64_t gfxFrameCount() {
    return s_counters.frames;
}

const GfxCounters& gfxCounters() {
    return s_counters;
}

void gfxLogCounters(const char* reason) {
    ensureResolved();
    const GfxCounters& c = s_counters;
    char comp[160] = {0};
    size_t used = 0;
    for (int i = 0; i < 6; ++i) {
        if (c.compFmt[i] == 0) {
            continue;
        }
        const int written = std::snprintf(comp + used, sizeof(comp) - used,
                                          "%s0x%04x:%llu", used != 0 ? "," : "",
                                          c.compFmt[i],
                                          (unsigned long long)c.compFmtCount[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    if (used == 0) {
        std::snprintf(comp, sizeof(comp), "none");
    }

    char texFmt[128] = {0};
    used = 0;
    for (int i = 0; i < 4; ++i) {
        if (c.texFmt[i] == 0) {
            continue;
        }
        const int written = std::snprintf(texFmt + used, sizeof(texFmt) - used,
                                          "%s0x%04x:%llu", used != 0 ? "," : "",
                                          c.texFmt[i],
                                          (unsigned long long)c.texFmtCount[i]);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    if (used == 0) {
        std::snprintf(texFmt, sizeof(texFmt), "none");
    }

    char unknown[256] = {0};
    used = 0;
    for (uint32_t i = 0; i < s_unknownCount && i < 8; ++i) {
        const int written =
            std::snprintf(unknown + used, sizeof(unknown) - used, "%s%s:%u",
                          used != 0 ? "," : "", s_unknown[i].name,
                          s_unknown[i].count);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    if (used == 0) {
        std::snprintf(unknown, sizeof(unknown), "none");
    }

    char callers[192] = {0};
    used = 0;
    for (uint32_t i = 0; i < s_callerCount; ++i) {
        const int written =
            std::snprintf(callers + used, sizeof(callers) - used, "%s%016llx:%u",
                          used != 0 ? "," : "",
                          (unsigned long long)s_callers[i].address,
                          s_callers[i].count);
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
    }
    if (used == 0) {
        std::snprintf(callers, sizeof(callers), "none");
    }

    char details[1024];
    std::snprintf(
        details, sizeof(details),
        " backend=%s src=%s reason=%s frames=%llu calls=%llu draws=%llu/%llu "
        "src=%llu/%llu glslDump=%llu bin=%llu/%llu fmt=0x%04x "
        "compile=%llu link=%llu prog=%llu sh=%llu tex=%llu vbo=%llu "
        "texImg=%llu/%llu[%s] compTex=%llu/%llu/%llu[%s] readPx=%llu last=%s "
        "unknown=%u[%s%s] magic=%s callers=[%s%s]",
        gfxBackendName(), gfxBackendSource(), reason != nullptr ? reason : "-",
        (unsigned long long)c.frames, (unsigned long long)c.calls,
        (unsigned long long)c.drawArrays, (unsigned long long)c.drawElements,
        (unsigned long long)c.shaderSource, (unsigned long long)c.glslBytes,
        (unsigned long long)c.glslDumps,
        (unsigned long long)c.shaderBinary, (unsigned long long)c.binBytes,
        c.binFormat, (unsigned long long)c.compileShader,
        (unsigned long long)c.linkProgram, (unsigned long long)c.programs,
        (unsigned long long)c.shaders, (unsigned long long)c.textures,
        (unsigned long long)c.bufferBytes, (unsigned long long)c.texImage2D,
        (unsigned long long)c.texImagePixels, texFmt,
        (unsigned long long)c.compressedTex, (unsigned long long)c.compressedTexBytes,
        (unsigned long long)c.compressedTexPixels, comp,
        (unsigned long long)c.readPixels, c.lastDump[0] != '\0' ? c.lastDump : "-",
        s_unknownCount, unknown, s_unknownOverflow != 0 ? ",+more" : "",
        c.firstBinMagic[0] != '\0' ? c.firstBinMagic : "-", callers,
        s_callerOverflow != 0 ? ",+more" : "");
    writeLine("gfx-stats", 301, c.frames, details);
}

void gfxTickFrame() {
    ensureResolved();
    s_counters.frames += 1;
    const uint64_t frame = s_counters.frames;
    // Front-load the signal (the interesting window is the first seconds) and
    // then settle to a ~2 s cadence at 60 Hz. Never per-call.
    bool log = frame <= 4 || frame == 8 || frame == 16 || frame == 32 ||
               frame == 64 || frame == 120 || frame == 240;
    if (frame > 240) {
        log = (frame % 120) == 0;
    }
    if (log) {
        gfxLogCounters("frame");
    }
}

} // namespace rr3
