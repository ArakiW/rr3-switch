#include "rr3/guest_filesystem.hpp"

#include "rr3/bionic_misc_shims.hpp"
#include "rr3/gfx_backend.hpp"
#include "rr3/guest_settings.hpp"
#include "rr3/recovery_guard.hpp"
#include "rr3/rr3_md5.hpp"
#include "bionic_errno.hpp"
#include "rr3_asset_candidates.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rr3 {
// A forward declaration for the SHARED OVERRIDE SET: the ONE set the base-list
// rewrite, the per-category gui list rewrite, the completed gui-assets
// manifest, the census, the inventory and the digest audit all read, so none
// of them can disagree about a path they all name. Its definition sits next
// to resolveAssetLocalFile(), the resolver it uses.
const std::vector<std::pair<std::string, std::string>>& sharedAssetOverrides();

// The rewritten copy of every asset list the port serves: leaf name -> the
// path on the card. Sorted by leaf, so a lookup is a binary search. Filled by
// buildRewrittenAssetLists() at boot; see the note on that function for why
// EVERY list the port serves has to be rewritten, not just the base one.
std::vector<std::pair<std::string, std::string>> gRewrittenLists;

// The path of the rewritten copy of the list the guest asks for, or nullptr
// when the port has no rewritten copy of it.
const char* rewrittenListPath(const char* guestPath);

namespace {

constexpr const char* kDataBase = "sdmc:/switch/rr3-native-rehost/data";
constexpr const char* kSaveRoot = "sdmc:/switch/rr3-native-rehost/save";
constexpr const char* kCacheRoot = "sdmc:/switch/rr3-native-rehost/cache";
constexpr const char* kLooseDataRoot =
    "sdmc:/switch/rr3-native-rehost/data/loose";
// Last successfully opened gui directory (for DT_UNKNOWN recovery in readdir).
const char* g_lastOpendirPath = nullptr;
// When listing the bare gui_assets folder, synthesize the ONE child the
// volumes do not list but the guest expects: 2048x1536 lives on the HD volume
// (rr3gui0) and does not appear in the SD listing; AndroidHigh.plist selects
// High → 2048x1536. contentDevice already routes 2048x1536 to rr3gui0.
//
// 1280x720 IS NOT INJECTED ANY MORE, and the reason is MEASURED, not
// preferred. The guest builds its "Res order:" OUT OF THE LISTING: in the
// archives from BEFORE the bionic-dirent fix (20261021-165605-eden-assetalias
// and its three neighbours) the guest printed
//
//     Res order:
//     share) Current res: 1280, 720
//     original [2] (960.000000, 640.000000) - 1.500000
//     ...
//
// -- an EMPTY "Res order:" while the current resolution WAS printed. So the
// current resolution is not added on its own; the entry the guest showed for
// 1280x720 came from the folder this file used to invent, and the fake entry
// put the guest on the one resolution the volumes ship NEITHER a tree NOR a
// leaf format for. The guest's own table already ranks 960x640 first
// ("actual [2] (960,640)"), so the resolution the guest actually wants is
// the one the volumes ship. See the long note at the injection site.
bool g_injectGui2048 = false;
int g_injectIndex = 0;
dirent g_fakeGuiEntry{};
// THE REAL CHILDREN OF THE CURRENT gui_assets LISTING, AND THE NAME THE PORT
// ADDED TO IT.
//
// The injection may only ADD a name the listing does not already contain.
// Before this was tracked, the core tree -- which really does ship
// files/.depot/gui_assets/2048x1536 -- was shown "2048x1536" TWICE, once as
// a real entry and once as the injected one (MEASURED, archive
// 20261023-175553-eden-vptr, guest-view.log L2228 and L2232).
char g_guiListingChildren[512]{};
char g_guiListingInjected[64]{};
bool g_guiListingOpen = false;

// THE NAMES ALREADY IN THE CURRENT gui_assets LISTING.
bool guiListingHasChild(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    const size_t n = std::strlen(name);
    for (const char* p = g_guiListingChildren; (p = std::strstr(p, name)) != nullptr;
         p += n) {
        const bool leftOk = (p == g_guiListingChildren) || (p[-1] == ' ');
        const bool rightOk = (p[n] == '\0') || (p[n] == ' ');
        if (leftOk && rightOk) return true;
    }
    return false;
}

void noteGuiListingChild(const char* name) {
    if (name == nullptr || name[0] == '\0') return;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) return;
    if (guiListingHasChild(name)) return;
    size_t used = std::strlen(g_guiListingChildren);
    const size_t n = std::strlen(name);
    if (used + n + 2 >= sizeof(g_guiListingChildren)) return;
    if (used != 0) g_guiListingChildren[used++] = ' ';
    std::memcpy(g_guiListingChildren + used, name, n + 1);
}

// THE LISTING THE GUEST WAS SHOWN, ONCE, WHEN THE LISTING ENDS.
//
// This answers "which resolution can the guest choose from" without having to
// read the guest's own banner: it names the real children of the gui_assets
// root and the one the port added to them.
void logGuiAssetsListing() {
    if (!g_guiListingOpen) return;
    g_guiListingOpen = false;
    char details[0x300];
    std::snprintf(details, sizeof(details),
                  "root=%s real=%s injected=%s",
                  g_lastOpendirPath != nullptr ? g_lastOpendirPath : "-",
                  g_guiListingChildren[0] != '\0' ? g_guiListingChildren : "-",
                  g_guiListingInjected[0] != '\0' ? g_guiListingInjected : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "gui-assets-listing", 0, 0, details);
}

// ---------------------------------------------------------------------------
// BIONIC'S struct dirent. The host's cannot be handed to the guest.
//
// MEASURED FROM THE GUEST BINARY, not from a header and not from a
// remembered Android layout. Four independent guest call sites read the
// directory entry at bionic's offsets:
//
//   0x12a5c60  add  x24, x25, #0x13   ; d_name
//   0x12a5c84  ldrb w8, [x25, #0x12]  ; d_type
//   0x12b553c  ldrb w8, [x25, #0x12]  ; d_type
//   0x12b5550  add  x25, x25, #0x13   ; d_name
//   0x1892b48  add  x25, x22, #0x13   ; d_name
//   0x1b5e12c  add  x1, x0, #0x13     ; d_name
//
// so the guest's dirent is {d_ino@0x00, d_off@0x08, d_reclen@0x10,
// d_type@0x12, d_name@0x13}: bionic's layout.
//
// The HOST's is devkitPro newlib's, and it was MEASURED with this repo's own
// build toolchain (aarch64-none-elf-gcc, devkitA64
// .../aarch64-none-elf/include/sys/dirent.h):
//
//     struct dirent { ino_t d_ino; unsigned char d_type; char d_name[NAME_MAX+1]; };
//
//     sizeof(ino_t)                  == 2
//     offsetof(struct dirent, d_type) == 2
//     offsetof(struct dirent, d_name) == 3
//     sizeof(struct dirent)          == 260
//
// NOTE THE TWO-BYTE ino_t: it is what puts d_type at 0x02 and d_name at 0x03
// rather than at 0x08/0x09. An earlier version of this comment claimed
// 0x08/0x09, and that claim was WRONG -- it was carried into
// bionicDirentLayoutSelfTest(), which then asserted the wrong offsets and
// therefore passed for the wrong reason. The static_asserts below are the
// guard that makes that impossible to repeat: they are checked by the
// compiler against the real header, on every build.
//
// THE CONSEQUENCE OF THE GAP. The guest reads d_type at 0x12 and d_name at
// 0x13. Against the host's real layout that is d_type = host d_name[0x12-3]
// = d_name[15] and d_name = host d_name[0x13-3] = d_name[16] -- the name
// the guest sees starts SIXTEEN BYTES into the host's name, and the type
// byte comes out of the middle of the name. So the port's old behaviour
// (returning the host's entry unchanged) made every directory entry
// INVISIBLE to the guest, and chopped exactly 16 bytes off every name it
// did manage to show (0x13 - 3 = 16).
//
// THE MEASURED CONSEQUENCE. The guest's directory walks filter on d_type:
// 0x12a5bb0 keeps d_type == DT_DIR into one vector and everything else into
// another, and 0x12b5504 keeps d_type == DT_REG or DT_DIR. With the host's
// layout both walks saw an empty name and an unknown type, so the guest's
// gui_assets enumeration (0x12a5bb0, reached from 0x10f8cf8) produced an
// EMPTY candidate vector. The guest therefore never reached its own
// sscanf("%ux%u") parse at 0x10f8ebc, and the cc::ImageResManager list was
// empty -- which is exactly why the guest printed "Res order:" with no
// entries under it.
//
// The fix: convert the host entry into bionic's layout on the way out.
// ---------------------------------------------------------------------------
struct BionicDirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};

// THE HOST LAYOUT, asserted against the real header.
//
// These three are the measurement the port's behaviour depends on, and they
// are the ones a toolchain change would move silently: a newlib whose ino_t
// grew from 2 bytes to 8 would push d_type from 0x02 to 0x08 and d_name from
// 0x03 to 0x09, and the guest -- which reads 0x12/0x13 -- would go back to
// seeing nothing at all. The compiler says so at build time instead of the
// guest saying so at run time, 16 bytes off, with no error anywhere.
static_assert(sizeof(ino_t) == 2,
              "host dirent layout drifted: ino_t is no longer 2 bytes, so "
              "d_type/d_name moved and the guest's 0x12/0x13 reads break");
static_assert(offsetof(struct dirent, d_type) == 2,
              "host dirent layout drifted: d_type is no longer at offset 2");
static_assert(offsetof(struct dirent, d_name) == 3,
              "host dirent layout drifted: d_name is no longer at offset 3");

static_assert(offsetof(BionicDirent, d_type) == 0x12,
              "bionic dirent layout drifted: the guest reads d_type at 0x12");
static_assert(offsetof(BionicDirent, d_name) == 0x13,
              "bionic dirent layout drifted: the guest reads d_name at 0x13");
static_assert(sizeof(BionicDirent) == 280,
              "bionic dirent size drifted: 275 bytes of data, padded to 8");

// The buffers the converted entries are published from.
//
// The guest copies the name out before it calls readdir again (0x12a5bb0
// memcpy's into its own std::string before the next readdir at 0x12a5c50),
// so one buffer per call would do for a single lister. A small ring is used
// instead, because the HOST's own readdir kept one buffer PER DIRECTORY, and
// two guest threads listing two different directories at the same time must
// not clobber each other. Sixteen slots covers that; the guest has no more
// than a handful of concurrent directory walks.
constexpr unsigned kBionicDirentPoolSize = 16;
BionicDirent g_bionicDirentPool[kBionicDirentPoolSize]{};
std::atomic<unsigned> g_bionicDirentNext{0};

void writeBionicDirent(const struct dirent& source, BionicDirent& target) {
    target.d_ino = static_cast<uint64_t>(source.d_ino);
    // The guest imports no telldir/seekdir/rewinddir, so d_off and d_reclen
    // are never read; they are filled in with the values bionic would use so
    // that a future guest user sees something sane rather than a zero.
    target.d_off = 0;
    target.d_reclen = static_cast<uint16_t>(
        offsetof(BionicDirent, d_name) + std::strlen(source.d_name) + 1);
    target.d_type = source.d_type;
    std::memset(target.d_name, 0, sizeof(target.d_name));
    std::strncpy(target.d_name, source.d_name, sizeof(target.d_name) - 1);
}

// The regression guard that checks this converter is public, so that the
// boot line can report it. It is defined after this anonymous namespace
// closes, and depends on nothing but the converter.

struct Volume {
    const char* device;
    const char* file;
    const char* sentinel;
};

constexpr Volume kVolumes[kExternalDataVolumeCount] = {
    {"rr3core", "core.romfs", "rr3core:/files/.depot/langs.utf8"},
    {"rr3gui0", "gui-hd.romfs", "rr3gui0:/files/.depot/gui_assets/2048x1536"},
    {"rr3gui1", "gui-sd.romfs", "rr3gui1:/files/.depot/gui_assets/960x640"},
    {"rr3track", "tracks.romfs", "rr3track:/files/.depot/Tracks/Menu"},
    {"rr3veh0", "vehicles-classic.romfs", "rr3veh0:/files/.depot/vehicles/2013_bentley_cgtspeed"},
    {"rr3veh1", "vehicles-mid.romfs", "rr3veh1:/files/.depot/vehicles/2018_porsche_911_gt2_rs"},
    {"rr3veh2", "vehicles-modern.romfs", "rr3veh2:/files/.depot/vehicles/2024_toyota_yaris_wrc"},
};

struct Resolution {
    char primary[0x600]{};
    char fallback[0x600]{};
    // NO THIRD SLOT, AND THAT IS A MEASURED DECISION.
    //
    // A third 1,536-byte path was tried here and the guest STOPPED BOOTING:
    // 0 "phase=guest-log-any" lines in 200 s against 2,933 in a healthy run,
    // with the self-test mask still 0x1fff and the port's own boot work
    // (45 asset-list entries, 58 manifest assets) completing normally.
    //
    // The reason is the struct's SIZE. A Resolution lives on the stack of
    // every path-taking entry point, and those entry points run on the
    // GUEST's threads, whose stacks are the guest's own (Android-sized) ones.
    // Growing the struct by 1,536 bytes multiplies that by the depth of the
    // port's own call chain (guestFopen -> resolvePath -> resolvePathImpl ->
    // withReadFallback), and the guest died before it ever entered the
    // bridge.
    //
    // The two slots are enough: the card-first order puts the card's two
    // trees in them, and the app-local card copy of the primary is still
    // reachable as the derived candidate at the END of the read chain.
    bool readOnly{};
    bool mapped{};
};

ExternalDataStatus gStatus{};
bool gMounted[kExternalDataVolumeCount]{};

void fsTrace(const char* message) {
    if (message != nullptr) svcOutputDebugString(message, std::strlen(message));
}

bool startsWith(const char* value, const char* prefix) {
    return value != nullptr && prefix != nullptr &&
        std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

bool containsTraversal(const char* path) {
    return path != nullptr &&
        (std::strcmp(path, "..") == 0 || startsWith(path, "../") ||
         std::strstr(path, "/../") != nullptr ||
         (std::strlen(path) >= 3 &&
          std::strcmp(path + std::strlen(path) - 3, "/..") == 0));
}

bool pathExists(const char* path) {
    struct stat info {};
    return path != nullptr && ::stat(path, &info) == 0;
}

bool ensureDirectory(const char* path) {
    if (pathExists(path)) return true;
    return ::mkdir(path, 0777) == 0 || errno == EEXIST;
}

// Gates both halves of the zero-byte-stub A/B: whether the files are created in
// ensureWritableRoots() below, and whether stubSuppressed() hides them from the
// path APIs further down. Defined here because the first use is 40 lines above
// the full result record, which is next to kStatBionicMode. 1 = the
// pre-investigation behaviour; 0 = neither create nor expose.
#ifndef RR3_TOUCH_STUBS
// RE-RUN of an A/B whose previous "null" result is void.
//
// The earlier A/B concluded "hiding them changed nothing", and that conclusion
// was reached while the guest was still wedged in the DeviceInfo teardown
// 0xcd2aa0 -- it never reached this code, so the test could not have observed
// anything either way. The guest now gets here, and 20261006-155937 shows what
// happens with the stubs ON:
//
//   guestFopen doc/ccsn.bin -> fopen-other size=0
//   ordinal 687..3992  "File ended, its corrupt!"  x3306
//
// doc/ccsn.bin is a placeholder WE create, it is not in the APK (verified:
// 1822 entries, zero of them under doc/, zero empty files anywhere), and the
// guest's FMCryptFile parser (class name from the RTTI name pool) opens it with
// no size check and then loops reading a NUL-terminated string whose only exit
// is `tmp == 0` -- an empty file can never satisfy that.
//
// The guest's own behaviour for a genuinely absent file of the same family is
// already known and works: localleaderboard.bin returned NULL from fopen and
// was skipped cleanly. So stubs=0 routes ccsn.bin down a path the guest has
// already been observed to survive.
#define RR3_TOUCH_STUBS 0
#endif

bool ensureWritableRoots() {
    if (!(ensureDirectory("sdmc:/switch") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost/doc") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost/proc") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost/.depot") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost/.depot/CC_Data") &&
          ensureDirectory("sdmc:/switch/rr3-native-rehost/apk") &&
          ensureDirectory(kSaveRoot) && ensureDirectory(kCacheRoot))) {
        return false;
    }
    // Empty stand-ins so first-boot fopen does not ENOENT. The guest treats
    // missing telemetry/profile as "create later" but the write path also
    // needs the parent directory; creating zero-byte files is enough for read.
    //
    // RETRACTED 2026-10-04: "enough for read" is false, and the guest says so.
    // With these six files present as zero bytes it logs
    //   GluCentralServices 7.3.0-2d3b0bd
    //   Bad config. Corrupted JSON config: unexpected end of input
    // immediately after fopen()ing telemetry_optin.bin / telemetry_opt_in.bin,
    // and two lines later "startMusic disabled during download" -- the state
    // that holds the splash. A zero-byte file IS an empty document, and an
    // empty document is exactly what "unexpected end of input" names. The
    // premise that the guest tolerates an empty file was never measured; it
    // was assumed from the read path, and the read path was the thing that
    // turned out to be wrong.
    //
    // The A/B switch RR3_TOUCH_STUBS is defined once, lower down, next to the
    // full result record. It gates BOTH halves: whether these files are created
    // here, and whether stubSuppressed() hides them from the path APIs. A/B
    // result: null -- hiding them changed nothing (see the record there), so the
    // switch is back at 1 and this loop runs exactly as it did before.
    static const char* kTouchFiles[] = {
        "sdmc:/switch/rr3-native-rehost/doc/telemetry_optin.bin",
        "sdmc:/switch/rr3-native-rehost/doc/telemetry_opt_in.bin",
        "sdmc:/switch/rr3-native-rehost/doc/ccsn.bin",
        "sdmc:/switch/rr3-native-rehost/doc/pcsf.bin",
        "sdmc:/switch/rr3-native-rehost/doc/rgn.bin",
        "sdmc:/switch/rr3-native-rehost/doc/player_profile.2.dat",
    };
    if (RR3_TOUCH_STUBS) {
        for (const char* path : kTouchFiles) {
            if (!pathExists(path)) {
                std::FILE* file = std::fopen(path, "wb");
                if (file != nullptr) {
                    std::fclose(file);
                }
            }
        }
    }
    // Virtual /proc/cpuinfo for getCpuName / feature probes.
    const char* cpuinfo =
        "sdmc:/switch/rr3-native-rehost/proc/cpuinfo";
    if (!pathExists(cpuinfo)) {
        std::FILE* file = std::fopen(cpuinfo, "wb");
        if (file != nullptr) {
            std::fputs(
                "processor\t: 0\n"
                "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
                "CPU implementer\t: 0x41\n"
                "CPU architecture: 8\n"
                "CPU variant\t: 0x0\n"
                "CPU part\t: 0xd03\n"
                "CPU revision\t: 4\n"
                "Hardware\t: Nintendo Switch\n",
                file);
            std::fclose(file);
        }
    }
    // Cloudcell webview assets exist only in the Android unpack, not the DXT
    // depot. After hideSplash the guest fopen()s each one; ENOENT stalls the
    // pass that would otherwise load menu sprites.
    static const char* kCloudcellStubs[] = {
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/rr3_automagic_const.js",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/rr_automagic_const.js",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_automagic_const.js",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/twitter_logo.gif",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/twitter_signin.html",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_burstly_btn_close.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_burstly_btn_close@2x.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_logo.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_logo@2x.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_background.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_background@2x.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_background_plain.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_background_plain@2x.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_closebutton.png",
        "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_closebutton@2x.png",
    };
    for (const char* stub : kCloudcellStubs) {
        if (!pathExists(stub)) {
            std::FILE* file = std::fopen(stub, "wb");
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    }
    for (int index = 0; index <= 12; ++index) {
        char path[0x180]{};
        std::snprintf(path, sizeof(path),
                      "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_loading_%d.png",
                      index);
        if (!pathExists(path)) {
            std::FILE* file = std::fopen(path, "wb");
            if (file != nullptr) {
                std::fclose(file);
            }
        }
        std::snprintf(path, sizeof(path),
                      "sdmc:/switch/rr3-native-rehost/.depot/CC_Data/cc_webbrowser_loading_%d@2x.png",
                      index);
        if (!pathExists(path)) {
            std::FILE* file = std::fopen(path, "wb");
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    }
    return true;
}

void cleanRelative(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) return;
    while (input != nullptr && startsWith(input, "./")) input += 2;
    while (input != nullptr && *input == '/') ++input;
    std::snprintf(output, outputSize, "%s", input != nullptr ? input : "");
    for (char* cursor = output; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\') *cursor = '/';
    }
}

// Collapse interior repeated separators: "a//b" -> "a/b".
//
// WHY IT EXISTS (measured 2026-10-21, archive 20261021-151708-eden-syncfixlong):
//
//   guest_path = sdmc:/switch/rr3-native-rehost/.depot//asset_list_base.txtCache.txt
//   primary    = rr3core:/files/.depot//asset_list_base.txtCache.txt
//   fopen-other size=-1
//
// The volume DOES ship files/.depot/asset_list_base.txtCache.txt - it is in
// core.romfs - but the doubled separator made the ROMFS lookup miss, so the
// request fell through to the SD card, where a STALE copy of the manifest
// cache sits. The guest then read the STALE manifest, asked for that build's
// flat /gui/*.gui names, and the volumes ship /gui/processed/*.gui instead -
// so every menu asset missed.
//
// The guest builds the doubled separator by concatenating a directory that
// ends in "/" with a manifest entry that starts with "/". That is the guest's
// doing and it is not a bug on its side: the filesystem layer is where a
// doubled separator has to be tolerated. One slash is the whole difference.
//
// The trailing form is preserved: a path that ENDS in "//" keeps one slash,
// so directory probes still enumerate.
void compactSlashes(const char* input, char* output, size_t outputSize) {
    if (output == nullptr || outputSize == 0) return;
    if (input == nullptr) {
        output[0] = '\0';
        return;
    }
    size_t written = 0;
    bool previousWasSlash = false;
    for (const char* cursor = input; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            if (previousWasSlash) {
                continue;
            }
            previousWasSlash = true;
        } else {
            previousWasSlash = false;
        }
        if (written + 1 >= outputSize) {
            break;
        }
        output[written++] = *cursor;
    }
    output[written] = '\0';
}

const char* contentDevice(const char* relative) {
    constexpr const char* kVehiclePrefix = "files/.depot/vehicles/";
    // Bare "gui_assets" (no resolution) must go to the SD gui volume, not core.
    // opendir("…/.depot/gui_assets") was routed to rr3core and returned ENOENT,
    // which aborted the sprite atlas walk (device round 24).
    if (startsWith(relative, "files/.depot/gui_assets/2048x1536")) return "rr3gui0";
    // Switch is 1280x720 but volumes only ship 480x320 / 960x640 / 2048x1536.
    // Guest listed 960x640 then never entered it — likely searching for a
    // 1280x720 folder that does not exist.
    if (startsWith(relative, "files/.depot/gui_assets/1280") ||
        startsWith(relative, "files/.depot/gui_assets/1920") ||
        startsWith(relative, "files/.depot/gui_assets/720p") ||
        startsWith(relative, "files/.depot/gui_assets/1080")) {
        return "rr3gui1";
    }
    if (startsWith(relative, "files/.depot/gui_assets")) return "rr3gui1";
    if (startsWith(relative, "files/.depot/Tracks/")) return "rr3track";
    if (!startsWith(relative, kVehiclePrefix)) return "rr3core";

    const char* name = relative + std::strlen(kVehiclePrefix);
    unsigned year = 0;
    for (int index = 0; index < 4 && name[index] >= '0' && name[index] <= '9'; ++index) {
        year = year * 10U + static_cast<unsigned>(name[index] - '0');
    }
    if (year == 0 || year <= 2015) return "rr3veh0";
    if (year <= 2020) return "rr3veh1";
    return "rr3veh2";
}

bool writePath(char* output, size_t outputSize, const char* root,
               const char* relative) {
    if (output == nullptr || outputSize == 0 || root == nullptr ||
        relative == nullptr || containsTraversal(relative)) {
        errno = kGuestErrnoEACCES;
        return false;
    }
    const int written = std::snprintf(output, outputSize, "%s/%s", root, relative);
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
        errno = kGuestErrnoENAMETOOLONG;
        return false;
    }
    return true;
}

// Rewrite missing resolution folder names onto the shipped 960x640 atlas.
bool rewriteGuiResolutionPath(const char* relative, char* out, size_t outSize) {
    if (relative == nullptr || out == nullptr || outSize == 0) return false;
    static const char* kMissing[] = {
        "files/.depot/gui_assets/1280x720",
        "files/.depot/gui_assets/1280x800",
        // 2026-10-21. The guest's OWN resolution table, read out of its own
        // log in archive 20261021-155728-eden-aliasfix:
        //
        //   Res order:
        //   share) Current res: 1280, 720
        //   original [2] (960.000000, 640.000000) - 1.500000
        //   original [0] (480.000000, 320.000000) - 1.500000
        //   original [3] (1024.000000, 768.000000) - 1.333333
        //   original [1] (768.000000, 576.000000) - 1.333333
        //
        // so the engine knows FOUR GUI resolutions and shares atlases between
        // them: 960x640 first, 480x320 second, then 1024x768 and 768x576.
        // The volumes ship 480x320, 960x640 and 2048x1536 -- they ship
        // NEITHER 1024x768 NOR 768x576, so the guest's third and fourth
        // choices both miss, and the fallback chain the guest printed is
        // exactly what ends in a miss.
        //
        // The rule this file already encodes -- "a rewrite is justified when
        // the volume does not ship the requested name, never merely because
        // the SD card does not have it" -- applies to both of them, and both
        // land on 960x640, which is the resolution the guest itself ranks
        // first and which the SD gui volume really ships (1,928
        // sprites.android.atlas files, measured).
        "files/.depot/gui_assets/1024x768",
        "files/.depot/gui_assets/768x576",
        "files/.depot/gui_assets/1920x1080",
        "files/.depot/gui_assets/1080p",
        "files/.depot/gui_assets/720p",
        // "files/.depot/gui_assets/2048x1536" IS DELIBERATELY NOT IN THIS LIST.
        //
        // 2026-10-21. It was here for one round and the round MEASURED it to
        // be wrong, twice over.
        //
        // WHAT THE VOLUMES SHIP, with tools/verify_rr3_data_volumes.py's
        // RomFS.contains(path), on the deployed data set
        // (E:\Eden\user\sdmc\switch\rr3-native-rehost\data):
        //
        //   gui-hd.romfs  files/.depot/gui_assets/2048x1536/loading_icon/
        //                   sprites.android.atlas            HIT  3,429 B
        //                 files/.depot/gui_assets/2048x1536/common/
        //                   sprites.android.atlas            HIT
        //                 files/.depot/gui_assets/2048x1536/splash/
        //                   sprites.android.atlas            HIT    257 B
        //                 files/.depot/gui_assets/960x640/... MISS (0 entries)
        //                 files/.depot/gui_assets/480x320/... MISS (0 entries)
        //
        //   gui-sd.romfs  files/.depot/gui_assets/960x640/loading_icon/
        //                   sprites.android.atlas            HIT  3,432 B
        //                 files/.depot/gui_assets/960x640/common/
        //                   sprites.android.atlas            HIT
        //                 files/.depot/gui_assets/960x640/splash/
        //                   sprites.android.atlas            HIT    257 B
        //                 files/.depot/gui_assets/2048x1536/... MISS (0 entries)
        //                 files/.depot/gui_assets/480x320/... 2 entries only
        //                   (events/, splash/)
        //
        // Both trees ship the SAME 53 sub-folders, so 2048x1536 is not a
        // different asset set -- it is the same set at another resolution --
        // and the 2048x1536 tree IS shipped. This file's own rule, written
        // above, is "a rewrite is justified when the volume does not ship the
        // requested name, never merely because the SD card does not have it",
        // and the volume DOES ship 2048x1536.
        //
        // MEASURED, and this is the part that decides it: with the rewrite in
        // place the port's own filesystem self-test loses a bit
        // (fsTest 0x1fff -> 0x1ffd, archive 20261021-174619-eden-menugate-terms),
        // because the self-test asserts that a gui_assets/2048x1536 request
        // resolves into rr3gui0 (the HD volume). And main.cpp GATES THE GUEST
        // BOOT ON THAT MASK:
        //
        //   else if (!dashboard.externalData.ready ||
        //            dashboard.filesystemTest != 0x1fff) {
        //       dashboardNotice = "...Guest 启动已阻止。";
        //   }
        //
        // so the rewrite does not merely cost a test bit: it stops the guest
        // from starting at all. The measured run that carried the rewrite
        // never reached onViewRenderJNI (no "view-probe-enter", no
        // "gfx-stats", 1,002 log lines instead of 24,751) and the census
        // spent the whole run with one thread and no guest progress.
        //
        // It is also the wrong bytes: the manifest the guest reads,
        // files/apk/res/asset_list_gui_assets_core.txt, names 3,292 files
        // that are ALL under /gui_assets/2048x1536/ and carries an md5 and a
        // size for each -- and the 2048x1536 and 960x640 copies of the same
        // atlas are NOT the same file (3,429 B vs 3,432 B).
        //
        // WHAT REPLACES IT: the 2048x1536 -> 960x640 rule is implemented as a
        // FALLBACK, in mapAppRootToContent() below, not as a rewrite. The
        // request is still answered from the 2048x1536 tree when the volume
        // has it -- the bytes the manifest describes -- and from the 960x640
        // tree when it does not. That is the same rule ("a 2048x1536 request
        // may be answered from 960x640") without giving up the tree that
        // matches the manifest.
    };
    for (const char* miss : kMissing) {
        const size_t n = std::strlen(miss);
        if (std::strncmp(relative, miss, n) == 0 &&
            (relative[n] == '\0' || relative[n] == '/')) {
            std::snprintf(out, outSize, "files/.depot/gui_assets/960x640%s",
                          relative + n);
            return true;
        }
    }
    // NO "gui/processed/" REWRITE. There used to be one here and it was wrong.
    //
    // The reasoning that produced it: the guest asks for
    // "gui/processed/GlobalStyles.gui", no tree on the SD card has a
    // "gui/processed/" directory, but ".depot/gui/GlobalStyles.gui" does, so
    // strip the segment. Every step of that chain checks out against the SD card
    // and against the Vita reference tree -- and it is still wrong, because the
    // guest is not asking the card. It is asking core.romfs, which ships exactly
    // the tree being "fixed up":
    //
    //   byte-scan of reference/rr3-native-rehost-original-containers/data/core.romfs
    //   (429,011,308 B), index entries are "<path>\t<md5>":
    //     /gui/processed/GlobalStyles.gui            9f324c93...  PRESENT
    //     /gui/processed/SpriteAnimation.gui        100126e0...  PRESENT
    //     /gui/processed/LoadingScreen_Init.gui     6aabccd1...  PRESENT
    //     /gui/GlobalStyles.gui                                  0 HITS
    //     /gui/processed/                         4,766 entries, and every one of
    //                                             the 4,766 entries under /gui/
    //                                             is a /gui/processed/ one
    //                                             (independently recounted 2026-10-11)
    //
    // The romfs has no "files/.depot/" prefix internally -- the volume layer
    // strips it -- so:
    //     rr3core:/files/.depot/gui/processed/GlobalStyles.gui -> /gui/processed/GlobalStyles.gui  HIT
    //     rr3core:/files/.depot/gui/GlobalStyles.gui          -> /gui/GlobalStyles.gui          MISS
    // The rewrite deleted a working path and created the failure it was written
    // to fix. gui3 showed exactly that: map-app to the rewritten path, then
    // open-fail on it (archive 20261011-120829-gui3, L4727-L4730).
    //
    // "SpriteAnimation.gui missing from both trees" was the same error: the file
    // was in core.romfs all along (md5 100126e0368fb9d87261c89eb5b1216b).
    //
    // CONTRAST with the gui_assets branch above, which IS correct: that one
    // remaps a resolution the guest names to the one the volume ships.
    //     /gui_assets/960x640                          12,965 hits
    //     /gui_assets/{1280x720,1280x800,1920x1080,1080p,720p}   0 hits each
    //
    // Rule this encodes: a rewrite is justified when the volume does not ship
    // the requested name, never merely because the SD card does not have it.
    // Check which tree is actually being served before remapping.
    //
    // ===== PROCESSED GUI, second attempt (2026-10-11) ======================
    // The block that used to live here got BOTH halves backwards. What the data
    // and the logs actually say:
    //
    // Shipped (DXT zip -- the authority; tests/test_external_data_vfs_contract.py
    // reads it): files/.depot/gui/processed/ holds 2,823 files including
    //     GlobalStyles.gui      16,597 B
    //     LoadingScreen_Init.gui  6,937 B
    //     SpriteAnimation.gui   16,605 B
    // and files/.depot/gui/GlobalStyles.gui has 0 entries. The _<W>x<H> suffixed
    // spellings have 0 entries anywhere. core.romfs carries the same bytes:
    // dir_table has files/.depot/gui/processed, file_table has the three names.
    //
    // The guest asks for BOTH spellings; only the unsuffixed one ships:
    //     gui/processed/GlobalStyles.gui            EXISTS  -> leave alone
    //     gui/processed/GlobalStyles_480x320.gui    MISSING -> drop the tail
    // The old rewrite required the suffix AND dropped the "gui/processed/"
    // segment: backwards on both counts. It converted a working path into a miss
    // (gui3: map-app to the rewritten name, then open-fail on it) and left the
    // genuinely missing name untouched.
    //
    // The doc/ third candidate is a SEPARATE gap, not handled here:
    // mapAppRootToContent() returns false for "doc/", so it gets no volume
    // fallback, and doc/ on the card holds only sv.2.dat.
    static const char* kProcessedPrefixes[] = {
        "files/.depot/gui/processed/",
        "files/apk/res/gui/processed/",
    };
    for (const char* kProcessed : kProcessedPrefixes) {
        const size_t pn = std::strlen(kProcessed);
        if (std::strncmp(relative, kProcessed, pn) != 0) {
            continue;
        }
        const char* leaf = relative + pn;
        const char* dot = std::strrchr(leaf, '.');
        if (dot == nullptr || dot <= leaf) {
            continue;
        }
        // Leaf verbatim by default -- the unsuffixed name is what ships.
        size_t nameLen = static_cast<size_t>(dot - leaf);
        // Shorten only when a trailing "_<digits>x<digits>" is really present.
        // Scanning from the end keeps earlier underscores:
        // Button_Back_1280x720 -> Button_Back, not Button.
        for (const char* us = dot - 1; us > leaf; --us) {
            if (*us != '_') {
                continue;
            }
            const char* q = us + 1;
            bool sawDigit = false;
            bool sawX = false;
            while (q < dot) {
                if (*q >= '0' && *q <= '9') {
                    sawDigit = true;
                } else if (*q == 'x' && sawDigit && !sawX) {
                    sawX = true;
                } else {
                    break;
                }
                ++q;
            }
            if (q != dot || !sawX) {
                continue;
            }
            nameLen = static_cast<size_t>(us - leaf);
            break;
        }
        // Keep the head EXACTLY as the guest asked. Computed, never typed:
        // hard-coding a prefix length is how this once produced
        // "files/.depot/guigui/...".
        std::snprintf(out, outSize, "%.*s%.*s%s",
                      static_cast<int>(pn), kProcessed,
                      static_cast<int>(nameLen), leaf, dot);
        return true;
    }
    return false;
}

bool writeContentPath(char* output, size_t outputSize, const char* relative) {
    if (containsTraversal(relative)) {
        errno = kGuestErrnoEACCES;
        return false;
    }
    char rewritten[0x500]{};
    if (rewriteGuiResolutionPath(relative, rewritten, sizeof(rewritten))) {
        relative = rewritten;
    }
    const int written = gStatus.looseFallback
        ? std::snprintf(output, outputSize, "%s/%s", kLooseDataRoot, relative)
        : std::snprintf(output, outputSize, "%s:/%s",
                        contentDevice(relative), relative);
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
        errno = kGuestErrnoENAMETOOLONG;
        return false;
    }
    return true;
}

const char* afterPackageFiles(const char* path) {
    constexpr const char* kNeedle = "/Android/data/com.ea.games.r3_row/files/";
    const char* found = path != nullptr ? std::strstr(path, kNeedle) : nullptr;
    return found != nullptr ? found + std::strlen(kNeedle) : nullptr;
}

const char* afterPackageRootDepot(const char* path) {
    constexpr const char* kNeedle = "/Android/data/com.ea.games.r3_row/.depot/";
    const char* found = path != nullptr ? std::strstr(path, kNeedle) : nullptr;
    return found != nullptr ? found + std::strlen(kNeedle) : nullptr;
}

const char* afterInternalFiles(const char* path) {
    constexpr const char* kPrefixes[] = {
        "/data/data/com.ea.games.r3_row/files/",
        "/data/user/0/com.ea.games.r3_row/files/",
    };
    for (const char* prefix : kPrefixes) {
        if (startsWith(path, prefix)) return path + std::strlen(prefix);
    }
    return nullptr;
}

const char* afterInternalCache(const char* path) {
    constexpr const char* kPrefixes[] = {
        "/data/data/com.ea.games.r3_row/cache/",
        "/data/user/0/com.ea.games.r3_row/cache/",
    };
    for (const char* prefix : kPrefixes) {
        if (startsWith(path, prefix)) return path + std::strlen(prefix);
    }
    return nullptr;
}

bool isDevicePath(const char* path) {
    if (path == nullptr) return false;
    const char* colon = std::strchr(path, ':');
    const char* slash = std::strchr(path, '/');
    return colon != nullptr && (slash == nullptr || colon < slash);
}

bool isSystemPath(const char* path) {
    return startsWith(path, "/dev/") || startsWith(path, "/proc/") ||
        startsWith(path, "/sys/") || startsWith(path, "/system/") ||
        startsWith(path, "/usr/") || startsWith(path, "/var/") ||
        startsWith(path, "/tmp/");
}

bool isAssetRelative(const char* relative) {
    constexpr const char* kPrefixes[] = {
        ".depot/", "files/", "gui_assets/", "Tracks/", "vehicles/",
        "audio/", "gui/", "Shaders/", "data/", "gamemodes/", "hud/",
        "vector_images/", "effects/", "feats/", "driver/", "sponsorship/",
        "text_", "MenuTransitions.xml", "langs.utf8", "res/",
        // "profile.dat" -- the player profile, asked for by BARE name.
        //
        // MEASURED 2026-10-19. Without it the guest logs
        //   LoadReadOnlyMappedFile failed getting full path: profile.dat
        //   Asset::LoadFile unable to load file: 'profile.dat'
        // and then sits in "startMusic disabled during download" forever,
        // never asking the GUI volume for anything. The file is NOT missing:
        // with a profile.dat present the sidecar trace shows it opened
        // rc=0 size=124 from rr3core:/files/.depot/profile.dat (archive
        // 20261019-235324-eden-magic1). The guest asks for the bare name and
        // this predicate is what decides whether a bare name is an asset
        // relative to the content root -- "gui/" was already in the list,
        // "profile.dat" was not.
        "profile.dat",
    };
    for (const char* prefix : kPrefixes) {
        if (startsWith(relative, prefix)) return true;
    }
    return false;
}

bool isCacheRelative(const char* relative) {
    return startsWith(relative, "cache/") || startsWith(relative, "webimage_cache/") ||
        startsWith(relative, "replayCache/");
}

constexpr const char* kAppRoot = "sdmc:/switch/rr3-native-rehost";

// ---------------------------------------------------------------------------
// THE DOUBLED-DEVICE-PATH RULE.
//
// A guest path that already names a device ("sdmc:/...") and then names one
// AGAIN -- or repeats the app root -- is a doubled path. The guest probes
// each of its search roots with the path it has ALREADY resolved, so
//
//   "sdmc:/switch/rr3-native-rehost/.depot/sdmc:/switch/rr3-native-rehost/.depot/vehicles/X"
//
// is a real request. MEASURED, archive 20261022-012740-eden-md5list-base-r3:
// 60 such requests in one run -- 20 under ".depot/", 20 under "apk/res/" and
// 20 under "doc/" -- and every one of them was answered with errno=22
// (EINVAL) and printed
//
//   unknown error stat(22) checking the path 'sdmc:/switch/rr3-native-rehost/.depot/sdmc:/...'
//
// WHY EINVAL: the port mapped the doubled path into a content tree
// ("rr3core:/files/.depot/sdmc:/switch/...") or handed it to the host
// verbatim, and the host's FAT layer rejects a path with a second colon in
// it. The guest's own first probe of the same asset -- the well-formed
// "<app root>/.depot/vehicles/X" -- succeeds, so the doubled probe is
// pure loss.
//
// THE RULE: a path that carries a second device prefix, or that repeats the
// app root, is REFUSED here with ENOENT. It is not mapped into a content
// tree and it is not handed to the host. ENOENT is exactly what the
// guest's other probes of the same asset already get, so the guest moves on
// to the next root instead of seeing a hard stat error.
//
// THE RULE IS NARROW: only a second device prefix or a repeated app root
// qualifies. A path that starts with the app root and then names a
// RELATIVE tail ("<app root>/.depot/vehicles/X") is untouched, and so is
// every path that does not name a device at all.
// ---------------------------------------------------------------------------
bool isDoubledDevicePath(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* first = std::strchr(path, ':');
    if (first == nullptr) {
        return false;
    }
    const char* rest = first + 1;
    // A SECOND device prefix: a colon followed by a separator or by the end
    // of the text. A colon inside a file name is not one.
    for (const char* cursor = rest; *cursor != '\0'; ++cursor) {
        if (*cursor != ':') {
            continue;
        }
        const char next = cursor[1];
        if (next == '\0' || next == '/') {
            return true;
        }
    }
    // The app root spelled out a second time.
    return std::strstr(rest, kAppRoot) != nullptr;
}

constexpr const char* kAssetSegment = "/assets_";
constexpr const char* kGuiAssetSegment = "/gui_assets/";

// TRUE for a resolution segment: "<digits>x<digits>", optionally followed by
// a separator. "960x640" and "2048x1536" are resolutions; "assets_something"
// is not, and must not be rewritten.
bool isResolutionSegment(const char* text) {
    if (text == nullptr) {
        return false;
    }
    const char* cursor = text;
    int digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0 || *cursor != 'x') {
        return false;
    }
    ++cursor;
    digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        ++cursor;
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    return *cursor == '\0' || *cursor == '/';
}

// The "gui_assets/<res>/<rest>" spelling of an "assets_<res>/<rest>" path, or
// the other way round.
//
// THE SEGMENT IS THE FIRST ONE. The tail handed in here has already had the
// volume's "files/.depot/" prefix dropped, so it starts with "assets_" or
// "gui_assets/" -- NOT with a separator. The first version of this function
// searched for "/gui_assets/", with a leading separator, and therefore never
// matched: the alias silently did nothing, the self-test's two assertions
// failed, the boot gate saw fsTest=0x1ff9 instead of 0x1fff, and the guest
// NEVER STARTED (0 "phase=guest-log-any" lines in 200 s, with the port's own
// boot work completing normally).
bool alternateAssetSpelling(const char* text, char* out, size_t outSize) {
    if (text == nullptr || out == nullptr || outSize == 0) {
        return false;
    }
    if (std::strncmp(text, "assets_", 7) == 0) {
        const char* tail = text + 7;
        if (!isResolutionSegment(tail)) {
            return false;
        }
        const int written = std::snprintf(out, outSize, "gui_assets/%s", tail);
        return written > 0 && static_cast<size_t>(written) < outSize;
    }
    if (std::strncmp(text, "gui_assets/", 11) == 0) {
        const char* tail = text + 11;
        if (!isResolutionSegment(tail)) {
            return false;
        }
        const int written = std::snprintf(out, outSize, "assets_%s", tail);
        return written > 0 && static_cast<size_t>(written) < outSize;
    }
    return false;
}

// Puts the three card-and-volume candidates of an asset path into the
// resolution's three slots, in the order the pack endpoint tries them.
//
// `contentRelative` is the VOLUME-relative form the resolver was built from
// ("files/.depot/gui_assets/960x640/common/X" or
// "files/.depot/assets_960x640/common/X"). The card's own copies are named
// from the same tail, so the two trees are reached through one rule.
//
// FILES ONLY. A DIRECTORY keeps the volume's tree, and that is not a
// detail: the guest enumerates "<app root>/.depot/gui_assets/<res>" to build
// its atlas registry, and the card's copy of that tree holds three folders
// (common, loading_icon, splash) against the volume's fifty-three. Let the
// card's tree answer the enumeration and the guest's registry collapses.
//
// MEASURED 2026-10-22: the first build with the card-first order applied to
// every path did NOT boot -- 0 "phase=guest-log-any" lines in 200 s against
// 2,933 in a healthy run, with the self-test mask still 0x1fff. The
// distinguishing property of a FILE here is its extension: every file in
// these trees is named "<something>.<ext>" ("sprites.android.atlas",
// "sprites_0.rgb.pvr.z", "GlobalStyles.gui") and no directory in them is.
void applyVolumeFirstAssetOrder(Resolution& resolution, const char* contentRelative) {
    if (contentRelative == nullptr) {
        return;
    }
    // A FILE has an extension in its last segment; a directory does not.
    const char* leaf = std::strrchr(contentRelative, '/');
    leaf = (leaf != nullptr) ? leaf + 1 : contentRelative;
    if (std::strchr(leaf, '.') == nullptr) {
        return;
    }
    // A ".ast" IS NOT AN ASSET -- IT IS THE GUEST'S OWN DOWNLOAD CACHE -- AND
    // THE SPELLING ALIAS BELOW MUST NOT TOUCH IT.
    //
    // MEASURED 2026-10-27, archive 20261027-121931-eden-f1j. The guest asked
    // for
    //     /assets_480x320/events/map/featured/weekly_events/
    //         lexus_is_f350_catalunya/sprites_0.rgb.pvr.z
    // with want_md5=d367c6f1b923e6551f2692bfa58b05bd, and the card holds
    // exactly ONE file for it -- the download cache, at
    //     <app root>/.depot/assets_480x320/.../sprites_0.rgb.pvr.z.ast
    // (65,102 B, md5 d367c6f1..., i.e. exactly the digest the guest asked
    // for).
    //
    // The endpoint could not answer it: "answered=0 reason=no-known-answer-shape
    // detail=no-local-file" with all FIVE candidates tried, and the guest's own
    // 60 "fopen-fail" lines for that path. The cause is NOT a missing file --
    // it is this alias, followed by the ".ast" rule in resolvePath().
    //
    // The alias moves the primary to the volume's "gui_assets/<res>" spelling,
    // so the card path derived from it afterwards is
    //     <app root>/.depot/gui_assets/480x320/.../sprites_0.rgb.pvr.z.ast
    // which NO card has; and the fallback -- which WAS the card's
    // "assets_480x320" copy, the one that exists -- is then wiped by the
    // ".ast" rule ("a .ast has exactly one home"). The file that exists became
    // unreachable from BOTH ".ast" candidates at once, because both of them
    // collapse onto the same rewritten primary.
    //
    // WHY THE ALIAS IS WRONG FOR A ".ast". The alias exists because the
    // volume's "gui_assets/<res>" copy is the COMPLETE revision while the
    // card's "assets_<res>" copy is the game's STALE INSTALLED revision of the
    // same logical asset, and the volume's copy has to win. A download cache
    // has no volume copy at all -- the seven volumes ship no ".ast" -- so there
    // is no revision to reconcile, and the swap can only move the request off
    // the one file that exists.
    //
    // The endpoint's candidate list already tries BOTH spellings of the cache
    // (rule 5 of rr3_asset_candidates.hpp), so the guest's own download cache
    // stays reachable under either name. What this restores is the property
    // the shared selection depends on: ONE candidate names ONE file, so the
    // digest gate can choose between the two instead of seeing the same first
    // file twice.
    const size_t leafLength = std::strlen(leaf);
    if (leafLength > 4 && std::strcmp(leaf + leafLength - 4, ".ast") == 0) {
        return;
    }
    // The card's relative tail: the volume's "files/.depot/" prefix dropped.
    const char* tail = contentRelative;
    const char* prefix = "files/.depot/";
    if (std::strncmp(tail, prefix, std::strlen(prefix)) == 0) {
        tail += std::strlen(prefix);
    }
    // The OTHER spelling of the same logical asset.
    char other[0x500]{};
    if (!alternateAssetSpelling(tail, other, sizeof(other))) {
        return;
    }

    // THE VOLUME'S COPY OF THE LOGICAL ASSET IS THE PRIMARY, AND THE CARD'S
    // "assets_<res>" COPY IS THE FALLBACK. THIS IS THE OPPOSITE OF THE
    // PREVIOUS RULE, AND THE MEASUREMENT THAT REVERSED IT IS BELOW.
    //
    // MEASURED 2026-10-22, on the deployed volumes (RomFS.contains() on
    // gui-sd.romfs / gui-hd.romfs, and the card's own files):
    //
    //   volume  gui_assets/960x640/common/sprites.android.atlas   39,199 B
    //           and its bytes carry gradient.png, common_info_button.png,
    //           fb_logo.png and apple_sign_in_icon.png -- the TEN sprites
    //           that the card's copy is missing (the card's copy has 165
    //           image entries, the volume's has 248).
    //   card    assets_960x640/common/sprites.android.atlas        24,689 B
    //           md5 43213527... and NONE of those ten sprite names.
    //
    // The card's "assets_<res>" tree is the game's INSTALLED revision and it
    // is STALE; the volume's "gui_assets/<res>" tree is the shipped data-pack
    // revision and it is complete. Promoting the card's copy to the primary
    // therefore put a STALE file in front of the guest for both the digest
    // the port publishes and the bytes the endpoint sends, while the
    // RUNTIME read (the app-root form, which is NOT aliased) kept serving
    // the volume's copy -- so the file the guest md5s and the file the
    // runtime renders were two different revisions of one asset. That is
    // the "didn't match its MD5" (24 lines, one asset) and the
    // "Failed to load image from atlas" (20 lines, 10 sprites).
    //
    // So the volume's copy is the primary here, for BOTH spellings, and the
    // two spellings still meet at ONE file -- which is what the self-test's
    // bit 4 asserts. The card's "assets_<res>" copy stays reachable as the
    // fallback (it is the only copy of the live weekly-event assets, which
    // no volume ships: gui_assets/<res>/events/.../lexus_is_f350_catalunya
    // is MISS in core.romfs, gui-sd.romfs and gui-hd.romfs alike).
    //
    // The card's "gui_assets/<res>" copy (the guest's own downloads) is the
    // THIRD candidate: it is derived from the primary by appLocalPath() in
    // withReadFallback(), so it is reached only when neither the volume nor
    // the installed copy has the file.
    const char* volumeTail = (std::strncmp(tail, "gui_assets/", 11) == 0) ? tail : other;
    char volumeRelative[0x500]{};
    std::snprintf(volumeRelative, sizeof(volumeRelative), "files/.depot/%s", volumeTail);
    char volumePath[0x600]{};
    if (!writeContentPath(volumePath, sizeof(volumePath), volumeRelative)) {
        return;
    }
    const char* installedTail = (std::strncmp(tail, "assets_", 7) == 0) ? tail : other;
    char installed[0x600]{};
    std::snprintf(installed, sizeof(installed), "%s/.depot/%s", kAppRoot, installedTail);
    std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", volumePath);
    std::snprintf(resolution.fallback, sizeof(resolution.fallback), "%s", installed);
    resolution.readOnly = true;
    resolution.mapped = true;
}

// THE DIGEST HELPER FOR THE ".ast" INSTRUMENT.
//
// rr3::md5File() reads into "unsigned char buffer[64 * 1024]" -- 64 KiB OF
// STACK. Called from the guest's thread, inside the guest's own call chain,
// that is a stack fault waiting for the asset-download phase: the guest dies,
// Eden exits on its own, and the run is 92 seconds long.
//
// This one reads 4 KiB at a time out of a FILE-SCOPE buffer, so it costs the
// guest's stack NOTHING.
//
// The buffer is not reentrant, and two threads may hash at once. That is
// acceptable for a diagnostic: the flag makes the second caller skip the
// digest rather than report a torn one.
bool digestFileShallow(const char* path, char out[33], size_t outSize,
                       unsigned long long* sizeOut) {
    static unsigned char buffer[4 * 1024];
    static volatile bool busy = false;
    if (path == nullptr || out == nullptr || outSize < 33) {
        return false;
    }
    if (busy) {
        out[0] = '\0';
        return false;
    }
    busy = true;
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        busy = false;
        return false;
    }
    Md5 digest;
    digest.begin();
    unsigned long long total = 0;
    for (;;) {
        const size_t got = std::fread(buffer, 1, sizeof(buffer), file);
        if (got > 0) {
            digest.update(buffer, got);
            total += got;
        }
        if (got < sizeof(buffer)) {
            break;
        }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    busy = false;
    if (failed) {
        return false;
    }
    digest.finish(out);
    if (sizeOut != nullptr) {
        *sizeOut = total;
    }
    return true;
}

// THE APP-LOCAL PATH for a resolved path. ONE function, used by BOTH the
// write redirect and the read fallback, so the two CANNOT disagree.
//
// A volume path is "<device>:/files/<rest>" and the app's own copy of the
// same tree on the card is "<app root>/<rest>" -- the volume's "files/"
// prefix is dropped, because the card layout has no "files/" level. A path
// that is already on the card is returned unchanged.
//
// WHY IT HAS TO BE ONE FUNCTION. The guest writes an asset cache to
// "<app root>/.depot/<asset>.ast" and reads it back later. The write has to
// land on the card (the volumes are read-only) and the read has to find it
// again. If the two derived the card path separately they would agree only
// by accident, and the day one of them changed the guest would silently
// stop finding its own downloads -- which is exactly the failure the
// ".ast" writes are hitting now (248 x "Cannot open asset file to write
// to." in archive 20261021-224416-eden-assetdl5, every one of them a write
// the port aimed at the read-only core volume).
bool appLocalPath(const char* resolved, char* output, size_t outputSize) {
    if (resolved == nullptr || output == nullptr || outputSize == 0) {
        return false;
    }
    if (std::strncmp(resolved, "sdmc:", 5) == 0) {
        const int written = std::snprintf(output, outputSize, "%s", resolved);
        return written > 0 && static_cast<size_t>(written) < outputSize;
    }
    const char* relative = std::strstr(resolved, ":/files/");
    if (relative == nullptr) {
        return false;
    }
    relative += 8;   // past ":/files/"
    const int written = std::snprintf(output, outputSize, "%s/%s",
                                      kAppRoot, relative);
    return written > 0 && static_cast<size_t>(written) < outputSize;
}

// Create the parent directory chain of a card path, so a write into a
// directory the card does not have yet still lands. The guest's asset cache
// paths are nested -- ".depot/assets_480x320/common/sprites...ast" -- and
// those directories exist only in the volume, never on the card.
bool ensureParentDirectories(const char* path) {
    if (path == nullptr) {
        return false;
    }
    char work[0x600]{};
    const int written = std::snprintf(work, sizeof(work), "%s", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(work)) {
        return false;
    }
    // Start after the device prefix, so "sdmc:" is never used as a directory.
    char* cursor = work;
    const char* prefix = std::strstr(work, ":/");
    if (prefix != nullptr) {
        cursor = work + (prefix - work) + 2;
    }
    else {
        cursor = work + 1;
    }
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        ensureDirectory(work);
        *cursor = '/';
    }
    return true;
}

// A WRITE of a path that resolved into a volume must land on the CARD.
//
// The seven romfs volumes are mounted read-only, so a write aimed at one
// can only fail -- and the guest's own message for that failure is
// "Cannot open asset file to write to." (248 times in one run), which is
// the guest giving up on the download it had just completed.
//
// The card is the app's own writable area, and the path used here is the
// SAME appLocalPath() the read fallback uses, so the guest's later read of
// the file it just wrote finds it again.
void redirectWriteToAppLocal(Resolution& resolution) {
    char local[0x600]{};
    if (!appLocalPath(resolution.primary, local, sizeof(local))) {
        return;
    }
    if (std::strcmp(local, resolution.primary) == 0) {
        // Already app-local: nothing to move, and nothing to unlock.
        resolution.readOnly = false;
        // A WRITE has no candidate chain: the file it writes is the one the
        // guest named. Leaving the other slots filled would let a later read
        // of the same path answer from a different tree.
        resolution.fallback[0] = '\0';
        return;
    }
    ensureParentDirectories(local);
    std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", local);
    // The card copy is writable; the volume copy never was.
    resolution.readOnly = false;
    // The fallback slot held a volume alias for the same file. A write has
    // no fallback, and leaving a volume path there would let a later read
    // answer from the volume instead of from the file just written.
    resolution.fallback[0] = '\0';
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "map-write", 0, 0, local);
}

// getAppPath/getExternalStorageDir both return kAppRoot. Guest then opens
// absolute sdmc: paths under it. isDevicePath used to pass those straight to
// FAT, so files/.depot/gui_assets/** never reached the seven romfs volumes.
// Save trees (doc/, cache/, .depot/CC_Data, *.dat) stay on SD.
bool mapAppRootToContent(const char* path, bool writing, Resolution& resolution) {
    if (path == nullptr || !startsWith(path, kAppRoot)) {
        return false;
    }
    // THE DOUBLED-PATH EXEMPTION. A path that names a device twice, or that
    // repeats the app root, is refused HERE -- before any content tree is
    // built out of it. See the note on isDoubledDevicePath() for the
    // measurement and for why the answer is ENOENT and not EINVAL.
    if (isDoubledDevicePath(path)) {
        errno = kGuestErrnoENOENT;
        return false;
    }
    const char* rest = path + std::strlen(kAppRoot);
    while (*rest == '/') {
        ++rest;
    }
    if (rest[0] == '\0') {
        return false;
    }
    // Collapse interior "//" before anything is built out of this path. The
    // guest concatenates a directory ending in "/" with a manifest entry
    // starting with "/", so the raw path arrives with a doubled separator -
    // and a doubled separator makes the ROMFS lookup MISS a file that the
    // volume actually ships. See compactSlashes() for the measurement.
    char restCompact[0x600]{};
    compactSlashes(rest, restCompact, sizeof(restCompact));
    rest = restCompact;
    if (rest[0] == '\0') {
        return false;
    }
    if (startsWith(rest, "save/") || startsWith(rest, "cache/") ||
        startsWith(rest, "data/") || startsWith(rest, "proc/")) {
        return false;
    }
    if (startsWith(rest, "doc/")) {
        // doc/ is the guest's app-private directory. It holds the SAVE files on
        // the card, but the guest ALSO probes it as one of its asset search
        // roots -- and it appears to require that probe to succeed.
        //
        // Measured gui6 (archive 20261011-123540-gui6, L4729-L4735): for
        // gui/processed/GlobalStyles.gui the .depot and apk/res candidates both
        // succeed and the ONLY failure is doc/ -- yet the guest still prints
        // "LoadReadOnlyMappedFile failed getting full path". gui7 removed the
        // other failures (suffix rewrite) and doc/ is now the last one standing:
        // every remaining rc=-1 for gui/processed is a doc/ path.
        //
        // So: save payloads stay on SD (that is what doc/ is for), and anything
        // that is plainly a content asset falls through to the volume with the
        // "doc/" segment dropped -- gui/processed/X -> files/.depot/gui/processed/X,
        // which is exactly where the asset ships.
        const char* afterDoc = rest + 4;
        const char* leaf = std::strrchr(afterDoc, '/');
        leaf = (leaf != nullptr) ? leaf + 1 : afterDoc;
        const bool savePayload =
            std::strncmp(leaf, "sv.", 3) == 0 ||
            std::strcmp(leaf, "ctd") == 0 ||
            std::strcmp(leaf, "profile.dat") == 0 ||
            std::strncmp(leaf, "player_profile", 15) == 0 ||
            std::strncmp(leaf, "pcsf", 4) == 0 ||
            std::strncmp(leaf, "ccsn", 4) == 0 ||
            std::strncmp(leaf, "rgn", 3) == 0 ||
            std::strncmp(leaf, "telemetry_opt", 12) == 0;
        if (savePayload || !isAssetRelative(afterDoc)) {
            return false;
        }
        char docRelative[0x500]{};
        std::snprintf(docRelative, sizeof(docRelative),
                      "files/.depot/%s", afterDoc);
        resolution.readOnly = true;
        resolution.mapped = true;
        const bool docOk = writeContentPath(resolution.primary,
                                            sizeof(resolution.primary),
                                            docRelative);
        if (!docOk) {
            return false;
        }
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "map-app", 0, 0, resolution.primary);
        return true;
    }
    // Only true save/cloudcell sidecars stay on SD. Do NOT exclude every
    // *.bin — .depot/data.bin is real content.
    if (startsWith(rest, ".depot/CC_Data") ||
        std::strstr(rest, "player_profile") != nullptr) {
        return false;
    }
    // Same rule, applied to the LEAF rather than the whole path. Measured
    // 2026-10-04 (archive 20261004-073609-fwritefix, guest-view.log):
    //
    //   api-touch  api=guestFopen guest_path=.../.depot/sv.2.dat.bak writing=1
    //   map-app    rr3core:/files/.depot/sv.2.dat.bak
    //   fopen-fail-resolve
    //
    // The guest is creating a BACKUP of its save under .depot/ and the router
    // sent it into the read-only core content volume, where it cannot exist and
    // cannot be written. That is the request immediately preceding
    // SaveManager.cpp:699's "error loading your game data".
    //
    // Leaf-scoped on purpose: `.depot/data.bin` and the rest of the content
    // tree must keep resolving into the volumes, so this matches the trailing
    // component only and never the directory.
    {
        const char* leaf = std::strrchr(rest, '/');
        leaf = (leaf != nullptr) ? leaf + 1 : rest;
        // sv.*   -- the save payload and its .bak / .0 rotation siblings
        // ctd     -- the Cloudcell document
        if (std::strncmp(leaf, "sv.", 3) == 0 ||
            std::strcmp(leaf, "ctd") == 0 ||
            // ".feats" -- the shader feature file. Measured across eight
            // archives: the guest stats it, gets ENOENT from the volume, and
            // never reaches the code that would open it (its own
            // "shader features file: %s.feats" line never appears in any log).
            // It is absent from all seven volumes and from the APK's 1822
            // entries. Routing it to SD instead means a file placed in the app
            // directory satisfies the guest's own stat, and the reaper's
            // asset=.../first=... line then answers whether engine+8 moves.
            std::strcmp(leaf, ".feats") == 0) {
            return false;
        }
    }
    char contentRelative[0x500]{};
    // Kept as a leaf so the alias below can rebuild the legacy .depot target
    // without re-testing the prefix.
    const char* appResLeaf = startsWith(rest, "apk/res/") ? rest + 8 : nullptr;
    if (startsWith(rest, "apk/files/")) {
        std::snprintf(contentRelative, sizeof(contentRelative), "%s", rest + 4);
    }
    else if (appResLeaf != nullptr) {
        // The volume ships TWO trees, not one: files/apk/res/ (1,519 names) and
        // files/.depot/ (5,709). Collapsing apk/res/ onto .depot/ left the
        // 1,297 names that exist only under apk/res/ unreachable -- the
        // asset_list_* family the game uses to enumerate and validate its own
        // asset set. Prefer the real apk/res/ copy and fall back to .depot/:
        // every same-name pair (222) is byte-identical, so this changes which
        // names resolve, never which bytes a path returns. Existence is
        // decided by the caller's real stat/fopen/open against primary and
        // then, on ENOENT, against fallback (withReadFallback, guestFopen,
        // guestOpen, guestOpendir) -- so the fallback has to land here, in the
        // resolver, and not in the pure-snprintf writeContentPath.
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/apk/res/%s", appResLeaf);
    }
    else if (startsWith(rest, "apk/")) {
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/.depot/%s", rest + 4);
    }
    else if (startsWith(rest, "files/")) {
        std::snprintf(contentRelative, sizeof(contentRelative), "%s", rest);
    }
    else if (startsWith(rest, ".depot/")) {
        std::snprintf(contentRelative, sizeof(contentRelative), "files/%s", rest);
    }
    else if (isAssetRelative(rest)) {
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/.depot/%s", rest);
    }
    else {
        return false;
    }
    resolution.readOnly = true;
    resolution.mapped = true;
    // Resolve the rewrite ONCE, here, and drive every candidate below from the
    // result. writeContentPath() rewrites too, so handing it the already
    // rewritten string is idempotent -- but the SD-card candidate is composed by
    // hand and needs the rewritten form explicitly.
    //
    // The split this prevents is real, not hypothetical: with primary rewritten
    // and the card candidate left raw, a request misses in BOTH places while
    // looking like a clean single miss. Measured with the (now removed)
    // gui/processed rewrite, gui3 archive 20261011-120829-gui3 L4727-L4730:
    //     map-app   rr3core:/files/.depot/gui/GlobalStyles.gui          <- rewritten
    //     open-fail rr3core:/files/.depot/gui/GlobalStyles.gui
    //     (card candidate was sdmc:/.../.depot/gui/processed/GlobalStyles.gui)
    // The gui_assets branch above is still live and still rewrites, so this
    // plumbing still earns its keep.
    char effectiveRelative[0x500]{};
    char guiRewritten[0x500]{};
    if (rewriteGuiResolutionPath(contentRelative, guiRewritten, sizeof(guiRewritten))) {
        std::snprintf(effectiveRelative, sizeof(effectiveRelative), "%s", guiRewritten);
    } else {
        std::snprintf(effectiveRelative, sizeof(effectiveRelative), "%s", contentRelative);
    }
    const bool ok = writeContentPath(resolution.primary, sizeof(resolution.primary),
                                     effectiveRelative);
    // THE APP-ROOT FORM IS NOT ALIASED, AND THAT IS DELIBERATE.
    //
    // The card-first order belongs to the ASSET path space -- the relative
    // forms the pack endpoint and the manifest rewrite use. The app-root form
    // is what the guest's RUNTIME reads use, and the runtime reads the
    // volume's copy of the tree: the volume's 960x640 "common" atlas has 248
    // image entries and carries gradient.png, common_info_button.png,
    // fb_logo.png, apple_sign_in_icon.png and btn_google_play_login.png, and
    // the card's copy of the same file has 165 entries and NONE of them
    // (searched for the literal names in both files' bytes).
    //
    // MEASURED: aliasing the app-root form as well cost the guest 20 "Failed
    // to load image from atlas" errors over those ten sprites (archive
    // 20261022-031343-eden-gfsalias-r1), and the run did not boot at all when
    // the alias was applied to DIRECTORIES as well (0 "guest-log-any" lines
    // in 200 s against 2,933 in a healthy run, with the self-test mask still
    // 0x1fff). The asset path space and the runtime path space are different
    // jobs, and the md5 check is the asset path space's job alone.
    //
    // The .ast ROUND TRIP still works here: the primary is the volume's path,
    // so the write lands on the card's app-local copy of it (appLocalPath),
    // and the read-back reaches the same card copy through the derived
    // candidate -- the same file, by construction.
    // THE 2048x1536 -> 960x640 RULE, AS A FALLBACK AND NOT AS A REWRITE.
    //
    // It is decided here, once, and applied below: the request for a
    // 2048x1536 gui_assets path may be answered from the 960x640 tree when
    // the 2048x1536 tree does not have the file.
    //
    // The guest's manifest -- files/apk/res/asset_list_gui_assets_core.txt,
    // 3,292 entries, every one of them under /gui_assets/2048x1536/ -- names
    // the 2048x1536 tree, with an md5 and a size per file, and the HD volume
    // (rr3gui0) ships exactly that tree. The SD volume (rr3gui1) ships the
    // 960x640 copy of the same 53 sub-folders. Both halves are measured with
    // RomFS.contains(path) on the deployed volumes; see the long note on
    // rewriteGuiResolutionPath() for the table.
    //
    // WHY NOT A REWRITE: a rewrite sends EVERY 2048x1536 request to 960x640,
    // which (a) gives up the tree whose bytes the manifest's md5s describe --
    // the two copies of one atlas are 3,429 B and 3,432 B, so they are not
    // the same file -- and (b) costs the port a self-test bit, and main.cpp
    // gates the whole guest boot on that mask being exactly 0x1fff. Measured
    // in archive 20261021-174619-eden-menugate-terms: with the rewrite in
    // place fsTest was 0x1ffd and the guest never started.
    const bool hdToSdFallback =
        startsWith(effectiveRelative, "files/.depot/gui_assets/2048x1536");
    if (ok) {
        if (appResLeaf != nullptr && !hdToSdFallback) {
            // The legacy .depot target becomes the second candidate, not the
            // only one, so no name that resolved before stops resolving.
            char alias[0x500]{};
            std::snprintf(alias, sizeof(alias), "files/.depot/%s", appResLeaf);
            writeContentPath(resolution.fallback, sizeof(resolution.fallback),
                             alias);
        }
        else if (startsWith(rest, ".depot/") && !hdToSdFallback) {
            // THE REVERSE ALIAS, and it was missing.
            //
            // The volume ships TWO trees: files/.depot/ and files/apk/res/.
            // The alias above covers an apk/res/ request that names a file the
            // volume keeps under .depot/. This covers the other direction, and
            // it is the direction the asset lists come from.
            //
            // MEASURED 2026-10-21, archive 20261021-154608-eden-slashfix: ten
            // asset-list files the guest asks for under .depot/ exist in the
            // volume ONLY under files/apk/res/:
            //     asset_list_audio.txt            asset_list_vehicle_common.txt
            //     asset_list_audio_base.txt       asset_list_vehicle_data.txt
            //     asset_list_track_data_common.txt        asset_list_vehicle_shaders.txt
            //     asset_list_track_laguna_seca_common.txt  asset_list_vehicle_wheels.txt
            //     asset_list_track_laguna_seca_high.txt    asset_list_vector_images.txt
            // All ten came back errno=2 (ENOENT) while their apk/res/ copies sat
            // in core.romfs the whole time. The guest probes several roots and
            // a missing probe is not free, so an unresolved asset list means an
            // unresolved asset set - which is the 159 MB the AssetDownloadService
            // then wanted to fetch.
            //
            // (asset_list_updates.txt is in NONE of the three packs, so its
            // ENOENT is correct: the shipping game does not have it either.)
            char alias[0x500]{};
            std::snprintf(alias, sizeof(alias), "files/apk/res/%s", rest + 7);
            writeContentPath(resolution.fallback, sizeof(resolution.fallback),
                             alias);
        }
        // THE 2048x1536 -> 960x640 FALLBACK ITSELF.
        //
        // It comes AFTER the alias chain and only fills the fallback slot if
        // that slot is still empty, so it never displaces the apk/res alias
        // (which resolves names that exist only under files/apk/res/).
        if (hdToSdFallback) {
            char alias[0x500]{};
            const char* const kHd = "files/.depot/gui_assets/2048x1536";
            const char* const kSd = "files/.depot/gui_assets/960x640";
            std::snprintf(alias, sizeof(alias), "%s%s", kSd,
                          effectiveRelative + std::strlen(kHd));
            writeContentPath(resolution.fallback, sizeof(resolution.fallback),
                             alias);
        }
        // THE SD CARD IS THE THIRD CANDIDATE, for EVERY path.
        //
        // writeContentPath() builds "<device>:/files/..." -- both the primary
        // above and the apk/res alias are INSIDE a romfs volume. Nothing on the
        // card was ever consulted, so a .depot tree dropped next to the NRO was
        // invisible to the guest no matter how complete it was. Measured
        // 2026-10-10: the Vita port's .depot (8156 files, 1469 MB, including
        // eds/AndroidHigh.plist which the guest asks for by name) sat complete
        // on the card while the guest logged src=92/471380 and drew 6 calls per
        // frame.
        //
        // The volume-relative path carries a "files/" prefix the card layout
        // does not use, so strip it: files/apk/res/X -> apk/res/X,
        // files/.depot/X -> .depot/X, files/X -> X.
        // effectiveRelative, NOT contentRelative: the card layout has no
        // "files/" prefix AND must see whatever rewrite the volume path got.
        // Two candidates that describe two different files is how a request can
        // miss in both places while looking like one clean miss.
        const char* onCard = effectiveRelative;
        if (std::strncmp(onCard, "files/", 6) == 0) {
            onCard += 6;
        }
        char cardPath[0x500]{};
        const int cardWritten = std::snprintf(
            cardPath, sizeof(cardPath), "sdmc:/switch/rr3-native-rehost/%s", onCard);
        if (cardWritten > 0 && static_cast<size_t>(cardWritten) < sizeof(cardPath)) {
            // Only when there is no volume alias already occupying the slot:
            // overwriting it would drop names that resolve inside the volume.
            if (resolution.fallback[0] == '\0') {
                std::snprintf(resolution.fallback, sizeof(resolution.fallback),
                              "%s", cardPath);
            }
        }
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "map-app", 0, 0, resolution.primary);
    }
    return ok;
}

// Which guest entry point is currently resolving a path. The api-touch trace
// needs this: a resolved-path log alone cannot say whether the guest opened and
// read a file or merely probed it for existence.
const char* gResolveApi = "?";

// Per-class line caps for the watched-path trace. Three SEPARATE counters, not
// one global budget, so the noisy Cloudcell webview directory (which the
// fopen-other trace deliberately skips) can never starve the one sidecar file
// this trace exists for. Totals: 4 + 40 + 16 = 60 lines per run, worst case.
//   hbn     4  -- /files/apk/res/hbn fired exactly once in the archived run.
//   sidecar 40 -- ctd/sv/profile/player_profile. The failing run names 8 of
//                    them across the whole boot, so 40 leaves ~5x headroom
//                    while still capping a file probed thousands of times.
//   cc_data 16 -- CC_Data/ is a webview cache; bounded, but kept because a
//                    filter that cannot see it is a filter that will hide the
//                    next failure too.
constexpr unsigned kHbnLines = 4;
constexpr unsigned kSidecarLines = 40;
constexpr unsigned kCcDataLines = 16;
// 2026-10-04. The content layer -- files/.depot/** and files/apk/res/** --
// was the one region watchedPathClass never covered, so for the run that maps
// 98 content paths and then opens exactly one of them, the rc and errno of
// every one of those 98 were unrecorded. map-app logs the RESOLVED path but is
// a different, unbounded trace with no result field, so "the guest asked for
// feats/.feats" was visible and "what came back" was not.
//
// 200 is sized to the 98 distinct first touches in that run, not to a guess
// about traffic volume: the budget is per line, so a path re-stat-ed 1000
// times still costs one. A guest that exceeds it produces a cap counter that
// saturates, which is the signal that the class is too broad -- not silently
// truncated data that reads like a complete census.
constexpr unsigned kContentLines = 200;
constexpr int kWatchedHbn = 0;
constexpr int kWatchedSidecar = 1;
constexpr int kWatchedCcData = 2;
constexpr int kWatchedContent = 3;

unsigned watchedCap(int index) {
    if (index == kWatchedHbn) return kHbnLines;
    if (index == kWatchedSidecar) return kSidecarLines;
    if (index == kWatchedCcData) return kCcDataLines;
    if (index == kWatchedContent) return kContentLines;
    return 0;
}

const char* watchedClassName(int index) {
    if (index == kWatchedHbn) return "hbn";
    if (index == kWatchedSidecar) return "sidecar";
    if (index == kWatchedCcData) return "cc_data";
    if (index == kWatchedContent) return "content";
    return "none";
}

// Claim one line for a watched class. Returns the 1-based ordinal inside the
// class cap, or 0 when the cap is spent. api-touch and the probe-result lines
// share the same counter, so the cap bounds the TOTAL new lines per class
// rather than each phase separately. Plain static counters, like the existing
// s_pathLog / s_assetOpens / s_otherOpens above: no mutex on a guest-visible
// path, no allocation.
unsigned claimWatchedLine(int index) {
    // Four entries, and the bound is kWatchedContent, not kWatchedCcData: a new
    // class added to watchedCap() without growing this array is rejected by
    // `index > kWatchedCcData` and silently reports nothing, which is the same
    // invisible-result failure this class was added to fix.
    static unsigned s_counts[4] = {0, 0, 0, 0};
    if (index < 0 || index > kWatchedContent || s_counts[index] >= watchedCap(index)) {
        return 0;
    }
    return ++s_counts[index];
}

// Cloudcell sidecar family, matched on BASENAME. Not a directory match: the
// guest resolves hundreds of .depot/ asset paths per boot, so a directory-level
// gate would spend its whole budget on art and still never name ctd.dat.
constexpr const char* kCloudcellSidecars[] = {
    "ctd",              // Cloudcell document -- the file under investigation
    "sv.",              // Cloudcell save/version payload
    "profile.dat",
    "player_profile",
    "ccsn",             // the doc/ Cloudcell blobs the shim pre-creates
    "pcsf",
    "rgn",
    "telemetry_optin",
};

const char* pathBasename(const char* path) {
    const char* base = path;
    for (const char* cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            base = cursor + 1;
        }
    }
    return base;
}

// TRUE for a path whose LEAF ends in ".ast".
//
// A ".ast" PATH IS CARD-ONLY, AND THIS IS THE PREDICATE THAT SAYS SO.
//
// WHY. The guest downloads an asset, writes the bytes to "<asset>.ast" on the
// card, and then RE-OPENS that file to md5 it. Every ".ast" the guest ever
// sees is therefore a file it wrote moments earlier: the seven romfs volumes
// ship no ".ast" file at all, and the card's three ".ast" files (measured
// 2026-10-22 on the deployed card) are all of them leftovers of a download
// that never completed.
//
// So a ".ast" path has exactly ONE legitimate home -- the card -- and
// mapping it into a content/volume tree can only ever produce the WRONG
// file: the volume's copy of the same name WITHOUT the suffix (the volume
// ships "sprites.android.atlas", not "sprites.android.atlas.ast"), which is
// a different revision of the same asset.
//
// The guest's own naming rule puts the download cache under the
// "gui_assets/<res>" spelling (measured: the guest wrote
// ".depot/gui_assets/960x640/common/sprites.android.atlas.ast" while it
// REQUESTED "/assets_960x640/common/sprites.android.atlas"), so this one
// predicate covers both spellings: the card path it resolves to is the same
// file either way.
bool isAstLeaf(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* leaf = pathBasename(path);
    const size_t length = std::strlen(leaf);
    return length > 4 && std::strcmp(leaf + length - 4, ".ast") == 0;
}

int watchedPathClass(const char* path) {
    if (path == nullptr) {
        return -1;
    }
    const char* base = pathBasename(path);
    if (std::strcmp(base, "hbn") == 0) {
        return kWatchedHbn;
    }
    for (const char* sidecar : kCloudcellSidecars) {
        if (std::strncmp(base, sidecar, std::strlen(sidecar)) == 0) {
            return kWatchedSidecar;
        }
    }
    if (std::strstr(path, "/CC_Data") != nullptr) {
        return kWatchedCcData;
    }
    // The content layer, last so CC_Data keeps its own class and budget. Both
    // trees are what mapAppRootToContent sends into the seven volumes, so
    // this is exactly the set whose stat result was invisible.
    if (std::strstr(path, "/.depot/") != nullptr ||
        std::strstr(path, "apk/res/") != nullptr) {
        return kWatchedContent;
    }
    return -1;
}

// Item 2: the value the guest actually receives. This is the measurement that
// closes the ctd.dat question -- rc + errno + st_size, and for open the fd, the
// size and the first 16 bytes. An 8-byte file has to be able to appear here as
// size 8; the shim never judges the length, it only reports it.
void logWatchedResult(const char* api, const char* guestPath,
                      const char* devicePath, int rc, int error,
                      long long size, unsigned mode) {
    const int index = watchedPathClass(guestPath);
    const unsigned ordinal = claimWatchedLine(index);
    if (ordinal == 0) {
        return;
    }
    char details[448];
    std::snprintf(details, sizeof(details),
                  "class=%s api=%s rc=%d errno=%d size=%lld mode=%o "
                  "guest_path=%.100s device_path=%.220s",
                  watchedClassName(index), api, rc, error, size, mode,
                  guestPath != nullptr ? guestPath : "-",
                  devicePath != nullptr ? devicePath : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "cloudcell-probe", ordinal,
                        static_cast<unsigned long long>(error), details);
}

// Same line shape as the open-ok / open-fail trace further down in guestOpen
// (fd in ordinal, size in target, first 16 bytes as hex in details), reached
// from a bounded gate instead of that block's unbounded substring list. The
// peek is 16 bytes and the offset is restored, so this is observation, not a
// change of open behaviour.
void logWatchedOpen(const char* guestPath, const Resolution& resolution, int fd) {
    const int index = watchedPathClass(guestPath);
    const unsigned ordinal = claimWatchedLine(index);
    if (ordinal == 0) {
        return;
    }
    char details[448];
    if (fd < 0) {
        std::snprintf(details, sizeof(details),
                      "class=%s api=guestOpen rc=-1 errno=%d head=- "
                      "guest_path=%.100s device_path=%.220s",
                      watchedClassName(index), errno,
                      guestPath != nullptr ? guestPath : "-", resolution.primary);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "open-fail", 0,
                            static_cast<unsigned long long>(errno), details);
        return;
    }
    char peek[16]{};
    const ssize_t n = ::read(fd, peek, sizeof(peek));
    const off_t end = ::lseek(fd, 0, SEEK_END);
    const long size = static_cast<long>(end);
    ::lseek(fd, 0, SEEK_SET);
    static const char* kHex = "0123456789abcdef";
    char hex[40]{};
    for (int i = 0; i < 16 && i < n; ++i) {
        hex[i * 2] = kHex[(static_cast<unsigned char>(peek[i]) >> 4) & 0xf];
        hex[i * 2 + 1] = kHex[static_cast<unsigned char>(peek[i]) & 0xf];
    }
    std::snprintf(details, sizeof(details),
                  "class=%s api=guestOpen rc=0 errno=0 ordinal=%u head=%s "
                  "guest_path=%.100s device_path=%.220s",
                  watchedClassName(index), ordinal, hex,
                  guestPath != nullptr ? guestPath : "-", resolution.primary);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "open-ok", static_cast<unsigned long long>(fd),
                        static_cast<unsigned long long>(size), details);
}

// ---------------------------------------------------------------------------
// The asset-list request trace.
//
// WHY IT EXISTS. The guest's download phase ends on a gate that asks whether
// the AssetDownloadService's queue is empty, and that queue holds ASSET-LIST
// FILE NAMES -- the names of the files the guest wanted and could not find.
// The existing path-req trace does not fire for these paths (they mention
// none of .depot/gui/apk/Tracks/vehicles/Shaders/.vsh/.fsh) and it carries
// neither the resolved candidates nor the errno, so "the guest asked for
// asset_list_base.txt and the host looked in rr3core:/files/apk/res/..." is
// not visible anywhere in the log.
//
// NARROW BY CONSTRUCTION. It fires only for a path that contains
// "asset_list", it is capped, and it changes nothing about path-req, about
// path-req's cap, or about any other trace.
//
// One line per request. The line carries the raw guest path, the two
// candidates the host tried (primary and fallback), the errno of the LAST
// attempt, and whether the request was a read or a write.
constexpr const char* kAssetListMarker = "asset_list";
// 2026-10-21. The cap was 60 and it was HIT in the archive
// 20261021-155728-eden-aliasfix: the trace stopped at
// asset_list_quest_flashback_assets.txt, which is the 18th of the 35 names
// the guest queues.  The remaining 17 requests, and any request for the
// "asset_list_updates.info" sidecar the guest's own code builds (guest
// 0x996e50), were therefore NOT on record -- and "the guest never asked" and
// "the trace stopped" were the same observation.
//
// 400 is sized against the measured traffic: the guest makes 4 requests per
// manifest (2 stat + 2 fopen) for 35 manifests, i.e. ~140 lines, plus the
// positive control and the cache.  The counter is a cap, not a filter: a run
// that exceeds it still says so in the counter rather than reading like a
// complete census.
constexpr unsigned kAssetListLines = 400;

bool isAssetListPath(const char* path) {
    return path != nullptr && std::strstr(path, kAssetListMarker) != nullptr;
}

// Defined below, next to withReadFallback: that is the only place that
// knows the errno of the last attempt, and this is where the resolve
// failure -- which never reaches an attempt at all -- is reported.
//
// `rc` is the result of the last attempt (0 = served) and `lastErrno` is
// the errno that attempt left behind.
void logAssetListRequest(const char* guestPath, const Resolution& resolution,
                         bool writing, int rc, int lastErrno);

// The resolver itself. resolvePath() below is the wrapper every caller uses.
bool resolvePathImpl(const char* path, bool writing, Resolution& resolution);

// THE LAUNCHER-SIDE SETTINGS LAYER.  Defined further down, next to the rest
// of the profile work; declared here because resolvePath() is where it
// belongs.  See the note at its definition for WHY it is here and not in the
// reader.
bool maybeSubstituteDeviceProfile(const char* guestPath, Resolution& resolution);

bool resolvePath(const char* path, bool writing, Resolution& resolution) {
    const bool resolved = resolvePathImpl(path, writing, resolution);
    if (resolved && !writing) {
        // THE GUEST'S DEVICE PROFILE, WITH THE CARD'S SETTINGS APPLIED.
        //
        // This is here, in the ONE place every path takes, because the guest
        // reads the profile with open() + mmap() and the asset list publishes
        // an md5 for it: a substitution anywhere else would make the bytes
        // the guest reads and the digest the list publishes disagree.
        maybeSubstituteDeviceProfile(path, resolution);
    }
    if (resolved && isAstLeaf(path)) {
        // THE ".ast" RULE: CARD-ONLY, AND IT IS ENFORCED HERE, IN THE ONE
        // PLACE EVERY PATH TAKES.
        //
        // resolvePathImpl() has already turned the guest's path into a
        // content-relative path and then into a device path ("rr3gui1:/files/
        // .depot/..."). For a ".ast" that device path is WRONG: no volume
        // ships a ".ast" file, so the lookup can only ever land on the
        // volume's copy of the same name WITHOUT the suffix.
        //
        // The card path is derived with appLocalPath() -- THE SAME function
        // redirectWriteToAppLocal() uses for a write -- so the ".ast" the
        // guest writes, the ".ast" it reads back and the ".ast" guestRename()
        // moves are one file by construction, and none of them is ever a
        // read-only volume path.
        char card[0x600]{};
        if (appLocalPath(resolution.primary, card, sizeof(card))) {
            std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", card);
            // A ".ast" has exactly one home: no alias, no second candidate.
            resolution.fallback[0] = '\0';
            resolution.readOnly = false;
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "map-ast", 0, 0, resolution.primary);
        }
    }
    if (resolved && writing) {
        // EVERY write goes through here, so this is the one place that has to
        // know that a write aimed at a read-only volume cannot work. The
        // path it lands on is the app-local one the read fallback also uses,
        // so the guest finds the file again when it reads it back.
        redirectWriteToAppLocal(resolution);
    }
    if (!resolved && isAssetListPath(path)) {
        // The request never got as far as an attempt: the resolver refused
        // it. The errno here is the resolver's, and the candidates are
        // whatever it managed to build before giving up.
        logAssetListRequest(path, resolution, writing, -1, errno);
    }
    return resolved;
}

bool resolvePathImpl(const char* path, bool writing, Resolution& resolution) {
    // Item 4: one bounded line, emitted from the first resolvePath of the run,
    // so the next run can prove items 1-3 are compiled in without re-deriving
    // anything. Placed here rather than behind the watched-path gate on
    // purpose: this still fires when NO watched path is ever touched, and that
    // absence is itself the finding. appendGuestBootLine is fsync-free.
    {
        static bool s_announced = false;
        if (!s_announced) {
            s_announced = true;
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "cloudcell-visibility", 1, 2,
                                "phases=rr3api-touch,rr3cloudcell-probe,rr3open-ok,rr3guest-log,rr3guest-log-any "
                                "caps=hbn4,sidecar40,ccdata16,android_any200 "
                                "stat_reports=st_size,errno; open_reports=fd,size,head16");
        }
    }
    if (path == nullptr || *path == '\0') {
        errno = kGuestErrnoENOENT;
        return false;
    }
    // Demand trace: first 60 paths that mention depot/gui/apk/Tracks so the
    // next device log shows what the resource manager actually asks for.
    {
        static unsigned s_pathLog = 0;
        if (s_pathLog < 120 &&
            (std::strstr(path, ".depot") != nullptr ||
             std::strstr(path, "gui") != nullptr ||
             std::strstr(path, "apk") != nullptr ||
             std::strstr(path, "Tracks") != nullptr ||
             std::strstr(path, "vehicles") != nullptr ||
             std::strstr(path, "Shaders") != nullptr ||
             std::strstr(path, ".vsh") != nullptr ||
             std::strstr(path, ".fsh") != nullptr)) {
            ++s_pathLog;
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "path-req", s_pathLog, writing ? 1 : 0, path);
        }
    }
    // Item 1: the hbn-touch gate, widened to the Cloudcell sidecar family and
    // renamed api-touch because it is no longer hbn-specific. gResolveApi was
    // already populated by all 19 path-taking entry points; the old condition
    // fired only for hbn, which is why ctd.dat never named itself. Now bounded
    // per class (kHbnLines / kSidecarLines / kCcDataLines).
    {
        const int index = watchedPathClass(path);
        const unsigned ordinal = claimWatchedLine(index);
        if (ordinal != 0) {
            char details[384];
            std::snprintf(details, sizeof(details),
                          "class=%s ordinal=%u cap=%u api=%s guest_path=%s writing=%d",
                          watchedClassName(index), ordinal, watchedCap(index),
                          gResolveApi, path, writing ? 1 : 0);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "api-touch", ordinal, 0, details);
        }
    }
    if (containsTraversal(path)) {
        errno = kGuestErrnoEACCES;
        return false;
    }
    // Android content URIs have no Horizon equivalent. Transparent passthrough
    // used to produce a colon path that fopen always failed on with a confusing
    // error; fail closed with ENOENT so the guest treats it as missing data.
    if (startsWith(path, "content://") || startsWith(path, "file://") ||
        startsWith(path, "android.resource://")) {
        errno = kGuestErrnoENOENT;
        return false;
    }
    // Virtual /proc on SD so guest cpu/feature probes succeed offline.
    if (startsWith(path, "/proc/")) {
        char mapped[0x200]{};
        std::snprintf(mapped, sizeof(mapped),
                      "sdmc:/switch/rr3-native-rehost/proc/%s", path + 6);
        std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", mapped);
        resolution.readOnly = true;
        resolution.mapped = true;
        return true;
    }
    if (isDevicePath(path) || isSystemPath(path)) {
        if (mapAppRootToContent(path, writing, resolution)) {
            return true;
        }
        // THE DOUBLED-PATH EXEMPTION, second line of defence.
        //
        // mapAppRootToContent() refuses a doubled path, but it refuses it by
        // returning false, and the line below would then hand the path to the
        // host VERBATIM -- and the host's FAT layer answers a path with a
        // second colon in it with EINVAL, which the guest reports as
        // "unknown error stat(22) checking the path '...'".
        //
        // So the refusal has to survive the fallthrough: a doubled path is
        // ENOENT, the same answer the guest's other, well-formed probes of
        // the same asset get. See isDoubledDevicePath().
        if (isDoubledDevicePath(path)) {
            errno = kGuestErrnoENOENT;
            return false;
        }
        std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", path);
        return true;
    }

    if (const char* relative = afterPackageFiles(path)) {
        char contentRelative[0x500]{};
        // The same doubled separator the app-root path can carry. The guest
        // builds it by joining a directory that ends in "/" with an asset name
        // that starts with "/", and a ROMFS lookup with "//" in it misses a
        // file the volume ships. Compact before anything is built from it.
        char relativeCompact[0x500]{};
        compactSlashes(relative, relativeCompact, sizeof(relativeCompact));
        relative = relativeCompact;
        // Android unpacks the depot under files/. Guest code sometimes asks
        // for files/gui/... or files/gui_assets/... without the .depot/ level.
        // The seven volumes store everything under files/.depot/.
        if (isAssetRelative(relative) && !startsWith(relative, ".depot/") &&
            !startsWith(relative, "files/")) {
            std::snprintf(contentRelative, sizeof(contentRelative),
                          "files/.depot/%s", relative);
        }
        else {
            std::snprintf(contentRelative, sizeof(contentRelative), "files/%s", relative);
        }
        resolution.readOnly = true;
        resolution.mapped = true;
        return writeContentPath(resolution.primary, sizeof(resolution.primary), contentRelative);
    }
    if (const char* relative = afterPackageRootDepot(path)) {
        char contentRelative[0x500]{};
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/.depot/%s", relative);
        resolution.readOnly = true;
        resolution.mapped = true;
        return writeContentPath(resolution.primary, sizeof(resolution.primary), contentRelative);
    }
    if (const char* relative = afterInternalCache(path)) {
        resolution.mapped = true;
        return writePath(resolution.primary, sizeof(resolution.primary), kCacheRoot, relative);
    }
    if (const char* relative = afterInternalFiles(path)) {
        resolution.mapped = true;
        return writePath(resolution.primary, sizeof(resolution.primary), kSaveRoot, relative);
    }

    char relative[0x500]{};
    cleanRelative(path, relative, sizeof(relative));
    if (startsWith(relative, ".depot/")) {
        char prefixed[0x500]{};
        std::snprintf(prefixed, sizeof(prefixed), "files/%s", relative);
        resolution.readOnly = true;
        resolution.mapped = true;
        if (!writeContentPath(resolution.primary, sizeof(resolution.primary),
                              prefixed)) {
            return false;
        }
        // THE CARD-FIRST ORDER, ON THE ASSET PATH SPACE ONLY.
        //
        // This branch is the one the pack endpoint and the manifest rewrite
        // use: their candidates are named ".depot/<...>", with no "files/"
        // prefix. The guest's own runtime requests are named
        // "files/.depot/<...>" (the api-touch trace shows
        // "files/.depot/gui_assets/480x320/...") and they are a DIFFERENT
        // job: the guest renders from the volume's tree, and the volume's
        // 960x640 "common" atlas has 248 image entries and carries
        // gradient.png, common_info_button.png, fb_logo.png,
        // apple_sign_in_icon.png and btn_google_play_login.png, where the
        // card's copy of the same file has 165 entries and NONE of them
        // (searched for the literal names in both files' bytes).
        //
        // MEASURED: aliasing the guest's runtime form as well cost the guest
        // 20 "Failed to load image from atlas" errors over those ten sprites
        // (archive 20261022-031343-eden-gfsalias-r1), and a build that
        // aliased DIRECTORIES as well did not boot at all (0 "guest-log-any"
        // lines in 200 s against 2,933 in a healthy run, with the self-test
        // mask still 0x1fff).
        //
        // So: the asset path space gets the card's copy, the runtime path
        // space keeps the volume's, and the two lists and the endpoint all
        // name the same bytes because they all live in the asset path space.
        applyVolumeFirstAssetOrder(resolution, prefixed);
        return true;
    }
    if (startsWith(relative, "res/")) {
        char prefixed[0x500]{};
        std::snprintf(prefixed, sizeof(prefixed), "files/apk/%s", relative);
        resolution.readOnly = true;
        resolution.mapped = true;
        return writeContentPath(resolution.primary, sizeof(resolution.primary), prefixed);
    }
    if (isAssetRelative(relative)) {
        char contentRelative[0x500]{};
        if (startsWith(relative, "files/.depot/")) {
            std::snprintf(contentRelative, sizeof(contentRelative), "%s", relative);
        }
        else if (startsWith(relative, "files/")) {
            std::snprintf(contentRelative, sizeof(contentRelative), "%s", relative);
        }
        else if (startsWith(relative, ".depot/")) {
            std::snprintf(contentRelative, sizeof(contentRelative), "files/%s", relative);
        }
        else {
            // gui/, Tracks/, vehicles/, audio/, … live under files/.depot/ in
            // the seven volumes; Android asked for them without that prefix.
            std::snprintf(contentRelative, sizeof(contentRelative),
                          "files/.depot/%s", relative);
        }
        resolution.readOnly = true;
        resolution.mapped = true;
        if (!writeContentPath(resolution.primary, sizeof(resolution.primary),
                              contentRelative)) {
            return false;
        }
        // NO CARD-FIRST ORDER HERE. This branch serves the guest's own
        // runtime requests -- "files/.depot/<...>", the form the api-touch
        // trace shows -- and those keep the volume's copy, which is the
        // revision the guest's atlas registry is built from. The asset path
        // space (the ".depot/<...>" form, one branch up) is where the card's
        // copy belongs. See the note on the branch above.
        //
        // THE 2048x1536 -> 960x640 FALLBACK, for the RELATIVE form of the
        // request. mapAppRootToContent() has the same rule for the app-root
        // form; this is the same rule on the other path into the resolver,
        // and the guest uses BOTH ("sdmc:/.../rr3-native-rehost/gui_assets/..."
        // and the bare "files/.depot/gui_assets/...").
        //
        // The primary stays the 2048x1536 tree on the HD volume -- the bytes
        // the manifest's md5s describe -- and the 960x640 copy on the SD volume
        // answers when the HD tree does not have the file. See the long note
        // in mapAppRootToContent() and in rewriteGuiResolutionPath().
        if (startsWith(contentRelative, "files/.depot/gui_assets/2048x1536") &&
            resolution.fallback[0] == '\0') {
            char alias[0x500]{};
            const char* const kHd = "files/.depot/gui_assets/2048x1536";
            const char* const kSd = "files/.depot/gui_assets/960x640";
            std::snprintf(alias, sizeof(alias), "%s%s", kSd,
                          contentRelative + std::strlen(kHd));
            writeContentPath(resolution.fallback, sizeof(resolution.fallback),
                             alias);
        }
        return true;
    }
    if (isCacheRelative(relative)) {
        resolution.mapped = true;
        return writePath(resolution.primary, sizeof(resolution.primary), kCacheRoot, relative);
    }

    resolution.mapped = true;
    if (!writePath(resolution.primary, sizeof(resolution.primary), kSaveRoot, relative)) {
        return false;
    }
    if (!writing) {
        char contentRelative[0x500]{};
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/.depot/%s", relative);
        writeContentPath(resolution.fallback, sizeof(resolution.fallback), contentRelative);
    }
    return true;
}

// One more read attempt, per candidate: the name without a trailing ".z".
//
// The guest enumerates each texture as a FORMAT LIST, not one path:
//   fresnel_gloss.dxt.dds / .etc.dds / .rgb.pvr, each also probed as "<name>.z",
// and the whole list is walked per texture (1,269 stat calls in one run, 788
// distinct candidates). Nothing here decompresses anything, so a ".z" request
// only matches a file that is literally named ".z" -- and the depot we have is
// stored UNCOMPRESSED (.rgb.pvr with no ".z"). Every texture therefore missed,
// which is what an 11.5% stat hit rate with a 6-draw-call scene looks like.
//
// RETRACTED 2026-10-16: the comment that used to follow is the premise that
// caused 70 permanent load failures, and it was wrong on its first sentence --
//     "Stripping .z is transparent rather than lossy: the .z file is the same
//      bytes as the plain one, and a request that wanted compressed content is
//      satisfied by the uncompressed bytes."
// It is not the same bytes. A .z container is a 4-byte LE32 uncompressed-size
// prefix followed by a zlib stream, and the guest inflates unconditionally
// (ldrsw x9,[x1] / add x2,x0,#4 / sub x3,x9,#4 / bl inflate at 0x15c0b18).
// Serving the plain file to a .z request therefore cannot satisfy it; it can
// only turn an honest ENOENT into a certain Z_DATA_ERROR. Verified against a
// real container: vehicles/1951_ferrari_f1_f375/...cab.dxt.dds.z is 99,549 B
// beginning 38 ab 02 00 78 da, and zlib.decompress(blob[4:]) yields exactly
// 174,904 bytes, matching the prefix.
//
// The format-list reasoning above was right, and it is why the fix is to
// return ENOENT rather than keep probing: the guest already asks for both
// spellings separately and walks them in order.
// Unbudgeted candidate trace for one subtree, 2026-10-11.
//
// Why it exists: 71 loads fail in a row as
//     mtResourceCache: Failed to load resource file:
//         vehicles/shaders/fresnel_gloss.rgb.pvr.z
// while guestStat for the same paths succeeds (176 api-touch hits) and the card
// really does hold .depot/vehicles/shaders/fresnel_gloss.rgb.pvr at 4,148 B.
//
// The reason was NOT established, and two attempts to infer it were both wrong:
// once by reading absence in stat-path, which DROPS successes past ordinal 600,
// and once by reading absence in api-touch, which is capped per class. Both are
// budgeted traces; "I cannot see it" is not "it did not happen".
//
// So this logs every candidate withReadFallback actually attempts, with the
// errno of each, and is NOT budgeted. It fires only for paths containing
// kProbeSubtree, so it cannot grow with the rest of the log.
constexpr const char* kProbeSubtree = "vehicles/shaders";

// The asset-list trace itself. One line per request, capped.
//
// The paths are truncated at 380 characters each: the buffer stays inside a
// guest thread's stack (the bridge allows a 16 KiB one) and a guest path
// that long does not occur -- the longest one seen is an apk/res path of
// about 90 characters.
void logAssetListRequest(const char* guestPath, const Resolution& resolution,
                         bool writing, int rc, int lastErrno) {
    static unsigned s_lines = 0;
    if (s_lines >= kAssetListLines) {
        return;
    }
    ++s_lines;
    char details[0x600];
    __builtin_snprintf(details, sizeof(details),
                       "api=%s writing=%d rc=%d last_errno=%d"
                       " guest_path=%.380s primary=%.380s fallback=%.380s",
                       gResolveApi != nullptr ? gResolveApi : "?",
                       writing ? 1 : 0, rc, lastErrno,
                       guestPath != nullptr ? guestPath : "-",
                       resolution.primary[0] != '\0' ? resolution.primary : "-",
                       resolution.fallback[0] != '\0' ? resolution.fallback : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-req", s_lines, 0, details);
}

// ---------------------------------------------------------------------------
// THE COMPLETED GUI-ASSETS MANIFEST.
//
// WHY THIS EXISTS. The guest's atlas registry is built from the asset list.
// The list the port serves, files/apk/res/asset_list_gui_assets_core.txt
// (365,312 B, 3,292 lines), names NO "splash" file and NO "loading_icon"
// file at ANY resolution -- measured, zero hits for either name in the whole
// file -- so ImageResManager::loadImageFromAtlas (0x10f98a4) fails for
// 'splash/sky_gradient.png' and for 'loading_icon/frame_01..13.png' WITHOUT
// ever asking the filesystem for an atlas: the entry is simply not in the
// registry.
//
// The volumes DO ship both features, at every resolution they carry. The
// manifest is what is incomplete, not the data.
//
// WHAT THIS DOES. It completes THAT ONE manifest and serves the completed
// copy:
//
//   * the shipped manifest is read from the volume, byte for byte;
//   * every file under a top-level "splash/" or "loading_icon/" directory at
//     every resolution the volumes ship is enumerated FROM THE VOLUMES
//     (opendir/readdir on the mounted devices -- the same calls guestOpendir
//     makes);
//   * for each of those files the md5 and the size are computed FROM THE
//     FILE'S OWN BYTES (rr3::md5File), so a line can never claim a digest
//     the file does not have;
//   * the lines are appended to the shipped manifest's text, and the result
//     is written to the SD card, which the resolver then serves INSTEAD of
//     the volume copy.
//
// The digest is not a constant in this file on purpose. The deployed volumes
// are content-addressed (rr3-data-manifest.json carries a sha256 per
// volume), and a digest computed at run time is a digest of the bytes that
// are actually there -- for whichever copy the resolver would serve.
//
// WHAT IT DOES NOT DO. It does not edit a volume, it does not invent a
// file, and it does not touch any other manifest. A file the volumes do not
// ship produces NO line, and the count of those is in the log line below.
// ---------------------------------------------------------------------------

// The manifest the guest reads to build its atlas registry.
constexpr const char* kGuiAssetsManifestLeaf = "asset_list_gui_assets_core.txt";
// Where the completed copy is written. The cache root is already a writable
// root (ensureWritableRoots), so this needs no new directory.
constexpr const char* kCompletedManifestPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_gui_assets_core.txt";
constexpr const char* kCompletedManifestTempPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_gui_assets_core.txt.tmp";
// The two FEATURES the shipped manifest does not name at any resolution.
constexpr const char* kManifestFeatures[] = {"splash", "loading_icon"};
// The resolutions the volumes ship. The guest's own resolution table names
// 960x640 first, then 480x320, then 1024x768 and 768x576 -- and the volumes
// ship neither of the last two, so these three are the complete set.
constexpr const char* kManifestResolutions[] = {"2048x1536", "960x640", "480x320"};

struct ManifestAsset {
    char logical[224];  // "/gui_assets/<resolution>/<relative path>"
};
constexpr unsigned kManifestMaxAssets = 256;

bool manifestPathKnown(const ManifestAsset* assets, unsigned count, const char* logical) {
    for (unsigned index = 0; index < count; ++index) {
        if (std::strcmp(assets[index].logical, logical) == 0) {
            return true;
        }
    }
    return false;
}

// Recursively collect the files under one volume directory.
//
// The host's opendir/readdir, NOT the guest's: guestReaddir injects synthetic
// entries for the bare gui_assets folder, and this walk must see only what the
// volume really ships.
void collectManifestAssets(const char* device, const char* volumeRelative,
                           const char* logicalPrefix,
                           ManifestAsset* assets, unsigned& count,
                           unsigned& overflow) {
    char directory[352];
    std::snprintf(directory, sizeof(directory), "%s:/%s", device, volumeRelative);
    DIR* handle = ::opendir(directory);
    if (handle == nullptr) {
        return;
    }
    for (;;) {
        struct dirent* entry = ::readdir(handle);
        if (entry == nullptr) {
            break;
        }
        if (entry->d_name[0] == '.') {
            continue;
        }
        char childRelative[352];
        std::snprintf(childRelative, sizeof(childRelative), "%s/%s",
                      volumeRelative, entry->d_name);
        char childDevice[384];
        std::snprintf(childDevice, sizeof(childDevice), "%s:/%s", device, childRelative);
        struct stat info{};
        if (::stat(childDevice, &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            char childLogical[256];
            std::snprintf(childLogical, sizeof(childLogical), "%s%s/",
                          logicalPrefix, entry->d_name);
            collectManifestAssets(device, childRelative, childLogical,
                                  assets, count, overflow);
            continue;
        }
        if (count >= kManifestMaxAssets) {
            ++overflow;
            continue;
        }
        char logical[256];
        std::snprintf(logical, sizeof(logical), "%s%s", logicalPrefix, entry->d_name);
        if (manifestPathKnown(assets, count, logical)) {
            continue;
        }
        std::snprintf(assets[count].logical, sizeof(assets[count].logical), "%s", logical);
        ++count;
    }
    ::closedir(handle);
}

// EVERY place the port can serve a logical manifest path from, in the order
// the resolver would try them.
//
// A manifest line has to describe bytes the port can actually hand the
// guest, and the port has FOUR ways to serve a gui_assets path:
//
//   1. the .depot copy in the gui volume for that resolution
//   2. the 960x640 copy -- the hd->sd FALLBACK, 2048x1536 only
//   3. the apk/res copy in the core volume -- the .depot -> apk/res ALIAS
//   4. the 960x640 .depot copy, for a 2048x1536 path
//
// MEASURED 2026-10-21, archive 20261021-183146-eden-massetgate-manifest: a
// two-candidate form that mirrored the resolver's own order resolved only the
// 480x320 paths (18 of 58) and called the other 40 absent -- and every one of
// those 40 IS present in the volumes, so that count was a statement about the
// candidate list and not about the data.
//
// THE FIRST VERSION OF THIS FUNCTION HAD THE OFF-BY-ONE THAT CAUSED IT: it
// compared the resolution SEGMENT LENGTH against 8 for "2048x1536" and 6 for
// "960x640". Both are wrong -- "2048x1536" is NINE characters and "960x640"
// is SEVEN -- so only "480x320" (seven) ever matched. The length is now
// never used to identify the resolution: the segment is copied out and
// compared as a STRING.
void manifestCandidates(const char* logical, char out[4][384], unsigned& countOut) {
    static const char kPrefix[] = "/gui_assets/";
    countOut = 0;
    if (logical == nullptr) {
        return;
    }
    if (std::strncmp(logical, kPrefix, sizeof(kPrefix) - 1) != 0) {
        return;
    }
    const char* resolution = logical + sizeof(kPrefix) - 1;
    const char* slash = std::strchr(resolution, '/');
    if (slash == nullptr || slash[1] == '\0') {
        return;
    }
    const size_t resolutionLength = static_cast<size_t>(slash - resolution);
    char resolutionName[16];
    if (resolutionLength >= sizeof(resolutionName)) {
        return;
    }
    std::memcpy(resolutionName, resolution, resolutionLength);
    resolutionName[resolutionLength] = '\0';
    const char* relative = slash + 1;
    if (std::strcmp(resolutionName, "2048x1536") == 0) {
        std::snprintf(out[0], sizeof(out[0]),
                      "rr3gui0:/files/.depot/gui_assets/2048x1536/%s", relative);
        std::snprintf(out[1], sizeof(out[1]),
                      "rr3core:/files/apk/res/gui_assets/2048x1536/%s", relative);
        std::snprintf(out[2], sizeof(out[2]),
                      "rr3gui1:/files/.depot/gui_assets/960x640/%s", relative);
        countOut = 3;
    } else if (std::strcmp(resolutionName, "960x640") == 0) {
        std::snprintf(out[0], sizeof(out[0]),
                      "rr3gui1:/files/.depot/gui_assets/960x640/%s", relative);
        std::snprintf(out[1], sizeof(out[1]),
                      "rr3core:/files/apk/res/gui_assets/960x640/%s", relative);
        countOut = 2;
    } else if (std::strcmp(resolutionName, "480x320") == 0) {
        std::snprintf(out[0], sizeof(out[0]),
                      "rr3gui1:/files/.depot/gui_assets/480x320/%s", relative);
        std::snprintf(out[1], sizeof(out[1]),
                      "rr3core:/files/apk/res/gui_assets/480x320/%s", relative);
        countOut = 2;
    }
}

// True once the completed manifest has been written and can be served.
bool gCompletedManifestReady = false;

const char* completedManifestPath() {
    return gCompletedManifestReady ? kCompletedManifestPath : nullptr;
}

// The asset paths the guest has downloaded, one per line.
constexpr const char* kAssetPathFile =
    "sdmc:/switch/rr3-native-rehost/cache/asset_paths.txt";

// The asset paths the guest has asked for, with the digest of the bytes the
// port serves for each of them.
//
// ONE function, used by BOTH the base-list rewrite and the completed
// gui-assets manifest, so the two cannot disagree about a digest.
//
// The digest comes from the SAME resolver the pack endpoint uses, so the
// bytes a list names and the bytes the endpoint sends are the same bytes by
// construction.
// ONE ASSET, TWO SPELLINGS, ONE KEY.
//
// The guest writes a download to "<asset path>.ast" under ITS OWN spelling, and
// the asset lists name the same file under the OTHER one: the update list says
// "/assets_960x640/common/sprites.android.atlas" and the base list says
// "/gui_assets/960x640/common/sprites.android.atlas". The override set is keyed
// by the path STRING, so an entry the guest produced under one spelling never
// matched the line the other spelling names -- and the base list kept the
// SHIPPED digest for it.
//
// MEASURED 2026-10-22: the shipped base list (files/.depot/asset_list_base.
// txtCache.txt in core.romfs, 4,212,795 B, 40,863 lines) names
//   /gui_assets/960x640/common/sprites.android.atlas
//       bba63527235d60c88863fa10e111abc6
// while the port served, and the update list named,
//   c72568828875f4e2a6b11f5c944e27e0   (the volume's copy, 39,199 B).
// The guest compared the bytes it had with bba63527..., answered "didn't match
// its MD5" 12 times, and NEVER INSTALLED the file -- the rename that installs
// a download only happens AFTER the check passes. The card's installed copy of
// that atlas still carries its old mtime, which is the physical proof.
//
// So the override set carries BOTH spellings of every asset it knows.
void appendOverrideWithBothSpellings(
    std::vector<std::pair<std::string, std::string>>& out,
    const std::string& path, const std::string& digest) {
    out.emplace_back(path, digest);
    std::string tail = path;
    size_t begin = 0;
    while (begin < tail.size() && tail[begin] == '/') {
        ++begin;
    }
    tail = tail.substr(begin);
    const std::string prefix = "files/.depot/";
    if (tail.compare(0, prefix.size(), prefix) == 0) {
        tail = tail.substr(prefix.size());
    }
    char other[0x500]{};
    if (!alternateAssetSpelling(tail.c_str(), other, sizeof(other))) {
        return;
    }
    out.emplace_back(std::string("/") + other, digest);
}

void collectAssetOverrides(std::vector<std::pair<std::string, std::string>>& out,
                           unsigned* unresolvedOut = nullptr) {
    out.clear();
    if (unresolvedOut != nullptr) {
        *unresolvedOut = 0;
    }
    std::vector<std::string> requested;
    std::FILE* file = std::fopen(kAssetPathFile, "rb");
    if (file == nullptr) {
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string path = line;
        while (!path.empty() &&
               (path.back() == '\n' || path.back() == '\r')) {
            path.pop_back();
        }
        size_t begin = 0;
        while (begin < path.size() && path[begin] == '/') {
            ++begin;
        }
        if (begin >= path.size()) {
            continue;
        }
        path = "/" + path.substr(begin);
        if (path[0] != '/') {
            continue;
        }
        requested.push_back(path);
    }
    std::fclose(file);
    // DEDUPLICATE BEFORE RESOLVING. The card's asset_paths.txt is APPENDED to
    // on every download, so it carries the same path many times over:
    // MEASURED 2026-10-22, 1,388 lines and 47 distinct paths. Resolving every
    // line read and hashed the same file 1,388 times.
    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()),
                    requested.end());
    for (const std::string& path : requested) {
        char source[0x600]{};
        char digest[33]{};
        unsigned long long size = 0;
        if (!resolveAssetLocalFile(path.c_str(), source, sizeof(source), digest,
                                  &size)) {
            if (unresolvedOut != nullptr) {
                ++*unresolvedOut;
            }
            continue;
        }
        appendOverrideWithBothSpellings(out, path, std::string(digest));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}
// A forward declaration: the definition sits next to the base-list rewrite,
// where the recorded-path file is in scope.
void collectAssetOverrides(std::vector<std::pair<std::string, std::string>>& out,
                           unsigned* unresolvedOut);

// Build the completed manifest and write it to the card. Returns true when
// the completed file exists and can be served.
bool buildCompletedGuiAssetsManifest() {
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();
    // 1. THE SHIPPED MANIFEST, byte for byte.
    static const char* const kShipped[] = {
        "rr3core:/files/apk/res/asset_list_gui_assets_core.txt",
        "rr3core:/files/.depot/asset_list_gui_assets_core.txt",
    };
    char* shipped = nullptr;
    size_t shippedLength = 0;
    for (const char* candidate : kShipped) {
        std::FILE* file = std::fopen(candidate, "rb");
        if (file == nullptr) {
            continue;
        }
        if (std::fseek(file, 0, SEEK_END) != 0) {
            std::fclose(file);
            continue;
        }
        const long size = std::ftell(file);
        if (size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
            std::fclose(file);
            continue;
        }
        char* text = static_cast<char*>(std::malloc(static_cast<size_t>(size) + 1));
        if (text == nullptr) {
            std::fclose(file);
            break;
        }
        const size_t got = std::fread(text, 1, static_cast<size_t>(size), file);
        std::fclose(file);
        if (got != static_cast<size_t>(size)) {
            std::free(text);
            continue;
        }
        text[got] = '\0';
        shipped = text;
        shippedLength = got;
        break;
    }
    if (shipped == nullptr) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "manifest-build", 0, 0,
                            "FAILED reason=no-shipped-manifest");
        return false;
    }
    // 2. THE FILES the shipped manifest does not name.
    ManifestAsset assets[kManifestMaxAssets]{};
    unsigned count = 0;
    unsigned overflow = 0;
    for (const char* resolution : kManifestResolutions) {
        const char* device =
            (std::strcmp(resolution, "2048x1536") == 0) ? "rr3gui0" : "rr3gui1";
        for (const char* feature : kManifestFeatures) {
            char logical[256];
            std::snprintf(logical, sizeof(logical), "/gui_assets/%s/%s/",
                          resolution, feature);
            char depotRelative[256];
            std::snprintf(depotRelative, sizeof(depotRelative),
                          "files/.depot/gui_assets/%s/%s", resolution, feature);
            collectManifestAssets(device, depotRelative, logical, assets, count, overflow);
            char coreRelative[256];
            std::snprintf(coreRelative, sizeof(coreRelative),
                          "files/apk/res/gui_assets/%s/%s", resolution, feature);
            collectManifestAssets("rr3core", coreRelative, logical, assets, count, overflow);
        }
    }
    // 3. THE LINES. Each one carries the md5 and the size of the file the
    //    port serves at that path, computed from the file's own bytes.
    const size_t capacity = shippedLength + static_cast<size_t>(count) * 320u + 64u;
    char* text = static_cast<char*>(std::malloc(capacity));
    if (text == nullptr) {
        std::free(shipped);
        return false;
    }
    std::memcpy(text, shipped, shippedLength);
    size_t used = shippedLength;
    std::free(shipped);
    // The shipped file's LAST line is TRUNCATED mid-digest -- it ends
    // ".../store/packs/sprites_0.ptc.pvr.z\tcdbcb25a9b102c81d7b5daf6d4c",
    // 28 hex characters instead of 32 and no size at all -- so a newline has
    // to go in before anything is appended, or the first appended line would
    // be glued onto that fragment and the manifest would name a file that
    // does not exist.
    if (used > 0 && text[used - 1] != '\n') {
        text[used++] = '\n';
    }
    // 2b. THE SHIPPED LINES' MD5 COLUMN, FOR THE ASSETS THE GUEST ASKS FOR.
    //
    // THE GUEST CHECKS ITS DOWNLOAD AGAINST *THIS* MANIFEST. The shipped
    // manifest (files/apk/res/asset_list_gui_assets_core.txt, 365,312 B, 3,292
    // entries) carries EA's own digests for the game's own build, and the port
    // serves a DIFFERENT revision of some of those files -- so the guest
    // compared the bytes it received against a digest that names bytes this
    // port does not have, and answered "didn't match its MD5".
    //
    // MEASURED: the shipped manifest names
    //   /gui_assets/960x640/common/sprites.android.atlas
    // with md5 bba63527235d60c88863fa10e111abc6, which is NEITHER the
    // card's copy (43213527..., 24,689 B) NOR the volume's (c7256882...,
    // 39,199 B). No bytes the port can serve match it, so the asset could
    // never pass its own check.
    //
    // The rewrite touches only the paths the guest actually asked for (47 of
    // them, MEASURED), so the manifest keeps its shipped layout, its entry
    // count and its order -- only the md5 and the size of an entry the port
    // serves differently change, and they change to the digest of the file
    // the port really serves there.
    {
        // THE SHARED OVERRIDE SET -- the same set the base list and the
        // per-category gui list are rewritten from, so the manifest and the
        // lists publish ONE digest for a path they all name.
        const std::vector<std::pair<std::string, std::string>>& overrides =
            sharedAssetOverrides();
        if (!overrides.empty()) {
            const auto byPath = [](const std::pair<std::string, std::string>& entry,
                                  const std::string& path) {
                return entry.first < path;
            };
            std::string patched;
            patched.reserve(used + 4096);
            size_t position = 0;
            unsigned patchedCount = 0;
            while (position < used) {
                const size_t end = std::strchr(text + position, '\n') != nullptr
                    ? (std::strchr(text + position, '\n') - text) : used;
                const std::string line(text + position, end - position);
                position = (end < used) ? end + 1 : used;
                const size_t tab = line.find('\t');
                if (tab == std::string::npos) {
                    patched += line;
                    patched += '\n';
                    continue;
                }
                const std::string path = line.substr(0, tab);
                auto it = std::lower_bound(overrides.begin(), overrides.end(),
                                           path, byPath);
                if (it == overrides.end() || it->first != path) {
                    patched += line;
                    patched += '\n';
                    continue;
                }
                // THE PORT-SERVED DIGEST, and the size the port really sends.
                char source[0x600]{};
                char digest[33]{};
                unsigned long long size = 0;
                if (!resolveAssetLocalFile(path.c_str(), source, sizeof(source),
                                          digest, &size)) {
                    patched += line;
                    patched += '\n';
                    continue;
                }
                patched += path;
                patched += '\t';
                patched += digest;
                patched += '\t';
                patched += std::to_string(size);
                patched += '\t';
                patched += std::to_string(size);
                patched += '\n';
                ++patchedCount;
            }
            std::memcpy(text, patched.data(), patched.size());
            used = patched.size();
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "manifest-patch", patchedCount, 0,
                                "the shipped gui-assets manifest's md5 column "
                                "rewritten to the port-served digests");
        }
    }
    unsigned emitted = 0;
    unsigned absent = 0;
    unsigned logged = 0;
    for (unsigned index = 0; index < count; ++index) {
        char candidates[4][384];
        unsigned candidateCount = 0;
        manifestCandidates(assets[index].logical, candidates, candidateCount);
        char digest[33];
        unsigned long long size = 0;
        bool found = false;
        const char* source = nullptr;
        // THE RESOLVER FIRST.
        //
        // The manifest's md5 has to describe the bytes the guest will READ
        // for that path, and the guest reads through the resolver -- so the
        // manifest has to ask the resolver, not the volume directly.
        //
        // It matters because of the GUI-ASSETS SPELLING ALIAS: the two
        // spellings of one logical asset now answer with ONE file, and that
        // file is the card's copy of it (the revision the server's update
        // list describes). A manifest that named the volume's copy would
        // name bytes the guest can no longer read, and the guest's own
        // validation of the manifest would fail on the difference.
        //
        // The explicit volume candidates stay as the FALLBACK, for the files
        // the resolver cannot reach at all (the volume's
        // files/apk/res/gui_assets/<res>/ tree holds .rgb.pvr.z files the
        // gui_assets tree does not).
        {
            char resolved[0x600]{};
            if (resolveAssetLocalFile(assets[index].logical, resolved,
                                      sizeof(resolved), digest, &size)) {
                found = true;
                source = resolved;
            }
        }
        for (unsigned candidate = 0; !found && candidate < candidateCount; ++candidate) {
            struct stat info{};
            if (::stat(candidates[candidate], &info) != 0) {
                continue;
            }
            if (!md5File(candidates[candidate], digest, &size)) {
                continue;
            }
            found = true;
            source = candidates[candidate];
            break;
        }
        if (!found) {
            // The file is genuinely not reachable from any of the places the
            // port could serve it. No line is invented for it; the count goes
            // into the log line instead.
            ++absent;
            if (logged < 80) {
                ++logged;
                char diag[640];
                std::snprintf(diag, sizeof(diag),
                              "path=%s candidates=%u ABSENT", assets[index].logical,
                              candidateCount);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "manifest-asset", logged, 0, diag);
            }
            continue;
        }
        const int written = std::snprintf(text + used, capacity - used,
                                          "%s\t%s\t%llu\t%llu\n",
                                          assets[index].logical, digest, size, size);
        if (written <= 0 || static_cast<size_t>(written) >= capacity - used) {
            break;
        }
        used += static_cast<size_t>(written);
        ++emitted;
        if (logged < 80) {
            ++logged;
            char diag[640];
            std::snprintf(diag, sizeof(diag),
                          "path=%s md5=%s size=%llu from=%s",
                          assets[index].logical, digest, size, source);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "manifest-asset", logged, 0, diag);
        }
    }
    // 4. WRITE IT, through a temporary name, so a reader never sees a
    //    half-written manifest.
    //
    //    THE TARGET IS REMOVED FIRST. libnx's fsdev rename does NOT overwrite
    //    an existing file: MEASURED 2026-10-21, archive
    //    20261021-184611-eden-massetgate-manifest3 -- the build produced
    //    371,191 bytes, the temporary file held them, and the target still
    //    held the PREVIOUS run's 367,109 bytes, because the rename failed
    //    against a file that was already there. The manifest the guest then
    //    read was the previous run's, which is the worst possible outcome: a
    //    stale completion that looks like a fresh one.
    std::FILE* output = std::fopen(kCompletedManifestTempPath, "wb");
    bool written = false;
    int writeErrno = 0;
    if (output != nullptr) {
        const size_t bytes = std::fwrite(text, 1, used, output);
        const bool flushed = (bytes == used);
        const int closeResult = std::fclose(output);
        writeErrno = errno;
        written = flushed && (closeResult == 0);
        if (written) {
            std::remove(kCompletedManifestPath);
            written = std::rename(kCompletedManifestTempPath, kCompletedManifestPath) == 0;
            if (!written) {
                writeErrno = errno;
            }
        }
    }
    else {
        writeErrno = errno;
    }
    std::free(text);
    char diag[512];
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    std::snprintf(diag, sizeof(diag),
                  "files=%u emitted=%u absent=%u overflow=%u shipped=%zu out=%zu ms=%llu written=%d errno=%d path=%s",
                  count, emitted, absent, overflow, shippedLength, used,
                  (unsigned long long)elapsedMs, written ? 1 : 0, writeErrno,
                  kCompletedManifestPath);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "manifest-build", 0, 0, diag);
    return written;
}

bool isGuiAssetsManifestPath(const char* guestPath) {
    return guestPath != nullptr &&
           std::strcmp(pathBasename(guestPath), kGuiAssetsManifestLeaf) == 0;
}

// The asset list the guest checks its downloads against. The guest's own log
// names it: "Assets are added asset_list_updates.txt".
constexpr const char* kUpdatesListLeaf = "asset_list_updates.txt";
// The guest path the list is read under.
constexpr const char* kUpdatesListGuestPath = "files/.depot/asset_list_updates.txt";
// The rewritten copy, on the card. The cache root is already a writable root
// (ensureWritableRoots), so this needs no new directory.
//
// IT IS NOT WRITTEN OVER THE ORIGINAL on purpose: the guest owns
// .depot/asset_list_updates.txt and may write it, and a rewritten copy that
// the guest can overwrite would put the server's md5s back. This copy is
// served INSTEAD of the original, the same way the completed gui-assets
// manifest is.
constexpr const char* kConsistentListPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_updates.txt";
constexpr const char* kConsistentListTempPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_updates.txt.tmp";

// The rewritten list, when there is one. Empty until the build finishes.
const char* gConsistentListServed = nullptr;

// True when the guest path is the asset list the port rewrites.
bool isConsistentListPath(const char* guestPath) {
    return guestPath != nullptr &&
           std::strcmp(pathBasename(guestPath), kUpdatesListLeaf) == 0;
}

// ---------------------------------------------------------------------------
// THE BASE ASSET LIST -- THE BIG ONE -- AND THE LAZY WAY TO KEEP IT TRUE.
//
// WHY IT EXISTS.
//
//   The guest does not only read asset_list_updates.txt. It also reads
//   files/.depot/asset_list_base.txtCache.txt (4,212,795 B, 40,862 entries,
//   "<path>\t<md5>") and uses THAT list's md5s for the assets it downloads
//   next -- the sprite atlases, MEASURED 2026-10-22 in archive
//   20261022-005538-eden-md5list-r3: the guest wrote
//   .depot/gui_assets/960x640/splash/formula_e/sprites.android.atlas.ast.
//
//   MEASURED, and this is the finding that makes the rewrite necessary: that
//   list describes the SERVER's newer revision, and the port's bytes are the
//   260530 build. For /gui_assets/960x640/splash/formula_e/
//   sprites.android.atlas the list says 98e585e2... and the file the port
//   serves hashes to b8a3896c... -- so the guest's md5 check would fail on
//   every atlas. (For most of the OTHER entries -- /crew.bin, /economy.bin,
//   the tracks, the vehicles -- the list's md5 DOES match the port's bytes,
//   because those files came out of the same data pack. The atlases are the
//   ones the server has moved on from.)
//
// THE COST, MEASURED, AND WHY THE REWRITE CANNOT BE DONE IN FULL AT BOOT.
//
//   Every one of the 40,862 entries resolves to a local file, and together
//   they are 9.394 GB (measured on the deployed data set, 2026-10-21). Hashing
//   that on Eden costs 22-45 MB/s -- the update list's 3.894 GB... no: its
//   3.894 MB took 173 ms, which is 22.5 MB/s -- so the full rewrite is
//   210-420 SECONDS of boot time. The guest is on the critical path and a
//   multi-second stall is not acceptable, so the full rewrite is out.
//
//   So the rewrite is LAZY, and it is driven by the one thing the port knows
//   for certain: WHICH ASSETS THE GUEST ACTUALLY ASKED FOR. The guest writes
//   every downloaded asset to "<asset path>.ast" before it checks it, so the
//   filesystem layer sees the asset path at the moment it is asked for -- no
//   change to the Cloudcell layer is needed, and the record is on the card
//   before the guest can ask again.
//
//   The recorded paths are hashed ONCE, at the next boot, and the base list is
//   rewritten with those digests. The entries the guest has not asked for
//   keep the server's md5, which is what they are today: the port is not
//   claiming to have them.
//
// THE SPELLING. The two lists spell the same file two ways: the update list
//   says "/assets_2048x1536/common/sprites.android.atlas" and the base list
//   says "/gui_assets/2048x1536/common/sprites.android.atlas". They are the
//   same file -- that is exactly what the resolver's third candidate says
//   ("files/.depot/gui_assets/<res>" + rest) -- so a lookup that misses is
//   retried with the other spelling.
// ---------------------------------------------------------------------------

// The base asset list, the leaf the guest reads it under.
constexpr const char* kBaseListLeaf = "asset_list_base.txtCache.txt";
// The guest path the base list is read under.
constexpr const char* kBaseListGuestPath = "files/.depot/asset_list_base.txtCache.txt";
// The rewritten base list, on the card.
constexpr const char* kConsistentBaseListPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_base.txtCache.txt";
// The key of the override set the rewritten base list was built from.
// Beside the list itself, so a card that is wiped loses the cache with it.
constexpr const char* kBaseListKeyPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_base.txtCache.txt.key";

// TRUE when the rewritten base list on the card was built from THIS override
// set, and the pass can be skipped.
//
// The rewritten list is a pure function of the shipped list (read-only, never
// changes) and the override set, so an unchanged override set means the
// file on the card is already correct. The key file is 33 bytes; reading it
// and the list's own size is the whole cost of the check.
//
// The list's OWN size is checked too, because the key alone would happily
// bless a list that was deleted or truncated after the key was written.
bool baseListCacheIsCurrent(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    std::FILE* keyFile = std::fopen(kBaseListKeyPath, "rb");
    if (keyFile == nullptr) {
        return false;
    }
    char stored[64]{};
    size_t got = std::fread(stored, 1, sizeof(stored) - 1, keyFile);
    std::fclose(keyFile);
    if (got == 0) {
        return false;
    }
    while (got > 0 && (stored[got - 1] == '\n' || stored[got - 1] == '\r' ||
                       stored[got - 1] == ' ')) {
        stored[--got] = '\0';
    }
    if (std::strcmp(stored, key) != 0) {
        return false;
    }
    struct stat info{};
    if (::stat(kConsistentBaseListPath, &info) != 0 || info.st_size <= 0) {
        return false;
    }
    return true;
}

// Records the override set the rewritten base list was built from.
void writeBaseListCacheKey(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", kBaseListKeyPath);
    std::FILE* file = std::fopen(tempPath, "wb");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%s\n", key);
    std::fclose(file);
    std::remove(kBaseListKeyPath);
    std::rename(tempPath, kBaseListKeyPath);
}

// The rewritten base list, when there is one.
const char* gConsistentBaseListServed = nullptr;

// True when the guest path is the base asset list the port rewrites.
bool isConsistentBaseListPath(const char* guestPath) {
    return guestPath != nullptr &&
           std::strcmp(pathBasename(guestPath), kBaseListLeaf) == 0;
}

// ---------------------------------------------------------------------------
// THE PER-CATEGORY GUI ASSET LIST -- "asset_list_base_gui_assets.txt".
//
// WHY IT EXISTS.
//
//   The guest does not read ONE asset list. It reads the big base list
//   (files/.depot/asset_list_base.txtCache.txt, 4,212,795 B) AND the
//   per-category lists under files/apk/res/ ("asset_list_base_<category>.txt")
//   and merges them into its own .depot/asset_list_base.txtCache.txt, which
//   is the table the download check compares against.
//
//   MEASURED 2026-10-23: the merged list the guest produced on the card had
//   NO entry for /gui_assets/960x640/common/sprites.android.atlas at all --
//   not under the "gui_assets" spelling and not under the "assets_960x640"
//   one -- while every other asset's entry survived. A merge that DROPS an
//   entry is a merge that saw the same path twice with two DIFFERENT
//   digests, and the conflict is resolved by removing the entry.
//
//   Two lists published two different digests for that one path:
//
//     files/.depot/asset_list_base.txtCache.txt   (REWRITTEN by the port)
//         /gui_assets/960x640/common/sprites.android.atlas
//         c72568828875f4e2a6b11f5c944e27e0   <- the bytes the port serves
//     files/apk/res/asset_list_base_gui_assets.txt   (298,223 B)
//         /gui_assets/960x640/common/sprites.android.atlas
//         bba63527235d60c88863fa10e111abc6   <- EA's factory digest
//
//   The second one was served VERBATIM out of the read-only volume, because
//   the rewrite machinery only knew about the update list and the big base
//   list. bba63527... describes NEITHER the card's copy of the atlas nor the
//   volume's copy (MEASURED: 43213527..., 24,689 B, and c7256882...,
//   39,199 B), so no bytes the port can serve ever matched it, and the guest
//   answered "didn't match its MD5" 24 times -- for this one asset only.
//
//   So the per-category list is rewritten the same way the base list is: the
//   SAME override set, the SAME exact-path lookup, the same "only the md5
//   column changes" rule. After the rewrite the two lists publish the SAME
//   digest for the asset, the merge keeps the entry, and the compare has a
//   digest to compare against.
//
//   The override set is the SHARED one (sharedAssetOverrides()) -- the same
//   set the completed gui-assets manifest, the base list's own pass, the
//   census and the inventory use -- so the two lists are rewritten from ONE
//   set by construction and cannot disagree about a path they both name.
// ---------------------------------------------------------------------------

// The per-category list, the leaf the guest reads it under.
constexpr const char* kGuiBaseListLeaf = "asset_list_base_gui_assets.txt";
// The guest path the list is read under.
constexpr const char* kGuiBaseListGuestPath = "files/apk/res/asset_list_base_gui_assets.txt";
// The rewritten copy, on the card. The cache root is already a writable root
// (ensureWritableRoots), so this needs no new directory.
constexpr const char* kConsistentGuiBaseListPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_base_gui_assets.txt";
// The key of the override set the rewritten list was built from, beside the
// list itself, so a card that is wiped loses the cache with it.
constexpr const char* kGuiBaseListKeyPath =
    "sdmc:/switch/rr3-native-rehost/cache/asset_list_base_gui_assets.txt.key";

// What the port knows about the rewritten per-category list, so the serve
// line can name the file the list came from, the file that is served, and
// the counts -- on a rebuilt run AND on a cached one.
struct ConsistentGuiList {
    const char* served{nullptr};
    char source[0x600]{};
    char destination[0x600]{};
    unsigned entries{0};
    unsigned rewritten{0};
    unsigned kept{0};
};
ConsistentGuiList gConsistentGuiList{};

// True when the guest path is the per-category GUI asset list the port
// rewrites.
//
// THE LEAF, NOT THE WHOLE PATH: the guest asks for this list under more
// than one root (files/apk/res/... and files/.depot/... both name it), and
// the base list's own predicate works the same way.
bool isConsistentGuiBaseListPath(const char* guestPath) {
    return guestPath != nullptr &&
           std::strcmp(pathBasename(guestPath), kGuiBaseListLeaf) == 0;
}

// THE ASSET PATH behind a guest path, in the spelling the asset lists use.
//
// The guest writes a downloaded asset to "<asset path>.ast", under its own
// app root ("sdmc:/switch/rr3-native-rehost/.depot/...") or under the depot
// root ("files/.depot/..."). Both are stripped, and the ".ast" goes, so what
// is left is the path the lists name.
bool assetPathFromGuestPath(const char* guestPath, char* out, size_t outSize) {
    if (guestPath == nullptr || out == nullptr || outSize == 0) {
        return false;
    }
    out[0] = '\0';
    std::string path = guestPath;
    // The ".ast" suffix.
    if (path.size() < 5u || path.compare(path.size() - 4u, 4u, ".ast") != 0) {
        return false;
    }
    path.erase(path.size() - 4u);
    // The app root, then the depot root.
    const std::string appRoot = std::string(kAppRoot) + "/.depot/";
    const std::string depotRoot = "files/.depot/";
    size_t start = 0;
    if (path.compare(0, appRoot.size(), appRoot) == 0) {
        start = appRoot.size();
    }
    else if (path.compare(0, depotRoot.size(), depotRoot) == 0) {
        start = depotRoot.size();
    }
    else {
        return false;
    }
    if (start >= path.size()) {
        return false;
    }
    // The guest joins a directory that ends in "/" with a name that starts
    // in "/", so the remainder can begin with a second separator. MEASURED:
    // the first version of this wrote "//MenuBannerData.xml" and every one
    // of those keys missed the list, so the rewrite changed nothing.
    while (start < path.size() && path[start] == '/') {
        ++start;
    }
    if (start >= path.size()) {
        return false;
    }
    const int written = std::snprintf(out, outSize, "/%s", path.c_str() + start);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

// Record the asset path the guest has just downloaded.
//
// THE CARD IS THE MEMORY: the list has to survive the run, because the
// rewrite happens at the NEXT boot -- the guest has already read the list by
// the time it downloads anything.
void recordAssetPath(const char* guestPath) {
    char assetPath[512];
    if (!assetPathFromGuestPath(guestPath, assetPath, sizeof(assetPath))) {
        return;
    }
    std::FILE* file = std::fopen(kAssetPathFile, "a");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%s\n", assetPath);
    std::fclose(file);
}

// ---------------------------------------------------------------------------
// THE LIVERY-ONLY CARS: a car that ships ONLY its livery, and the car whose
// body it reuses.
//
// MEASURED 2026-10-31, over the q30 archive, all seven ROMFS volumes and the
// three source texture packs: the 2021 Porsche 911 RSR's directory in
// vehicles-modern.romfs holds exactly ONE file
// (livery/2021_porsche_911_rsr_ext.dxt.dds.z, 189,918 B) and NO body
// texture at all -- and the guest's own car definition
// (vehicles/data/452_2021_porsche_911_rsr.bin.nct, 968 B), decrypted with the
// guest's OWN RC4 variant (tools/rr3-plist.py), names "2020_911_RSR" as its
// base car.  So the body was never published under the 2021 name: the 2021
// car IS a livery over the 2020 body.
//
// WITHOUT THE ALIAS the guest asks for a body texture that does not exist
// (30 log lines in five bursts, ENOENT on FIVE different device paths) and
// falls back to "Warning: Using default AI values for '2021 Porsche 911
// RSR'" -- the car renders with default values instead of its own.
//
// WHY IT BELONGS IN THE FILE LAYER AND NOT IN THE ASSET CANDIDATE LIST.
// The request is a guest FILESYSTEM stat (guestStat), not a pack-endpoint
// request: "phase=cc-asset-req" has 53 lines in the whole log and mentions
// this car on ZERO of them.  And the path is in NO asset list -- the 34 base
// list entries that name this car are all gui_assets atlases and liveries,
// none of them a body texture -- so NO digest is published for it and the
// md5 contract is not touched by this alias at all.
//
// EVERY ENTRY IS EVIDENCE: the base car is the one the car definition names.
// It is not a guess on the year.
// ---------------------------------------------------------------------------
struct VehicleBodyAlias {
    const char* car;   // the livery-only car
    const char* base;  // the car whose body it reuses
};

constexpr VehicleBodyAlias kVehicleBodyAliases[] = {
    {"2021_porsche_911_rsr", "2020_porsche_911_rsr"},
};

constexpr unsigned kVehicleBodyAliasCount = static_cast<unsigned>(
    sizeof(kVehicleBodyAliases) / sizeof(kVehicleBodyAliases[0]));

// Rewrites every occurrence of the car's name in a GUEST path.
//
// THE CAR NAME IS IN THE DIRECTORY AND IN THE FILE NAME
// ("<car>_sha.dxt.dds.z"), so both have to change -- otherwise the alias
// asks for a file that cannot exist under any name.
//
// BUT ONLY THE BODY TEXTURE IS ALIASED, AND THAT RESTRICTION IS THE WHOLE
// POINT.  The first version rewrote EVERY path under the car's directory,
// and that broke the run: the car's LIVERY files ("<car>_ext.dxt.dds.z" and
// the per-part "<car>_lights.ptc.pvr.z") DO exist under their own name AND
// they ARE named in the asset lists, so rewriting them changed the bytes the
// port serves under a path whose published digest is the ORIGINAL one's --
// and the guest's md5 check then rejected the download and the boot blocked
// with no error anywhere.
//
// So the alias is restricted to the "_sha" family, which is (a) PROVEN missing
// for this car, (b) PROVEN to exist under the base car, and (c) NOT named in
// any asset list, so no published digest describes it and none is broken.
bool rewriteVehicleBodyAlias(const char* guestPath, char* out, size_t outSize) {
    if (guestPath == nullptr || out == nullptr || outSize == 0u) {
        return false;
    }
    for (unsigned i = 0; i < kVehicleBodyAliasCount; ++i) {
        const char* from = kVehicleBodyAliases[i].car;
        const char* to = kVehicleBodyAliases[i].base;
        if (std::strstr(guestPath, from) == nullptr) {
            continue;
        }
        // ONLY THE LEAF THAT IS THE BODY TEXTURE.
        const char* leaf = std::strrchr(guestPath, '/');
        leaf = (leaf != nullptr) ? (leaf + 1) : guestPath;
        if (std::strstr(leaf, "_sha") == nullptr) {
            continue;
        }
        const size_t fromLength = std::strlen(from);
        const size_t toLength = std::strlen(to);
        size_t written = 0;
        const char* cursor = guestPath;
        bool replaced = false;
        while (*cursor != '\0') {
            const char* hit = std::strstr(cursor, from);
            const size_t chunk = (hit != nullptr)
                                     ? static_cast<size_t>(hit - cursor)
                                     : std::strlen(cursor);
            if (written + chunk >= outSize) {
                return false;
            }
            std::memcpy(out + written, cursor, chunk);
            written += chunk;
            cursor += chunk;
            if (hit == nullptr) {
                break;
            }
            if (written + toLength >= outSize) {
                return false;
            }
            std::memcpy(out + written, to, toLength);
            written += toLength;
            cursor = hit + fromLength;
            replaced = true;
        }
        out[written] = '\0';
        if (replaced) {
            return true;
        }
    }
    return false;
}

template <typename Callback>
int withReadFallback(const Resolution& resolution, const char* guestPath,
                     Callback callback, const char** servedOut = nullptr) {
    const bool probe = resolution.primary != nullptr &&
                       std::strstr(resolution.primary, kProbeSubtree) != nullptr;
    // The asset-list requests are traced here because this is the only place
    // that knows WHICH candidates the host tried and what the last attempt
    // left behind. The guest path is passed in because the resolver is the
    // only place that knows what the guest asked for.
    const bool assetList = isAssetListPath(guestPath);
    // WHICH CANDIDATE ANSWERED. Until now nothing said so: the resolver
    // builds up to three candidates (the volume primary, the volume alias
    // and the SD card), and the caller only ever saw the RESULT. "size=-1"
    // on an fopen line is the stat of the PRIMARY, so it said "the primary
    // does not exist" and NOT "the open failed" -- and it said nothing at
    // all about which tree actually served the bytes.
    //
    // The pointer is into the Resolution (or into the local card path), and
    // the caller reads it before the Resolution goes out of scope.
    if (servedOut != nullptr) {
        *servedOut = nullptr;
    }
    // Counts the attempts this one logical lookup makes, so the trace can say
    // "3 candidates, last errno=2" instead of leaving the reader to infer it.
    unsigned attempts = 0;
    int lastErrno = 0;
    const auto attempt = [&](const char* candidate) {
        ++attempts;
        const int rc = callback(candidate);
        lastErrno = errno;
        if (probe) {
            char diag[640];
            __builtin_snprintf(diag, sizeof(diag),
                "try[%u/%s] rc=%d errno=%d %s", attempts,
                gResolveApi != nullptr ? gResolveApi : "?", rc, errno, candidate);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fs-candidate", 249, 0, diag);
        }
        return rc;
    };
    // THE TWO CANDIDATES: the volume primary and its alias. The app-local
    // card copy of the primary is still tried at the END of the chain (see
    // below), which is where the guest's own downloads live.
    const char* const candidates[2] = {resolution.primary, resolution.fallback};
    const int candidateCount = 2;
    int result = -1;
    int firstError = ENOENT;
    // THE COMPLETED GUI-ASSETS MANIFEST, SERVED INSTEAD OF THE VOLUME COPY.
    //
    // One file, one leaf name. The completed copy is the shipped manifest
    // plus the lines for the features the shipped manifest does not name, and
    // it has to be the copy the guest sees: the appended lines are PAST THE
    // END of the shipped text, so a stat that still said 365,312 would cut
    // them off and the guest would rebuild the same incomplete registry.
    //
    // The candidates below are still tried, in order, if the completed copy
    // is not there -- so a card that cannot be written costs the completion
    // and nothing else.
    if (isGuiAssetsManifestPath(guestPath)) {
        const char* completed = completedManifestPath();
        if (completed == nullptr) {
            char diag[320];
            std::snprintf(diag, sizeof(diag),
                          "guest=%.200s completed=NONE (the build did not finish)",
                          guestPath != nullptr ? guestPath : "-");
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "manifest-serve", 0, 0, diag);
        }
        if (completed != nullptr) {
            ++attempts;
            const int rc = attempt(completed);
            // The serve attempt is logged EVERY time, because "the completed
            // manifest was built and the guest still read the shipped one" is
            // only distinguishable from "the completed manifest was never
            // offered" with this line on record.
            {
                char diag[640];
                std::snprintf(diag, sizeof(diag),
                              "guest=%.200s completed=%.200s rc=%d errno=%d",
                              guestPath != nullptr ? guestPath : "-", completed, rc, errno);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "manifest-serve", 0, 0, diag);
            }
            if (rc == 0) {
                if (servedOut != nullptr) {
                    *servedOut = completed;
                }
                if (assetList) {
                    logAssetListRequest(guestPath, resolution, false, 0, lastErrno);
                }
                return 0;
            }
        }
    }
    // THE CONSISTENT ASSET LISTS, served INSTEAD of the shipped ones.
    //
    // The guest MD5s what it receives and compares it with the md5 in an
    // asset list, so that list has to describe the bytes the port serves --
    // and the port serves the bytes it has. The rewritten copies are on the
    // card and are offered FIRST, so the guest never reads the shipped list.
    //
    // They are offered before the volume candidates and not after them
    // because the shipped copies live on READ-ONLY volumes: the rewritten
    // copies are the only ones the port can write, so they have to be the
    // ones it serves.
    {
        const char* consistent = nullptr;
        // TRUE when the copy on offer is the rewritten PER-CATEGORY GUI list,
        // so the serve line below can name its source, its destination and
        // its counts.
        bool guiList = false;
        if (gConsistentListServed != nullptr && isConsistentListPath(guestPath)) {
            consistent = gConsistentListServed;
        }
        else if (gConsistentBaseListServed != nullptr &&
                 isConsistentBaseListPath(guestPath)) {
            consistent = gConsistentBaseListServed;
        }
        else if (gConsistentGuiList.served != nullptr &&
                 isConsistentGuiBaseListPath(guestPath)) {
            consistent = gConsistentGuiList.served;
            guiList = true;
        }
        else {
            // EVERY OTHER ASSET LIST, rewritten from the SAME override set, so
            // that no two lists the guest merges can publish two digests for
            // one path. See buildRewrittenAssetLists() for why this is the
            // difference between the front end opening and the guest
            // drowning in "didn't match its MD5".
            const char* rewritten = rewrittenListPath(guestPath);
            if (rewritten != nullptr) {
                consistent = rewritten;
            }
        }
        if (consistent != nullptr) {
            ++attempts;
            const int rc = attempt(consistent);
            {
                char diag[640];
                std::snprintf(diag, sizeof(diag),
                              "guest=%.200s consistent=%.200s rc=%d errno=%d",
                              guestPath != nullptr ? guestPath : "-",
                              consistent, rc, errno);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "assetlist-serve", 0, 0, diag);
            }
            if (guiList) {
                // THE PER-CATEGORY LIST'S OWN SERVE LINE.
                //
                // It names the file the list was READ FROM, the file that is
                // SERVED, how many entries the list has and how many of them
                // were rewritten -- so a run can tell "the guest read the
                // rewritten list" from "the guest read the shipped one" and
                // from "the rewrite found nothing to change", which are three
                // different observations and used to look like one.
                char diag[1400];
                std::snprintf(diag, sizeof(diag),
                              "guest=%.200s source=%.300s out=%.300s entries=%u "
                              "rewritten=%u kept=%u rc=%d errno=%d",
                              guestPath != nullptr ? guestPath : "-",
                              gConsistentGuiList.source,
                              gConsistentGuiList.destination,
                              gConsistentGuiList.entries,
                              gConsistentGuiList.rewritten,
                              gConsistentGuiList.kept, rc, errno);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "assetlist-serve-gui", 0, 0, diag);
            }
            if (rc == 0) {
                if (servedOut != nullptr) {
                    *servedOut = consistent;
                }
                if (assetList) {
                    logAssetListRequest(guestPath, resolution, false, 0, lastErrno);
                }
                return 0;
            }
        }
    }
    for (int i = 0; i < candidateCount; ++i) {
        const char* candidate = candidates[i];
        if (candidate == nullptr || candidate[0] == '\0') {
            continue;
        }
        result = attempt(candidate);
        if (result == 0) {
            if (servedOut != nullptr) {
                *servedOut = candidate;
            }
            if (assetList) {
                logAssetListRequest(guestPath, resolution, false, 0, lastErrno);
            }
            // THE ".ast" ROUND-TRIP INSTRUMENT.
            //
            // One line that says, for a path ending in ".ast": what the guest
            // asked for, which candidate answered, the SIZE and the MD5 of
            // the bytes that came back, and -- for the card candidate -- the
            // size and the md5 of the file that is PHYSICALLY on the card at
            // that path.
            //
            // WHY IT IS NEEDED: the "size=" field of the open trace is the
            // stat of the PRIMARY, so a read-back that was answered by a
            // different file looks like a successful open of the right size.
            // That is the instrument mistake this project keeps making, and
            // it is the one that left the "the write did not persist" and "the
            // read served the wrong file" hypotheses indistinguishable.
            //
            // NO STACK, AND THAT IS THE POINT. The FIRST version of this
            // instrument called rr3::md5File(), whose buffer is
            // "unsigned char buffer[64 * 1024]" -- 64 KiB OF STACK, on the
            // GUEST's thread, inside the guest's own call chain. The guest
            // died with a stack fault during the asset-download phase (the
            // phase the .ast read-back happens in), Eden exited on its own,
            // and three runs in a row died that way: 163 s, 92 s, 92 s, with
            // the guest's log stopping mid-"StartListDownload()". The port's
            // own frames are a few kilobytes; 64 KiB is not.
            //
            // So the digest is computed here, from a file-scope buffer, with
            // a 4 KiB read chunk and no allocation on the stack at all.
            if (guestPath != nullptr && std::strstr(guestPath, ".ast") != nullptr) {
                char digest[33] = {0};
                unsigned long long bytes = 0;
                if (digestFileShallow(candidate, digest, sizeof(digest), &bytes)) {
                    char cardSize[32] = {"-"};
                    char cardDigest[40] = {"-"};
                    for (int slot = 0; slot < candidateCount; ++slot) {
                        const char* cardCandidate = candidates[slot];
                        if (cardCandidate == nullptr || cardCandidate[0] == '\0') {
                            continue;
                        }
                        if (std::strncmp(cardCandidate, "sdmc:", 5) != 0) {
                            continue;
                        }
                        struct stat onCard{};
                        if (::stat(cardCandidate, &onCard) != 0) {
                            continue;
                        }
                        char cardMd5[33] = {0};
                        if (!digestFileShallow(cardCandidate, cardMd5,
                                              sizeof(cardMd5), nullptr)) {
                            continue;
                        }
                        std::snprintf(cardSize, sizeof(cardSize), "%lld",
                                      static_cast<long long>(onCard.st_size));
                        std::snprintf(cardDigest, sizeof(cardDigest), "%s", cardMd5);
                        break;
                    }
                    char diag[1400];
                    std::snprintf(diag, sizeof(diag),
                                  "guest=%.200s primary=%.200s fallback=%.200s "
                                  "served=%.200s bytes=%llu md5=%s "
                                  "card_bytes=%s card_md5=%s",
                                  guestPath, resolution.primary, resolution.fallback,
                                  candidate, bytes, digest,
                                  cardSize, cardDigest);
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "ast-readback", 0, 0, diag);
                }
            }
            return 0;
        }
        const int error = errno;
        if (i == 0) {
            firstError = error;
        }
        if (error != ENOENT) {
            // Only ENOENT is worth retrying somewhere else; EACCES/EROFS/EISDIR
            // will come back identical from the next candidate.
            break;
        }
        // RETRACTED 2026-10-16: the ".z means try the plain name" fallback that
        // used to stand here. It did not help, it silently corrupted: 70 files
        // under files/.depot/vehicles/shaders/ come back
        //   deScrambleDataDecompression error: Z_DATA_ERROR
        // every run, and the host trace shows exactly why --
        //   try[1] rc=-1 errno=2  vehicles/shaders/fresnel_gloss.rgb.pvr.z
        //   try[2] rc=0  errno=0  vehicles/shaders/fresnel_gloss.rgb.pvr
        // The volume has 72 files there and every one is an uncompressed .rgb.pvr
        // with no .z sibling, while the guest asks for 69 of them as .z. Handing
        // back an uncompressed file for a .z request is a FALSE SUCCESS: the
        // guest feeds those bytes straight to inflate() unconditionally
        // (ldrsw x9,[x1]; add x2,x0,#4; sub x3,x9,#4; bl inflate at 0x15c0b18),
        // and a .z container is a 4-byte LE32 size followed by zlib. So the
        // fallback replaced an honest ENOENT with a guaranteed Z_DATA_ERROR.
        //
        // An honest ENOENT is strictly better: the guest enumerates each texture
        // as a format list and issues SEPARATE requests for the spellings it
        // wants -- both ...etc.dds.z and ...etc.dds appear as distinct path
        // lines in the same log -- and walks dxt.dds -> etc.dds -> rgb.pvr. A
        // true miss on .z lets it proceed to the plain name, which exists.
        // Truncation and routing were both ruled out: 0 corrupt .z files across
        // the volumes, and rr3veh0 is the correct device.
    }
    // THIRD CANDIDATE: the same content, taken off the SD card.
    //
    // Every volume path is shaped "<device>:/files/<rest>". The card holds the
    // same tree under "sdmc:/switch/rr3-native-rehost/<rest>", and Resolution has
    // only two slots -- for the apk/res/ family the second is already taken by
    // the volume's own ".depot" alias, so the card was never consulted for the
    // majority of requests. Deriving it from the primary instead of storing it
    // costs nothing and covers every path, whichever alias owns the slot.
    //
    // It is derived by appLocalPath() -- the SAME function the write
    // redirect uses -- so the file a guest writes to the card and the file
    // the guest reads back are the same path by construction, not by two
    // derivations that happen to agree today.
    const char* primary = resolution.primary;
    if (primary != nullptr && std::strncmp(primary, "sdmc:", 5) != 0) {
        char card[0x500]{};
        if (appLocalPath(primary, card, sizeof(card))) {
            result = callback(card);
            lastErrno = errno;
            if (result == 0) {
                if (servedOut != nullptr) {
                    *servedOut = card;
                }
                if (assetList) {
                    logAssetListRequest(guestPath, resolution, false, 0,
                                        lastErrno);
                }
                return 0;
            }
            // Same retraction as the volume candidate above, and for the
            // same reason: a ".z" request must not be answered with the
            // uncompressed file. The guest inflates unconditionally.
        }
    }
    // FOURTH CANDIDATE: THE LIVERY-ONLY CAR'S BODY, from the car it reuses.
    //
    // The car name is rewritten in the GUEST path and the result is resolved
    // AGAIN, because the DEVICE changes with it: the 2021 car is routed to
    // rr3veh2 (year >= 2021) and the 2020 body lives in rr3veh1 (2016..2020).
    // A string replacement on the device path would look for the body in the
    // wrong volume.
    //
    // IT IS TRIED LAST, so a file that does exist under its own name always
    // wins: the 2021 car's OWN livery files are never shadowed by the base
    // car's, and the base car is only reached for the files that were never
    // published.
    if (result != 0 && guestPath != nullptr) {
        // THE BUFFERS LIVE ON THE HEAP, ON PURPOSE.
        //
        // withReadFallback() runs on the GUEST's threads, whose stacks are the
        // guest's own, and this port has already MEASURED a 64 KiB stack
        // buffer as fatal there (the candidate list in
        // rr3_asset_candidates.hpp is on the heap for the same reason).
        //
        // THE RESOLUTION GOES ON THE HEAP TOO, AND THAT WAS THE SECOND HALF
        // OF THE SAME BUG: Resolution is two 1,536-byte path slots plus two
        // flags, so a local one costs 2.5 KB of the guest thread's stack.
        // With it on the stack the port overflowed the guest thread, the
        // return address was corrupted, and the port kept running with a PC
        // INSIDE THE DATA (the port's own sampler reported "OUT-OF-RANGE
        // pc_off=0x1ce0eac"), which is what a corrupted stack looks like.
        // The symptom was a run that stopped making progress with no error
        // anywhere -- and the first attempt, which only moved the path
        // buffers to the heap, still had the Resolution on the stack.
        std::vector<char> aliasGuest(0x600, '\0');
        std::vector<char> candidates(3u * 0x600u, '\0');
        std::vector<char> aliasStorage(sizeof(Resolution), '\0');
        Resolution& aliasResolution =
            *reinterpret_cast<Resolution*>(aliasStorage.data());
        if (rewriteVehicleBodyAlias(guestPath, aliasGuest.data(),
                                    aliasGuest.size())) {
            if (resolvePath(aliasGuest.data(), false, aliasResolution)) {
                int count = 0;
                if (aliasResolution.primary[0] != '\0') {
                    std::snprintf(candidates.data() + count * 0x600, 0x600,
                                  "%s", aliasResolution.primary);
                    ++count;
                }
                if (aliasResolution.fallback[0] != '\0') {
                    std::snprintf(candidates.data() + count * 0x600, 0x600,
                                  "%s", aliasResolution.fallback);
                    ++count;
                }
                if (count > 0 &&
                    std::strncmp(aliasResolution.primary, "sdmc:", 5) != 0 &&
                    appLocalPath(aliasResolution.primary,
                                 candidates.data() + count * 0x600, 0x600)) {
                    ++count;
                }
                for (int i = 0; i < count; ++i) {
                    ++attempts;
                    result = callback(candidates.data() + i * 0x600);
                    lastErrno = errno;
                    if (result == 0) {
                        if (servedOut != nullptr) {
                            *servedOut = candidates.data() + i * 0x600;
                        }
                        char diag[640];
                        std::snprintf(diag, sizeof(diag),
                                      "alias=%s guest=%.200s served=%.200s",
                                      aliasGuest.data(), guestPath,
                                      candidates.data() + i * 0x600);
                        appendGuestBootLine(
                            "sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "vehicle-body-alias", 0, 0, diag);
                        if (assetList) {
                            logAssetListRequest(guestPath, resolution, false, 0,
                                                lastErrno);
                        }
                        return 0;
                    }
                }
            }
        }
    }
    if (result != 0 && errno == 0) {
        errno = firstError;
    }
    if (assetList) {
        logAssetListRequest(guestPath, resolution, false, result, errno);
    }
    return result;
}

bool rejectReadOnlyWrite(const Resolution& resolution, bool writing) {
    if (writing && resolution.readOnly) {
        errno = kGuestErrnoEROFS;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// BULK READ for large files.
//
// WHY IT EXISTS.
//
// libnx's fsdev read path asks the FS service for the WHOLE requested size
// in one request. The service refuses a size it will not transfer and
// answers 0xD401, and libnx then falls back to reading the file in
// 4096-byte pieces -- one synchronous service round trip each. That fallback
// is visible in the NRO: fsdev_read compares the service's answer with
// 0xD401 and, on a match, calls fsdev_read_safe, whose loop is
//     cmp x20, #0x1, lsl #12        ; 4096
//     csel x26, x20, x25, ls
//     bl   fsFileRead
// i.e. one fsFileRead IPC per 4 KiB.
//
// The guest's read of files/.depot/asset_list_base.txtCache.txt -- 4,212,795
// bytes -- is therefore ~1029 service round trips, and the port's main
// thread sits in all of them.
//
// MEASURED, archive 20261021-205414-eden-stab3 (the stalled run):
//   * the last thing the port logged was the open of that file
//     (fopen-other size=4212795 served=rr3core:/files/.depot/...)
//   * the port's own heartbeat (guest-boot.progress) stopped at t=25.56 s
//     and the emulator's log stopped at t=26.04 s, while the run went on
//     to 153 s at 0.6 CPU/s
//   * census-ord ordinal=0: the root thread's last import was fread
//   * census-ord ordinal=4: a guest thread stuck in pthread_cond_wait for
//     12.26 s, holding a guest mutex (census-mutex-held held_ms=12259)
//   * census-wait: signalled=0 deficit=1 -- the condvar it waits on is never
//     signalled, because the thread that has to signal it is the one stuck
//     in the read
// So the read is what the guest is waiting on, and the wait never ends.
//
// WHAT THIS DOES. The port does the read ITSELF, in pieces of a size the
// service serves in ONE round trip, and hands the guest a memory-backed
// stream. The guest's own freads then cost nothing at all, and the port's
// main thread is out of the service loop.
//
// THE PIECE SIZE IS ADAPTED, NOT GUESSED. The service's maximum is not
// documented in the vendored headers, so the read measures itself: each
// piece is timed, and the next piece is doubled when the last one came
// back in less than half the time (a request served in one round trip
// costs about the same whatever its size) and halved when it took more
// than twice as long (a request the service refuses costs proportionally
// more, because libnx chunks it internally). The starting size is the one
// rr3::md5File() already uses against the same volumes.
//
// The result is logged, so the next run knows what the service really
// served and what one piece cost.
// ---------------------------------------------------------------------------

// Below this a file is not worth a bulk read: the service overhead of one
// request dominates and the ordinary fopen path is already cheap.
constexpr unsigned long long kBulkReadMinBytes = 256ull * 1024ull;
// Above this the buffer is not worth holding.
constexpr unsigned long long kBulkReadMaxFileBytes = 8ull * 1024ull * 1024ull;
// The total the bulk path is allowed to hold at once.
//
// MEASURED 2026-10-31: 32 MB was too small, and the failure it caused was
// silent and remote.  The guest asks for the 4.2 MB base asset list TWICE --
// once from the ROMFS volume and once from the card's rewritten copy -- and
// the second ask is served from the in-memory ring.  Once the run had
// bulk-read more than ~28 MB, the budget was spent and the second ask took
// the ORDINARY fopen path instead: the guest then read 4.2 MB through the
// port's per-call interception, which is thousands of small reads over the
// emulated SD card, and the run looked FROZEN (no log line for 120 s, the
// CPU still climbing) while it was in fact crawling.
//
// So the budget is raised, the ring gets more slots so a file read early is
// still there when the guest asks for it again, and -- the part that
// actually cost two rounds -- THE REJECTION IS NOW LOGGED.  A cap that
// fails silently is a cap that cannot be diagnosed.
constexpr unsigned long long kBulkReadMaxTotalBytes = 128ull * 1024ull * 1024ull;
// The starting piece size, and the largest one tried.
constexpr size_t kBulkReadStartChunk = 64 * 1024;
constexpr size_t kBulkReadMaxChunk = 1024 * 1024;

// The bulk-read cache.
//
// The entries are NEVER freed. A guest FILE* handed out earlier points into
// the buffer, and the port has no way to know when the guest closed it, so
// freeing on eviction would be a use-after-free waiting to happen. The
// table is small, the per-file size is capped, and the total is capped;
// past either cap the bulk path is simply not used and the ordinary fopen
// path answers.
struct BulkReadEntry {
    char path[0x600]{};
    void* data{};
    size_t size{};
};
constexpr unsigned kBulkReadSlots = 8;
BulkReadEntry gBulkReads[kBulkReadSlots]{};
unsigned gBulkReadNext = 0;
unsigned long long gBulkReadTotalBytes = 0;
unsigned gBulkReadCount = 0;

unsigned long long systemTickMillis(uint64_t ticks) {
    const uint64_t frequency = armGetSystemTickFreq();
    if (frequency == 0) {
        return 0;
    }
    return (ticks * 1000ull) / frequency;
}

// The ".ast" stream probe, defined below. The bulk path reports the digest
// of the buffer it hands the guest through it, so the memory-backed streams
// are measured by the same line as the ordinary ones -- and the control on
// the memory-backed path is measured by exactly the code path that could
// hand out a stale buffer.
void emitAstStreamLine(const char* guestPath, const char* servedPath, const char* mode,
                        unsigned long long fileSizeAtOpen, unsigned long long bytesHanded,
                        const char* handedDigest);

// Put a buffer into the bulk-read cache under a path.
//
// WHY THIS EXISTS.  The guest asks for the 4.2 MB base asset list TWICE:
// once from the ROMFS volume and once from the card's rewritten copy -- and
// the second ask is the one that kills the round.
//
// MEASURED 2026-10-31 (q43, q44, q49): the run stalls with no log line for
// 120 s and the CPU still climbing.  The last thing the port did was
// "fopen-other size=4212795 served=sdmc:/switch/rr3-native-rehost/cache/
// asset_list_base.txtCache.txt", and Eden's last line is a
// GetFileTimeStampRaw on that very file: the port was serving the card's
// copy through the ORDINARY path, so the guest's reads went one small piece
// at a time through the port's per-call interception, over the emulated SD
// card.  Whether the ring happened to hold that path was LUCK: in q42 it did
// (a "bulk-cache-hit" line) and the round was fine; in q43, q44 and q49 it
// did not, and the round died.
//
// So the card's copy is REGISTERED HERE, at the moment the port writes it,
// from the very bytes it wrote.  The guest's second ask then hits the ring
// every time, because the port put it there on purpose.
//
// The buffer is never freed: a guest FILE* handed out later points into it,
// and the port cannot know when the guest closed that stream.
void registerBulkRead(const char* path, const char* data, size_t size) {
    if (path == nullptr || path[0] == '\0' || size == 0) {
        return;
    }
    void* buffer = std::malloc(size);
    if (buffer == nullptr) {
        return;
    }
    std::memcpy(buffer, data, size);
    BulkReadEntry& entry = gBulkReads[gBulkReadNext];
    gBulkReadNext = (gBulkReadNext + 1) % kBulkReadSlots;
    std::snprintf(entry.path, sizeof(entry.path), "%s", path);
    entry.data = buffer;
    entry.size = size;
    gBulkReadTotalBytes += size;
    ++gBulkReadCount;
    char details[768];
    std::snprintf(details, sizeof(details),
                  "path=%s size=%zu registered=1 slot=%u reads=%u held=%llu",
                  path, size, (gBulkReadNext + kBulkReadSlots - 1) % kBulkReadSlots,
                  gBulkReadCount, gBulkReadTotalBytes);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "bulk-registered", 0, 0, details);
}

// Open a large file into memory and hand back a memory-backed stream.
//
// Returns false when the file is not bulk-worthy, or when the bulk path is
// out of budget; the caller then falls back to the ordinary fopen.
bool bulkOpenFile(const char* candidate, const char* mode, std::FILE** out) {
    if (candidate == nullptr || out == nullptr || mode == nullptr) {
        return false;
    }
    if (std::strchr(mode, 'w') != nullptr || std::strchr(mode, 'a') != nullptr) {
        return false;
    }
    // Already read? Then reuse the bytes rather than paying for them again.
    for (unsigned index = 0; index < kBulkReadSlots; ++index) {
        const BulkReadEntry& entry = gBulkReads[index];
        if (entry.data != nullptr && std::strcmp(entry.path, candidate) == 0) {
            std::FILE* stream = ::fmemopen(entry.data, entry.size, "rb");
            if (stream == nullptr) {
                return false;
            }
            // THE CACHED BUFFER'S DIGEST, and WHEN IT WAS CACHED.
            //
            // A cache hit hands the guest bytes that were read EARLIER. If the
            // file changed since, the guest gets the old bytes and hashes
            // them -- which is exactly the shape of a "didn't match its MD5"
            // that no amount of correct serving can clear. So the digest and
            // the age of the cached buffer are on the line.
            char cachedDigest[33]{};
            {
                Md5 md5;
                md5.begin();
                if (entry.size > 0) {
                    md5.update(entry.data, entry.size);
                }
                md5.finish(cachedDigest);
            }
            char details[768];
            std::snprintf(details, sizeof(details),
                          "path=%s size=%zu cached_md5=%.32s",
                          candidate, entry.size, cachedDigest);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "bulk-cache-hit", index, 0, details);
            // THE SAME MEASUREMENT THE ORDINARY PATH MAKES. A cache hit
            // hands the guest bytes that were read EARLIER, and the fresh
            // disk read is what says whether those are still the file's
            // bytes. The digest below is of the buffer the guest is about
            // to read, so this line is a fact about the guest's bytes and
            // not about the file.
            emitAstStreamLine(nullptr, candidate, mode, entry.size, entry.size, cachedDigest);
            *out = stream;
            return true;
        }
    }
    const int fd = ::open(candidate, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        static_cast<unsigned long long>(info.st_size) < kBulkReadMinBytes ||
        static_cast<unsigned long long>(info.st_size) > kBulkReadMaxFileBytes) {
        ::close(fd);
        return false;
    }
    const size_t total = static_cast<size_t>(info.st_size);
    if (gBulkReadTotalBytes + total > kBulkReadMaxTotalBytes) {
        ::close(fd);
        // THE REJECTION IS LOGGED.  It used to be silent, and the run it
        // caused looked like a freeze 120 s later, with no way back to this
        // line.  A cap that fails silently cannot be diagnosed.
        char diag[768];
        std::snprintf(diag, sizeof(diag),
                      "path=%s size=%zu held=%llu budget=%llu slots=%u "
                      "reads=%u why=budget-exhausted -> the ordinary path "
                      "will answer",
                      candidate, total, gBulkReadTotalBytes,
                      kBulkReadMaxTotalBytes, kBulkReadSlots, gBulkReadCount);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "bulk-rejected", 0, 0, diag);
        return false;
    }
    void* buffer = std::malloc(total);
    if (buffer == nullptr) {
        ::close(fd);
        return false;
    }
    // THE READ.
    size_t done = 0;
    size_t chunk = kBulkReadStartChunk;
    unsigned long long previousMillis = 0;
    unsigned pieces = 0;
    const uint64_t started = armGetSystemTick();
    while (done < total) {
        const size_t want = chunk < total - done ? chunk : total - done;
        const uint64_t before = armGetSystemTick();
        const ssize_t got = ::read(fd, static_cast<char*>(buffer) + done, want);
        const unsigned long long millis = systemTickMillis(armGetSystemTick() - before);
        if (got <= 0) {
            ::close(fd);
            std::free(buffer);
            return false;
        }
        done += static_cast<size_t>(got);
        ++pieces;
        // ADAPT THE PIECE SIZE. A request the service serves in one round
        // trip costs about the same whatever its size, so a piece that came
        // back in less than half the time means the service can take more
        // than that in one go. A request it refuses costs proportionally
        // more -- libnx chunks it into 4 KiB pieces -- so a piece that took
        // more than twice as long means the size is too big.
        if (previousMillis != 0) {
            if (millis * 2 < previousMillis && chunk < kBulkReadMaxChunk) {
                chunk *= 2;
            }
            else if (millis > previousMillis * 2 && chunk > 4096) {
                chunk /= 2;
            }
        }
        previousMillis = millis;
    }
    const unsigned long long totalMillis = systemTickMillis(armGetSystemTick() - started);
    ::close(fd);
    std::FILE* stream = ::fmemopen(buffer, total, "rb");
    if (stream == nullptr) {
        std::free(buffer);
        return false;
    }
    // THE DIGEST OF THE BYTES THE GUEST WILL ACTUALLY READ.
    //
    // The "fopen-asset" line reports the SIZE of the file the resolver
    // picked. That is not what the guest receives: this buffer is. A guest
    // that hashes the stream it was handed can therefore disagree with the
    // port about the bytes, and the port's own read-back of the same file
    // agrees with nobody. So the digest is computed HERE, on the exact
    // buffer that becomes the guest's stream, and it is logged next to the
    // size the guest will see.
    char servedDigest[33]{};
    {
        Md5 md5;
        md5.begin();
        if (total > 0) {
            md5.update(buffer, total);
        }
        md5.finish(servedDigest);
    }
    // Cache it. The slot may already hold an entry; that entry's buffer is
    // NOT freed -- a guest stream may still point into it.
    BulkReadEntry& entry = gBulkReads[gBulkReadNext];
    gBulkReadNext = (gBulkReadNext + 1) % kBulkReadSlots;
    std::snprintf(entry.path, sizeof(entry.path), "%s", candidate);
    entry.data = buffer;
    entry.size = total;
    gBulkReadTotalBytes += total;
    ++gBulkReadCount;
    // The first few, and then every sixteenth, so the line is there without
    // turning into a log of its own.
    if (gBulkReadCount <= 4 || (gBulkReadCount % 16) == 0) {
        char details[768];
        std::snprintf(details, sizeof(details),
                      "path=%s size=%zu pieces=%u piece_kb=%zu ms=%llu "
                      "bytes_per_ms=%llu served_md5=%.32s",
                      candidate, total, pieces, chunk / 1024, totalMillis,
                      totalMillis != 0 ? total / totalMillis : total,
                      servedDigest);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "bulk-read", gBulkReadCount, 0, details);
    }
    // The digest of the buffer the guest is about to read, measured by the
    // same line the ordinary path uses.
    emitAstStreamLine(nullptr, candidate, mode, total, total, servedDigest);
    *out = stream;
    return true;
}

// ---------------------------------------------------------------------------
// THE ".ast" STREAM PROBE -- the missing sample.
//
// WHY IT EXISTS.
//
// The guest's own md5 check compares the digest of the file it just wrote
// (the ".ast") against the digest the manifest publishes for that asset.
// The port had three samples of "the same file", and NOT ONE of them was
// the bytes the guest hashed:
//
//   * the endpoint hashes the bytes it SERVED (a copy still in memory);
//   * the "ast-readback" instrument re-reads the file from disk after the
//     write;
//   * the "served-digest" instrument re-reads it again, later.
//
// So when the guest rejects an asset that every port-side instrument calls
// perfect, none of those three can explain it: they were never looking at
// the bytes the guest hashed. That is the hole this probe fills.
//
// WHAT IT REPORTS, per stream:
//
//   file_size_at_open          the size of the file at the moment of the open
//   bytes_handed_to_guest      how many bytes the guest can read from it
//   digest_of_handed_bytes     the digest of EXACTLY those bytes
//   digest_of_fresh_disk_read  the digest of an INDEPENDENT second read
//   agree                      1 when the two samples are the same
//
// For the ordinary path the port serves the guest FROM THE PROBE'S OWN
// BUFFER, so digest_of_handed_bytes is a fact and not a claim. The second
// sample goes through a different syscall path (raw read(2), not stdio) and
// its own buffer, so the two are genuinely independent.
//
// THE CONTROL CAN FAIL. The control streams carry expect_md5 -- the digest
// the port AND the guest both report for them -- and expect_match is the
// comparison of the measured digest against that constant. A control that
// cannot fail is not a control.
// ---------------------------------------------------------------------------

constexpr char kAstStreamLogPath[] = "sdmc:/switch/rr3-native-rehost.guest-view.log";

// Below this the ordinary path answers, so the probe takes it over and the
// digest it reports is the digest of the bytes the guest really gets. It is
// the same boundary the bulk path uses, so the two never both claim a file.
constexpr unsigned long long kAstStreamMaxBytes = 256ull * 1024ull;
// The total the probe may hold at once. The buffers are NEVER freed -- a
// guest stream may still point into one -- so this budget is the only thing
// that bounds the probe's memory.
constexpr unsigned long long kAstStreamBudgetBytes = 24ull * 1024ull * 1024ull;

unsigned long long gAstStreamHeldBytes = 0;
unsigned gAstStreamCount = 0;

// THE CONTROLS.
//
// Every one of these is an asset the guest accepts, and every one carries
// the digest the port AND the guest independently report.
//
//   bulk         the memory-backed path, where a stale buffer could have
//                been handed out -- and the file the port's own endpoint
//                logs as 4e063961c02bc8487524d415556b5095
//   plain        the ordinary path, the same shape as the suspect
//   suspect-*    the suspect itself, in BOTH of the spellings it is served
//                under: the asset file and the ".ast" the guest hashes
struct AstStreamControl {
    const char* label;
    const char* servedSuffix;
    const char* expectMd5;
};
constexpr AstStreamControl kAstStreamControls[] = {
    {"bulk", "/gui_assets/960x640/common/sprites_0.rgb.pvr.z",
     "4e063961c02bc8487524d415556b5095"},
    {"plain", "/assets_480x320/common/sprites.android.atlas",
     "7f546f712bf2681e4269643d999226d4"},
    {"suspect-asset", "/assets_960x640/common/sprites.android.atlas",
     "c72568828875f4e2a6b11f5c944e27e0"},
    {"suspect-ast", "/gui_assets/960x640/common/sprites.android.atlas",
     "c72568828875f4e2a6b11f5c944e27e0"},
};

// The control whose served path ends with this suffix, or with the suffix
// followed by ".ast" -- the guest writes the ".ast" for the same asset, and
// it holds the same bytes.
const AstStreamControl* astStreamControlFor(const char* servedPath) {
    if (servedPath == nullptr) {
        return nullptr;
    }
    const size_t pathLength = std::strlen(servedPath);
    for (const AstStreamControl& control : kAstStreamControls) {
        const size_t suffixLength = std::strlen(control.servedSuffix);
        if (pathLength < suffixLength) {
            continue;
        }
        const char* tail = servedPath + pathLength - suffixLength;
        if (std::strcmp(tail, control.servedSuffix) == 0) {
            return &control;
        }
        if (pathLength >= suffixLength + 4 &&
            std::strcmp(servedPath + pathLength - 4, ".ast") == 0 &&
            std::strcmp(servedPath + pathLength - suffixLength - 4, control.servedSuffix) == 0) {
            return &control;
        }
    }
    return nullptr;
}

// Is this a stream the probe reports on?
bool astStreamIsTracked(const char* servedPath) {
    if (servedPath == nullptr) {
        return false;
    }
    if (astStreamControlFor(servedPath) != nullptr) {
        return true;
    }
    const size_t length = std::strlen(servedPath);
    return length >= 4 && std::strcmp(servedPath + length - 4, ".ast") == 0;
}

// THE OTHER REPRESENTATION.
//
// The guest's compare site reverses the bytes within each 4-byte word of the
// digest it computed (0x1cdcb10-0x1cdcb8c) and compares the result with the
// 16 bytes it parsed out of the manifest text. So the same 16 bytes have two
// possible forms, and a disagreement between the forms would look like a
// corrupt download instead of a byte-order disagreement. Both are printed
// side by side so the difference shows up as a difference.
//
// This takes the 32 hex characters and swaps the four byte-pairs inside each
// 4-byte word: "c7256882" becomes "826825c7".
void astStreamWordReversedHex(const char* hex32, char out[33]) {
    if (hex32 == nullptr || out == nullptr) {
        return;
    }
    for (unsigned word = 0; word < 4u; ++word) {
        for (unsigned byte = 0; byte < 4u; ++byte) {
            const unsigned source = (word * 4u + (3u - byte)) * 2u;
            out[word * 8u + byte * 2u] = hex32[source];
            out[word * 8u + byte * 2u + 1u] = hex32[source + 1u];
        }
    }
    out[32] = '\0';
}

// AN INDEPENDENT SECOND READ.
//
// A different syscall path from the probe's own read (raw read(2), not
// stdio), its own buffer, its own digest. It is the only sample in the port
// that is taken at the moment the guest's stream is opened and that shares
// no buffer with the sample it is compared against.
//
// The buffer is file-scope, not stack: this runs on the guest's thread, and
// 64 KiB of stack there has killed runs before. Two threads may hash at
// once, and the flag makes the second one report "not measured" instead of a
// torn digest.
bool astStreamFreshDiskDigest(const char* path, char out[33], unsigned long long* sizeOut) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    out[0] = '\0';
    static unsigned char buffer[8 * 1024];
    static volatile bool busy = false;
    if (busy) {
        return false;
    }
    busy = true;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        busy = false;
        return false;
    }
    Md5 digest;
    digest.begin();
    unsigned long long total = 0;
    bool failed = false;
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            failed = true;
            break;
        }
        if (got == 0) {
            break;
        }
        digest.update(buffer, static_cast<size_t>(got));
        total += static_cast<unsigned long long>(got);
    }
    ::close(fd);
    busy = false;
    if (failed) {
        return false;
    }
    digest.finish(out);
    if (sizeOut != nullptr) {
        *sizeOut = total;
    }
    return true;
}

// THE LINE.
void emitAstStreamLine(const char* guestPath, const char* servedPath, const char* mode,
                        unsigned long long fileSizeAtOpen, unsigned long long bytesHanded,
                        const char* handedDigest) {
    if (servedPath == nullptr || handedDigest == nullptr || !astStreamIsTracked(servedPath)) {
        return;
    }
    const AstStreamControl* control = astStreamControlFor(servedPath);

    char diskDigest[33]{};
    unsigned long long diskBytes = 0;
    const bool haveDisk = astStreamFreshDiskDigest(servedPath, diskDigest, &diskBytes);
    // agree = 1 only when BOTH samples are the same and the sizes agree.
    // "na" means the second sample could not be taken, which is NOT a
    // disagreement -- and saying so is the whole point of the field.
    const char* agree = "na";
    if (haveDisk) {
        agree = (diskBytes == bytesHanded && std::strcmp(diskDigest, handedDigest) == 0) ? "1" : "0";
    }

    char reversed[33]{};
    astStreamWordReversedHex(handedDigest, reversed);

    char details[1536];
    int written = std::snprintf(details, sizeof(details),
                                 "path=%.300s served=%.300s mode=%s "
                                 "file_size_at_open=%llu bytes_handed_to_guest=%llu "
                                 "digest_of_handed_bytes=%s digest_of_fresh_disk_read=%s "
                                 "agree=%s md5_hex=%s md5_wordrev_hex=%s",
                                 guestPath != nullptr ? guestPath : "-",
                                 servedPath,
                                 mode != nullptr ? mode : "-",
                                 fileSizeAtOpen,
                                 bytesHanded,
                                 handedDigest,
                                 haveDisk ? diskDigest : "-",
                                 agree,
                                 handedDigest,
                                 reversed);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(details)) {
        written = 0;
    }
    if (control != nullptr) {
        const int expect = asset::digestEquals(handedDigest, control->expectMd5) ? 1 : 0;
        const int extra = std::snprintf(details + written, sizeof(details) - written,
                                        " control=%s expect_md5=%s expect_match=%d",
                                        control->label, control->expectMd5, expect);
        if (extra > 0 && static_cast<size_t>(written + extra) < sizeof(details)) {
            written += extra;
        }
    }
    appendGuestBootLine(kAstStreamLogPath, "ast-stream", ++gAstStreamCount, 0, details);
}

// THE ORDINARY-PATH PROBE.
//
// The port's ordinary read path hands the guest a std::FILE* on the real
// file. Nothing in the port sees the bytes the guest then reads from it,
// and the port's own read-back instruments read the file a SECOND time --
// so a port that says "the file hashes to X" is not, by itself, evidence
// that the guest hashed X.
//
// THIS MAKES IT EVIDENCE: the probe reads the file once, keeps the bytes,
// and serves the guest FROM THOSE BYTES. The digest it reports is the
// digest of the bytes the guest will read, because those are the bytes it
// gets.
//
// The cost is memory: the buffer is never freed, because a guest stream may
// still point into it. The budget is what bounds that, and the ordinary
// path is left completely unchanged for every file the probe declines.
bool probeAstStream(const char* guestPath, const char* servedPath, const char* mode,
                    std::FILE** out) {
    if (guestPath == nullptr || servedPath == nullptr || mode == nullptr || out == nullptr) {
        return false;
    }
    // A read-only open only. A write or an update needs a real fd.
    if (std::strchr(mode, 'w') != nullptr || std::strchr(mode, 'a') != nullptr ||
        std::strchr(mode, '+') != nullptr) {
        return false;
    }
    if (!astStreamIsTracked(servedPath)) {
        return false;
    }
    const int fd = ::open(servedPath, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return false;
    }
    const unsigned long long fileSize = static_cast<unsigned long long>(info.st_size);
    // Zero-length and bulk-sized files are left to the paths that already
    // handle them: a zero-length file has no digest to compare, and a
    // bulk-sized one is the bulk path's, with its own digest instrument.
    if (fileSize == 0 || fileSize >= kAstStreamMaxBytes) {
        ::close(fd);
        return false;
    }
    if (gAstStreamHeldBytes + fileSize > kAstStreamBudgetBytes) {
        ::close(fd);
        return false;
    }
    unsigned char* buffer = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(fileSize)));
    if (buffer == nullptr) {
        ::close(fd);
        return false;
    }
    size_t done = 0;
    bool failed = false;
    while (done < fileSize) {
        const ssize_t got = ::read(fd, buffer + done, static_cast<size_t>(fileSize - done));
        if (got <= 0) {
            failed = true;
            break;
        }
        done += static_cast<size_t>(got);
    }
    ::close(fd);
    if (failed || done != fileSize) {
        std::free(buffer);
        return false;
    }

    // THE DIGEST OF THE BYTES THE GUEST IS ABOUT TO GET.
    char handedDigest[33]{};
    {
        Md5 digest;
        digest.begin();
        digest.update(buffer, done);
        digest.finish(handedDigest);
    }

    std::FILE* stream = ::fmemopen(buffer, done, "rb");
    if (stream == nullptr) {
        std::free(buffer);
        return false;
    }
    // The buffer is NEVER freed: the guest holds the stream.
    gAstStreamHeldBytes += done;
    emitAstStreamLine(guestPath, servedPath, mode, fileSize, done, handedDigest);
    *out = stream;
    return true;
}

// --- one-shot startup evidence for the apk/res -> .depot fallback -----------
// Walks the real files/apk/res/ tree once, at mount time, and for every leaf
// asks the REAL resolver (resolvePath -> primary, then fallback) whether the
// guest form apk/res/<leaf> now resolves, and separately stats the legacy
// files/.depot/<leaf> target to recover what the old single-candidate rule
// could reach. before/after/delta in one line IS the acceptance evidence: if
// mapAppRootToContent ever stops populating resolution.fallback, via_fallback
// and delta both fall to 0 and before == after.
//
// Bounded by kApkResProbeCap leaves and kApkResProbeMaxDepth levels, runs at
// most once per process, emits exactly one line, and never runs per request.
// Raw ::opendir/::stat only: it must not touch the guest-visible shims
// (g_lastOpendirPath, gui injection) and adds no lock and no fsync.
constexpr unsigned kApkResProbeCap = 2048;
constexpr unsigned kApkResProbeMaxDepth = 8;

struct ApkResProbe {
    unsigned probed{};
    unsigned before{};
    unsigned after{};
    unsigned viaFallback{};
    unsigned unresolved{};
    unsigned capped{};
};

void probeApkResDir(const char* deviceRel, const char* guestRel, unsigned depth,
                    ApkResProbe& out) {
    if (depth > kApkResProbeMaxDepth) return;
    char deviceDir[0x400]{};
    std::snprintf(deviceDir, sizeof(deviceDir), "rr3core:/%s", deviceRel);
    DIR* dir = ::opendir(deviceDir);
    if (dir == nullptr) return;
    while (struct dirent* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (out.probed >= kApkResProbeCap) {
            out.capped = 1;
            break;
        }
        char childDevice[0x400]{};
        char childGuest[0x400]{};
        std::snprintf(childDevice, sizeof(childDevice), "%s/%s", deviceRel,
                      entry->d_name);
        std::snprintf(childGuest, sizeof(childGuest), "%s/%s", guestRel,
                      entry->d_name);
        struct stat info{};
        if (::stat(childDevice, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            probeApkResDir(childDevice, childGuest, depth + 1, out);
            continue;
        }
        if (!S_ISREG(info.st_mode)) continue;

        ++out.probed;
        // Old rule: apk/res/<leaf> resolved only if files/.depot/<leaf> exists.
        char depotProbe[0x400]{};
        struct stat depotInfo{};
        std::snprintf(depotProbe, sizeof(depotProbe), "rr3core:/files/.depot/%s",
                      childGuest + 8);  // strip the leading "apk/res/"
        if (::stat(depotProbe, &depotInfo) == 0) ++out.before;

        // New rule, measured through the real resolver.
        char guestPath[0x500]{};
        std::snprintf(guestPath, sizeof(guestPath), "%s/%s", kAppRoot, childGuest);
        Resolution value{};
        bool resolved = false;
        bool fellBack = false;
        if (resolvePath(guestPath, false, value)) {
            struct stat primaryInfo{};
            if (::stat(value.primary, &primaryInfo) == 0) {
                resolved = true;
            }
            else if (value.fallback[0] != '\0' &&
                     ::stat(value.fallback, &primaryInfo) == 0) {
                resolved = true;
                fellBack = true;
            }
        }
        if (resolved) ++out.after;
        else ++out.unresolved;
        if (fellBack) ++out.viaFallback;
    }
    ::closedir(dir);
}

void logApkResReachabilityOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    ApkResProbe out{};
    probeApkResDir("files/apk/res", "apk/res", 0, out);
    char line[0x300]{};
    std::snprintf(line, sizeof(line),
                  "volume=rr3core probed=%u before=%u after=%u delta=%+d "
                  "via_fallback=%u unresolved=%u capped=%u",
                  out.probed, out.before, out.after,
                  static_cast<int>(out.after) - static_cast<int>(out.before),
                  out.viaFallback, out.unresolved, out.capped);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "apk-res-reach", 0, 0, line);
}

// ---------------------------------------------------------------------------
// THE GUI-ASSET REQUEST CENSUS.
//
// WHY: a texture leaf is named by TWO things -- the resolution segment and
// the leaf format -- and the port can answer a request only when the volume
// ships THAT file in THAT tree. The defect this census was built for is a
// MISMATCH between the two, so they are counted together: "1280x720/dxt.dds,
// 24 requests, 0 hits" says something a count of failures alone does not.
//
// WHAT IT COUNTS: every stat and every open the guest makes for a path
// under "gui_assets/", keyed by (resolution segment, leaf format), split
// into requests / hits / misses. The stat is the guest's EXISTENCE PROBE and
// the open is what the guest actually CONSUMED, so a key with hits and no
// opens is a file the guest found and could not read.
//
// IT IS DELIBERATELY NOT A "stat-path" LINE. stat-path logs FAILURES, and
// budgets successes out after 600 calls -- which is why the baseline run
// (archive 20261023-175553-eden-vptr) could report "108 requests under
// 1280x720, 0 succeeded" while the same run went on to READ 471,920 bytes
// of rr3gui1:/files/.depot/gui_assets/960x640/common/sprites_0.rgb.pvr.z.
// An instrument that only sees failures cannot tell "the port served
// nothing" from "the instrument cannot see a success". This one sees both.
//
// THE POSITIVE CONTROL IS BELOW, AND IT IS NOT OPTIONAL.
// ---------------------------------------------------------------------------
constexpr unsigned kGuiCensusSlots = 48;
constexpr unsigned kGuiCensusSummaryEvery = 50;

struct GuiCensusEntry {
    char res[16];
    char fmt[16];
    unsigned statReq;
    unsigned statHit;
    unsigned statMiss;
    unsigned openReq;
    unsigned openHit;
    unsigned openMiss;
    bool firstLogged;
};

GuiCensusEntry gGuiCensus[kGuiCensusSlots];
unsigned gGuiCensusKeys = 0;
unsigned gGuiCensusRequests = 0;
unsigned gGuiCensusSummaries = 0;
bool gGuiCensusBusy = false;
bool gGuiCensusSelfTestDone = false;

// THE RESOLUTION SEGMENT AND THE LEAF FORMAT OF A GUI-ASSET REQUEST.
//
// The resolution segment is the directory right after "gui_assets/", and the
// format is what the leaf's extension says the PIXELS are in -- ".z" is the
// compression, not the format, so ".dxt.dds.z" and ".dxt.dds" are one format.
bool guiAssetKey(const char* guestPath, char* res, size_t resSize,
                 char* fmt, size_t fmtSize) {
    if (guestPath == nullptr || res == nullptr || fmt == nullptr) return false;
    const char* marker = std::strstr(guestPath, "gui_assets/");
    if (marker == nullptr) return false;
    const char* after = marker + std::strlen("gui_assets/");
    const char* slash = std::strchr(after, '/');
    if (slash == nullptr || slash == after) return false;
    const size_t resLen = static_cast<size_t>(slash - after);
    if (resLen + 1 > resSize) return false;
    std::memcpy(res, after, resLen);
    res[resLen] = '\0';
    const char* leaf = std::strrchr(slash + 1, '/');
    leaf = (leaf != nullptr) ? leaf + 1 : slash + 1;
    char leafCopy[0x200];
    std::snprintf(leafCopy, sizeof(leafCopy), "%s", leaf);
    size_t n = std::strlen(leafCopy);
    if (n > 2 && std::strcmp(leafCopy + n - 2, ".z") == 0) {
        leafCopy[n - 2] = '\0';
    }
    const char* format = "other";
    if (std::strstr(leafCopy, ".rgb.pvr") != nullptr) format = "rgb.pvr";
    else if (std::strstr(leafCopy, ".ptc.pvr") != nullptr) format = "ptc.pvr";
    else if (std::strstr(leafCopy, ".dxt.dds") != nullptr) format = "dxt.dds";
    else if (std::strstr(leafCopy, ".etc.dds") != nullptr) format = "etc.dds";
    else if (std::strstr(leafCopy, ".atc.dds") != nullptr) format = "atc.dds";
    else if (std::strstr(leafCopy, ".android.atlas") != nullptr) format = "atlas";
    else if (std::strstr(leafCopy, ".pvr") != nullptr) format = "pvr";
    else if (std::strstr(leafCopy, ".dds") != nullptr) format = "dds";
    std::snprintf(fmt, fmtSize, "%s", format);
    return true;
}

GuiCensusEntry* guiCensusSlot(const char* res, const char* fmt) {
    for (unsigned i = 0; i < gGuiCensusKeys; ++i) {
        if (std::strcmp(gGuiCensus[i].res, res) == 0 &&
            std::strcmp(gGuiCensus[i].fmt, fmt) == 0) {
            return &gGuiCensus[i];
        }
    }
    if (gGuiCensusKeys >= kGuiCensusSlots) return nullptr;
    GuiCensusEntry* entry = &gGuiCensus[gGuiCensusKeys++];
    std::snprintf(entry->res, sizeof(entry->res), "%s", res);
    std::snprintf(entry->fmt, sizeof(entry->fmt), "%s", fmt);
    entry->firstLogged = false;
    return entry;
}

// THE CENSUS, AS ONE LINE.
//
// "res/fmt:stat req/hit/miss open req/hit/miss" -- the three numbers are
// always in that order, so a reader can split the line without a legend.
void logGuiCensusSummary(const char* why) {
    char details[0x900];
    int n = std::snprintf(details, sizeof(details),
                          "requests=%u keys=%u %s |", gGuiCensusRequests,
                          gGuiCensusKeys, why);
    for (unsigned i = 0; i < gGuiCensusKeys; ++i) {
        if (n < 0 || n >= static_cast<int>(sizeof(details))) break;
        const GuiCensusEntry& e = gGuiCensus[i];
        n += std::snprintf(details + n, sizeof(details) - n,
                           " %s/%s:stat %u/%u/%u open %u/%u/%u",
                           e.res, e.fmt, e.statReq, e.statHit, e.statMiss,
                           e.openReq, e.openHit, e.openMiss);
    }
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(details))) {
        n = static_cast<int>(sizeof(details)) - 1;
    }
    details[n] = '\0';
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "gui-asset-census", ++gGuiCensusSummaries, 0, details);
}

// THE POSITIVE CONTROL.
//
// An instrument that can only report "miss" looks exactly like a perfect
// instrument on a broken port: every request fails, the census says "0 hits",
// and nobody can tell "the port serves nothing" from "the instrument cannot
// see a success". So the census is REQUIRED to classify two probes it makes
// itself, through the SAME resolver and the SAME classifier it uses for the
// guest's requests:
//
//   present  .depot/gui_assets/960x640/common/sprites_0.rgb.pvr.z  -> must hit
//   absent   .depot/gui_assets/1280x720/common/sprites_0.dxt.dds.z -> must miss
//
// Both are MEASURED facts about the deployed volumes (archive
// 20261023-175553-eden-vptr): the first is stat'ed OK (471,920 B, stat-layout
// at L39299), the second is ENOENT (stat-path rc=-1 errno=2 at L7740). The
// line reports ok=1 only when the classifier agrees with the filesystem on
// BOTH, and it prints the path each probe actually resolved to.
void guiAssetCensusSelfTest() {
    if (gGuiCensusSelfTestDone) return;
    gGuiCensusSelfTestDone = true;
    gGuiCensusBusy = true;
    struct Probe {
        const char* guestPath;
        bool expectHit;
        const char* label;
    };
    const Probe probes[] = {
        {"sdmc:/switch/rr3-native-rehost/.depot/gui_assets/960x640/common/sprites_0.rgb.pvr.z",
         true, "present"},
        {"sdmc:/switch/rr3-native-rehost/.depot/gui_assets/1280x720/common/sprites_0.dxt.dds.z",
         false, "absent"},
    };
    int ok = 1;
    char present[640]{};
    char absent[640]{};
    for (const Probe& probe : probes) {
        Resolution resolution{};
        struct stat host{};
        int rc = -1;
        if (resolvePath(probe.guestPath, false, resolution)) {
            rc = withReadFallback(resolution, probe.guestPath,
                [&host](const char* candidate) { return ::stat(candidate, &host); });
        }
        const bool hit = (rc == 0);
        char res[16]{};
        char fmt[16]{};
        const bool keyed = guiAssetKey(probe.guestPath, res, sizeof(res),
                                       fmt, sizeof(fmt));
        const char* classified = hit ? "hit" : "miss";
        const bool good = keyed && (hit == probe.expectHit);
        if (!good) ok = 0;
        char* slot = probe.expectHit ? present : absent;
        std::snprintf(slot, 640, "%s rc=%d errno=%d classified=%s key=%s/%s "
                                   "resolved=%s",
                      probe.label, rc, rc == 0 ? 0 : errno, classified,
                      res, fmt,
                      resolution.primary[0] != '\0' ? resolution.primary : "-");
    }
    gGuiCensusBusy = false;
    char details[0x900];
    std::snprintf(details, sizeof(details),
                  "ok=%d | %s | %s", ok, present, absent);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "gui-asset-selftest", 0, static_cast<unsigned long long>(ok),
                        details);
}

// ONE GUI-ASSET REQUEST.
void censusGuiAssetRequest(const char* guestPath, int rc, bool isOpen) {
    if (gGuiCensusBusy) return;
    char res[16]{};
    char fmt[16]{};
    if (!guiAssetKey(guestPath, res, sizeof(res), fmt, sizeof(fmt))) return;
    if (!gGuiCensusSelfTestDone) guiAssetCensusSelfTest();
    GuiCensusEntry* entry = guiCensusSlot(res, fmt);
    if (entry == nullptr) return;
    ++gGuiCensusRequests;
    if (isOpen) {
        ++entry->openReq;
        if (rc == 0) ++entry->openHit; else ++entry->openMiss;
    } else {
        ++entry->statReq;
        if (rc == 0) ++entry->statHit; else ++entry->statMiss;
    }
    if (!entry->firstLogged) {
        entry->firstLogged = true;
        char details[0x300];
        std::snprintf(details, sizeof(details),
                      "key=%s/%s first_request=%s rc=%d api=%s",
                      entry->res, entry->fmt,
                      guestPath != nullptr ? guestPath : "-", rc,
                      isOpen ? "open" : "stat");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "gui-asset-census-new", 0, 0, details);
    }
    if (gGuiCensusRequests % kGuiCensusSummaryEvery == 0) {
        logGuiCensusSummary("periodic");
    }
}

} // namespace

// ---------------------------------------------------------------------------
// THE SHARED ASSET RESOLVER, AND THE ASSET LIST THAT MATCHES IT.
//
// WHY IT EXISTS.
//
//   The port TERMINATES the Cloudcell asset protocol. It answers
//   assetManDownloadPackHash.php with bytes taken off the card and the
//   volumes, so for every asset the guest downloads the port decides WHICH
//   bytes the guest gets. The guest then MD5s what it received and compares
//   it with an md5 from an asset list; when the two disagree it answers
//
//       CC: ASSET DOWNLOAD ERROR! ... [MSG: didn't match its MD5,
//       corrupt sync?] ... Asset (/MenuBannerData.xml) didn't match its MD5
//
//   and DELETES the .ast it has just written.
//
//   MEASURED 2026-10-21, archive 20261021-234544-eden-fsfix3: the md5 the
//   guest asks for is the one in files/.depot/asset_list_updates.txt (3,854
//   B, 45 entries) -- all 41 requested assets are in that list and all 41
//   wanted md5s match it, 41 of 41. The same wanted md5s are in NEITHER
//   asset_list_base.txtCache.txt (4,212,795 B, 40,862 entries) NOR any of
//   the 1,237 other asset lists on the card and in the volumes: that list
//   does not name /MenuBannerData.xml, /events.dat or /crew.bin at all.
//
//   So the list the guest checks its downloads against is
//   asset_list_updates.txt, and it describes the SERVER's newer revision
//   while the card holds the 260530 build. The port is the authority for
//   the bytes it serves, so the list it hands the guest has to describe
//   those bytes.
//
//   This is NOT a way of making the guest accept wrong content: every md5
//   written is the real md5 of a real local file, and the content is the
//   real content of that file. The inconsistency being fixed is the port
//   serving a manifest from one revision and files from another.
// ---------------------------------------------------------------------------

// The local files that can back a requested asset path, in the order the pack
// endpoint tries them.
//
// THIS LIST IS NOT SPELLED OUT HERE ANY MORE, BECAUSE THERE IS NO SECOND COPY
// OF IT TO SPELL OUT.
//
// It used to be: this file carried a hand-written copy of the endpoint's
// order, and the endpoint carried its own, and the file's own rule said they
// had to "stay in lockstep" -- "two copies of a list in two files can only
// ever agree by accident". They did NOT agree: the endpoint grew rules 5 and
// 6 (the guest's own download cache and the nearest lower resolution) and
// this copy did not, so the endpoint could answer a path from a file this
// resolver never looked at, and the manifest published a digest that did not
// describe the bytes the endpoint sent.
//
// BOTH SIDES NOW CALL THE SAME FUNCTIONS OUT OF THE SAME HEADER:
// native-switch/source/rr3_asset_candidates.hpp.
//
//   rr3::asset::buildCandidates()   the ordered candidate list
//   rr3::asset::selectCandidate()   which candidate is the answer
//
// The digest a manifest publishes is the digest of the candidate the shared
// selection picks, so the bytes the endpoint sends and the digest the guest
// compares against are the same bytes by construction.
// THE CANDIDATE LIST IS THE SHARED ONE.
//
// rr3::asset::buildCandidates() -- native-switch/source/rr3_asset_candidates.hpp
// -- is the ONE list, and the pack endpoint calls the same function
// (cc_http_request.cpp assetCandidates() is now a one-line wrapper around it).
// The rules are:
//
//   1. the guest's OWN depot on the card, under the same relative name:
//      ".depot" + path
//   2. the volume tree with the same relative name: "files/.depot" + path
//   3. the gui_assets tree, for the server's "/assets_<res>/" spelling:
//      "files/.depot/gui_assets/<res>" + rest
//   4. the card's installed tree, for the guest's "/gui_assets/<res>/" spelling:
//      ".depot/assets_<res>" + rest
//   5. the guest's own download cache, "<path>.ast", for BOTH spellings --
//      digest-gated, so a leftover of an older revision is never the answer
//   6. the nearest LOWER resolution (2048x1536 -> 960x640 -> 480x320)
//
// WHY THE REVERSE ALIAS IS IN THE LIST: the guest asks for the asset under
// the "gui_assets/<res>/X" spelling, but the ONLY manifest that names it
// names it under the other one ("/assets_960x640/common/sprites.android.atlas"
// in asset_list_updates.txt) -- the shipped files/apk/res/
// asset_list_gui_assets_core.txt (365,312 B, 3,292 entries) has no entry for
// the bare "gui_assets/<res>/common/sprites.android.atlas" at all. So the
// guest applies the game's own spelling rule and checks the bytes it
// received against the digest published for the other spelling. ONE digest,
// TWO spellings, so the two spellings MUST reach the same file.
//
// WHY THE ALIAS IS IN THE LIST (rule 6): a 2048x1536 request for a weekly
// event asset has no 2048x1536 copy ANYWHERE -- not on the card, not in the
// seven volumes, not in the data packs. The 960x640 and the 480x320 trees do
// have it, and the port's own runtime path space already falls back that way.
// So the endpoint answers it from the lower resolution, and the manifest has
// to publish the digest of the ALIASED file's bytes under the REQUESTED path:
// a digest of a file at the requested resolution that does not exist would
// describe bytes the port never sends.
static void assetCandidates(const char* path, std::vector<asset::Candidate>& out) {
    asset::buildCandidates(path, out);
}

// READS ONE CANDIDATE THROUGH THE PORT'S OWN CHAIN.
//
// The SAME read chain the endpoint uses: resolvePath() for the candidate's
// device path, and withReadFallback() for the volume primary, its alias and
// the app-local card copy derived from the primary. So the bytes hashed here
// are the bytes the endpoint will send.
//
// guestFopen() -- the endpoint's own reader -- is resolvePath() plus
// withReadFallback() plus two additions that do not change the bytes of an
// asset: the stub suppression, which names five configuration files and never
// an asset, and the bulk-open cache, which serves the same content from
// memory.
//
// servedOut[index] receives the path that answered, because the selection can
// land on a candidate that is not the last one read.
bool probeAssetCandidate(const asset::Candidate& candidate, unsigned index,
                         char digest[33], unsigned long long& sizeOut,
                         std::vector<unsigned char>& bytes,
                         std::vector<std::string>& servedOut) {
    Resolution resolution{};
    if (!resolvePath(candidate.path, false, resolution)) {
        return false;
    }
    const int rc = withReadFallback(
        resolution, candidate.path,
        [&bytes, &servedOut, index](const char* path) -> int {
            std::FILE* file = std::fopen(path, "rb");
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            bytes.resize(static_cast<size_t>(size));
            const size_t got = (size > 0)
                ? std::fread(bytes.data(), 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                bytes.clear();
                return -1;
            }
            if (index < servedOut.size()) {
                servedOut[index] = path;
            }
            return 0;
        });
    if (rc != 0) {
        return false;
    }
    Md5 md5;
    md5.begin();
    if (!bytes.empty()) {
        md5.update(bytes.data(), bytes.size());
    }
    md5.finish(digest);
    sizeOut = static_cast<unsigned long long>(bytes.size());
    return true;
}

bool resolveAssetLocalFile(const char* assetPath, char* outPath, size_t outPathSize,
                           char digest[33], unsigned long long* sizeOut) {
    if (assetPath == nullptr || outPath == nullptr || outPathSize == 0 ||
        digest == nullptr) {
        return false;
    }
    outPath[0] = '\0';
    digest[0] = '\0';
    if (sizeOut != nullptr) {
        *sizeOut = 0;
    }
    // THE ONE CANDIDATE LIST: rules 1 to 6, from the shared header, so the
    // manifest side and the endpoint see the same files in the same order --
    // including the guest's own download cache (rule 5) and the nearest lower
    // resolution (rule 6).
    std::vector<asset::Candidate> candidates;
    assetCandidates(assetPath, candidates);
    // THE MANIFEST SIDE OF THE SHARED SELECTION. There is no expected digest
    // here, because the digest is what this function computes: the first
    // candidate that exists is the answer, so the digest written into a list
    // is the digest of the bytes the endpoint sends for the same path.
    //
    // The guest's own download cache ("<path>.ast") is the answer only when
    // NOTHING ELSE backs the path: a leftover of an older revision is not the
    // asset, and publishing its digest would hand the guest bytes that are not
    // the revision it asked for.
    std::vector<std::string> served(candidates.size());
    const asset::Selection selection = asset::selectCandidate(
        candidates, nullptr,
        [&served](const asset::Candidate& candidate, unsigned index, char outDigest[33],
                  unsigned long long& outSize, std::vector<unsigned char>& bytes) -> bool {
            return probeAssetCandidate(candidate, index, outDigest, outSize, bytes,
                                       served);
        });
    if (selection.index < 0) {
        return false;
    }
    std::snprintf(outPath, outPathSize, "%s", served[selection.index].c_str());
    std::memcpy(digest, selection.digest, 33);
    if (sizeOut != nullptr) {
        *sizeOut = selection.size;
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE SHARED OVERRIDE SET: EVERY ATLAS PATH THE LISTS NAME.
//
// WHY IT EXISTS.
//
//   MEASURED 2026-10-25, archive 20261025-222948-eden-f1f: the guest made ZERO
//   filesystem requests for the "button" and "avatars" atlases and 24 for
//   "common"/"events", and it answered "Failed to load image from atlas:
//   '<sprite>'" for every sprite in the two atlases that never opened.
//
//   The cause is neither a missing file nor a broken resolver. It is the
//   DIGEST the lists publish. The guest validates every asset against the md5
//   the asset lists name, and for EVERY "/gui_assets/<res>/**/
//   sprites.android.atlas" path the shipped list publishes EA's own revision
//   while the port serves the data pack's. The guest's check fails, the asset
//   is dropped from its registry, and the file is NEVER OPENED.
//
//   "common" is the ONE folder that works, and it is the one folder that
//   happened to be inside the old override set -- the 69 paths the guest
//   happened to download. The override set was too narrow, and the failures
//   live in everything it did not cover.
//
// ONE SET, BUILT ONCE PER BOOT.
//
//   Every consumer -- the base-list rewrite, the per-category GUI list
//   rewrite, the completed gui-assets manifest, the census, the inventory and
//   the digest audit -- reads THIS set. Five separate builds would cost five
//   times the hashing and could disagree about a digest; one build cannot.
//
// THE DIGEST COMES FROM THE SHARED CANDIDATE RULE.
//
//   resolveAssetLocalFile() is the MANIFEST SIDE of rr3::asset's shared
//   selection: it builds the candidates with rr3::asset::buildCandidates()
//   and picks one with rr3::asset::selectCandidate() -- the very functions
//   the pack endpoint uses, from the very header (rr3_asset_candidates.hpp).
//   The endpoint's side is selectCandidate() with the digest the guest asked
//   for; the manifest's side is selectCandidate() with no expected digest,
//   which takes the FIRST CANDIDATE THAT EXISTS. The endpoint's tie-breaker
//   then finds that very candidate when the guest asks for the digest the
//   manifest published, so the bytes the guest receives are the bytes the
//   digest describes.
//
//   That also keeps the two fallbacks the shared header already handles: the
//   guest's own download cache ("<path>.ast", for BOTH spellings) and the
//   NEAREST LOWER RESOLUTION (a 2048x1536 path answered from the 960x640
//   tree). A path the port can only serve at a lower resolution publishes
//   THAT file's digest, which is correct: the shipped digest names bytes the
//   port never sends.
//
// THE SCOPE: THE ATLASES ONLY.
//
//   The atlases are the proven failure mode. The textures (.pvr.z) mostly
//   match already, and every entry of every list would cost a resolve and a
//   hash: the base list names 40,862 entries and the whole set is 9.394 GB,
//   which is 210-420 SECONDS of boot. The atlas set is 4,289 paths and
//   2.68 MB -- affordable -- and it is the set the guest's failures are in.
//
// THE COST IS LOGGED.
//
//   considered / resolved / bytes / ms, so the next round's log proves it.
// ---------------------------------------------------------------------------

// THE ATLAS LEAF: the sprite atlas file the guest validates.
constexpr const char kAtlasLeaf[] = "sprites.android.atlas";

// TRUE when the path names a sprite atlas in the gui_assets tree.
//
// WHOLE SEGMENT, NOT A SUFFIX: the leaf is "sprites.android.atlas", so the
// path has to END with it AND carry a separator in front of it.
bool isGuiAtlasAssetPath(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const size_t leafLength = sizeof(kAtlasLeaf) - 1u;
    const size_t length = std::strlen(path);
    if (length <= leafLength) {
        return false;
    }
    if (std::strcmp(path + (length - leafLength), kAtlasLeaf) != 0) {
        return false;
    }
    if (path[length - leafLength - 1u] != '/') {
        return false;
    }
    return std::strncmp(path, "/gui_assets/", 12) == 0;
}

// THE PATHS ONE ASSET LIST NAMES.
//
// The list is read through the port's own resolver, so the paths are the ones
// the port serves, and the list is the FIXED TABLE the guest merges, so every
// path the guest can look up is covered. The volumes are NOT walked: the
// lists are the table.
//
// `atlasOnly` selects the sprite-atlas paths; with it off, EVERY path the
// list names is collected, which is what the verbatim-list census needs.
void collectPathsFromList(const char* guestPath, std::vector<std::string>& out,
                          bool atlasOnly, std::string* textOut = nullptr) {
    if (guestPath == nullptr) {
        return;
    }
    Resolution resolution{};
    if (!resolvePath(guestPath, false, resolution)) {
        return;
    }
    std::string text;
    const int rc = withReadFallback(
        resolution, guestPath,
        [&text](const char* candidate) -> int {
            // THE BULK PATH FIRST. The base list is 4,212,795 B, and libnx's
            // ordinary read path asks the FS service for the whole size in one
            // request, is refused, and falls back to 4 KiB pieces -- ~1029
            // service round trips for one file.
            std::FILE* file = nullptr;
            if (!bulkOpenFile(candidate, "rb", &file)) {
                file = std::fopen(candidate, "rb");
            }
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = (size > 0)
                ? std::fread(&text[0], 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                text.clear();
                return -1;
            }
            return 0;
        });
    if (rc != 0) {
        return;
    }
    if (textOut != nullptr) {
        *textOut = text;
    }
    // THE LINES. The path is the FIRST field; the md5 and the size fields that
    // follow it are NOT read here -- the digest comes from the resolver, not
    // from the list. A line without a tab is a header (the base list's first
    // line is a binary header) and names no asset.
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
        size_t tab = text.find('\t', position);
        if (tab == std::string::npos || tab > lineEnd) {
            tab = lineEnd;
        }
        if (tab > position) {
            const std::string path = text.substr(position, tab - position);
            if (!atlasOnly || isGuiAtlasAssetPath(path.c_str())) {
                out.push_back(path);
            }
        }
        position = (end == std::string::npos) ? text.size() : end + 1;
    }
}

// TRUE when the path names a sprite atlas, under EITHER spelling.
//
// The guest's lists say "gui_assets/<res>/..." and the guest asks for
// "assets_<res>/..."; a conflict between two lists is a conflict on the path
// STRING, so both spellings have to be recognised.
bool isAtlasLeafPath(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const size_t leafLength = sizeof(kAtlasLeaf) - 1u;
    const size_t length = std::strlen(path);
    if (length <= leafLength) {
        return false;
    }
    return std::strcmp(path + (length - leafLength), kAtlasLeaf) == 0 &&
           path[length - leafLength - 1u] == '/';
}

// THE ATLAS PATHS THE VERBATIM LISTS NAME.
//
// WHY THIS EXISTS.
//
//   The guest merges the big base list with every per-category list it
//   downloads. A path named by two lists with TWO DIFFERENT digests is
//   DROPPED by the merge, and the asset then never reaches the guest's
//   registry at all.
//
//   The port rewrites four lists (the base list, the per-category GUI list,
//   the completed gui-assets manifest and the update list) and serves the
//   rest VERBATIM out of the volume, with EA's own digests. So an override -
//   which replaces a digest with the port's own - is only safe for a path
//   that NO verbatim list names. Otherwise the rewritten list says the
//   port's digest, the verbatim list says EA's, the merge drops the entry,
//   and the guest then asks for an asset whose digest the port cannot
//   produce.
//
//   MEASURED 2026-10-26, archive 20261026-230107-eden-f1g: 85 paths were
//   dropped that way. The run logged 839 md5 mismatches, the front end never
//   opened, and Eden exited on its own at 559 s. The four rounds before it
//   (f1c, f1d, f1e, f1f) reached the race with 0 md5 mismatches.
//
// The enumeration is over the LIST TABLE, not a volume walk: the lists are
// what the guest merges. Only the atlas paths are collected, because those
// are the only paths the override set can touch.
void collectVerbatimListGuestPaths(std::vector<std::string>& out) {
    // The lists the port rewrites. A path one of these names is safe to
    // override, because the rewrite replaces the digest there too.
    static const char* kRewritten[] = {
        "asset_list_base.txtCache.txt",
        "asset_list_base_gui_assets.txt",
        "asset_list_gui_assets_core.txt",
        "asset_list_updates.txt",
    };
    static const char* kDirs[] = {"files/apk/res", "files/.depot"};
    for (const char* dir : kDirs) {
        Resolution resolution{};
        if (!resolvePath(dir, false, resolution)) {
            continue;
        }
        DIR* handle = ::opendir(resolution.primary);
        if (handle == nullptr && errno == ENOENT && resolution.fallback[0] != '\0') {
            handle = ::opendir(resolution.fallback);
        }
        if (handle == nullptr) {
            continue;
        }
        for (;;) {
            struct dirent* entry = ::readdir(handle);
            if (entry == nullptr) {
                break;
            }
            const char* leaf = entry->d_name;
            if (leaf[0] == '.' || std::strstr(leaf, "asset_list") == nullptr) {
                continue;
            }
            bool rewritten = false;
            for (const char* known : kRewritten) {
                if (std::strcmp(leaf, known) == 0) {
                    rewritten = true;
                    break;
                }
            }
            if (rewritten) {
                continue;
            }
            char guestPath[0x600]{};
            std::snprintf(guestPath, sizeof(guestPath), "%s/%s", dir, leaf);
            out.push_back(guestPath);
        }
        ::closedir(handle);
    }
    // The caller wants the LISTS themselves, not their contents: each one is
    // rewritten from the shared override set, so the lists agree.
}

// ---------------------------------------------------------------------------
// EVERY ASSET LIST THE PORT SERVES, REWRITTEN FROM THE ONE OVERRIDE SET.
//
// WHY THIS EXISTS.
//
//   The guest merges the big base list with the per-category lists it
//   downloads, and it validates every asset against the digest ONE of those
//   lists publishes. The port rewrites four of them (the base list, the
//   per-category GUI list, the completed gui-assets manifest and the update
//   list) so that they publish the digest of the bytes the port serves; the
//   other ~220 it serves VERBATIM, with EA's own digests.
//
//   MEASURED 2026-10-26, archives 20261026-230107-eden-f1g and
//   20261026-233247-eden-f1h: with the base list rewritten and the rest not,
//   the two sides published different digests for one path, the guest's merge
//   dropped the entry, and the guest asked for an asset whose digest the port
//   could not produce: 839 and then 844 "didn't match its MD5" messages, the
//   front end never opened, and Eden exited on its own.
//
//   So EVERY list the port serves is rewritten from the SAME override set.
//   One set, every list, and then no two lists can publish two digests for
//   one path.
//
// WHERE THE REWRITTEN COPIES GO.
//
//   On the card, beside the ones the port already keeps there, with an "rr3-"
//   prefix, so a rewritten copy can never collide with a shipped name or with
//   the other rewritten lists. A card that is wiped loses them with the cache
//   and the next boot rebuilds them.
// ---------------------------------------------------------------------------
constexpr const char* kRewrittenListDir = "sdmc:/switch/rr3-native-rehost/cache/";

void buildRewrittenAssetLists() {
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();
    const std::vector<std::pair<std::string, std::string>>& overrides =
        sharedAssetOverrides();
    const auto byPath = [](const std::pair<std::string, std::string>& entry,
                           const std::string& path) {
        return entry.first < path;
    };

    std::vector<std::string> lists;
    collectVerbatimListGuestPaths(lists);

    unsigned touched = 0;
    unsigned written = 0;
    unsigned entries = 0;
    unsigned atlasEntries = 0;
    unsigned long long bytes = 0;
    for (const std::string& guestPath : lists) {
        std::string text;
        std::vector<std::string> paths;
        collectPathsFromList(guestPath.c_str(), paths, false, &text);
        if (text.empty()) {
            continue;
        }
        // DOES THIS LIST NAME ANYTHING THE OVERRIDE SET COVERS? A list that
        // names nothing the set covers keeps the shipped digest everywhere,
        // so it needs no copy and costs no space.
        bool touches = false;
        for (const std::string& path : paths) {
            auto it = std::lower_bound(overrides.begin(), overrides.end(),
                                       path, byPath);
            if (it != overrides.end() && it->first == path) {
                touches = true;
                break;
            }
        }
        if (!touches) {
            continue;
        }
        ++touched;

        // REWRITE THE MD5 COLUMN, AND ONLY THAT COLUMN. The path text, the
        // order, the count and the field layout are preserved exactly.
        std::string out;
        out.reserve(text.size() + 64u);
        size_t position = 0;
        unsigned changed = 0;
        while (position < text.size()) {
            const size_t end = text.find('\n', position);
            const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
            const std::string line = text.substr(position, lineEnd - position);
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                out.append(line);
            } else {
                const std::string path = line.substr(0, tab);
                const size_t next = line.find('\t', tab + 1);
                const std::string rest =
                    (next == std::string::npos) ? std::string() : line.substr(next);
                auto it = std::lower_bound(overrides.begin(), overrides.end(),
                                           path, byPath);
                if (it != overrides.end() && it->first == path) {
                    out.append(path);
                    out.push_back('\t');
                    out.append(it->second);
                    out.append(rest);
                    ++changed;
                    ++entries;
                    if (isGuiAtlasAssetPath(path.c_str())) {
                        ++atlasEntries;
                    }
                } else {
                    out.append(line);
                }
            }
            if (end == std::string::npos) {
                break;
            }
            out.push_back('\n');
            position = end + 1;
        }
        if (changed == 0) {
            continue;
        }

        const char* leaf = guestPath.c_str();
        const size_t slash = guestPath.find_last_of('/');
        leaf = (slash == std::string::npos) ? guestPath.c_str()
                                            : guestPath.c_str() + slash + 1;
        char tempPath[0x600]{};
        char finalPath[0x600]{};
        std::snprintf(tempPath, sizeof(tempPath), "%srr3-%s.tmp",
                      kRewrittenListDir, leaf);
        std::snprintf(finalPath, sizeof(finalPath), "%srr3-%s",
                      kRewrittenListDir, leaf);
        // THROUGH A TEMPORARY FILE, so a card that is pulled mid-write never
        // shows the guest a half list.
        std::FILE* file = std::fopen(tempPath, "wb");
        if (file == nullptr) {
            continue;
        }
        const size_t put = std::fwrite(out.data(), 1, out.size(), file);
        const bool ok = (put == out.size()) && (std::fclose(file) == 0);
        if (!ok) {
            std::remove(tempPath);
            continue;
        }
        std::remove(finalPath);
        if (std::rename(tempPath, finalPath) != 0) {
            std::remove(tempPath);
            continue;
        }
        ++written;
        bytes += out.size();
        gRewrittenLists.emplace_back(leaf, finalPath);
    }
    std::sort(gRewrittenLists.begin(), gRewrittenLists.end());
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    char diag[768];
    std::snprintf(diag, sizeof(diag),
                  "lists_touched=%u lists_written=%u entries_rewritten=%u "
                  "atlas_entries=%u bytes=%llu ms=%llu overrides=%zu",
                  touched, written, entries, atlasEntries,
                  (unsigned long long)bytes, (unsigned long long)elapsedMs,
                  overrides.size());
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-rewrite-all", 0, 0, diag);
}

// THE REWRITTEN COPY OF THE LIST THE GUEST ASKS FOR, OR nullptr.
const char* rewrittenListPath(const char* guestPath) {
    if (guestPath == nullptr || gRewrittenLists.empty()) {
        return nullptr;
    }
    const char* leaf = std::strrchr(guestPath, '/');
    leaf = (leaf != nullptr) ? leaf + 1 : guestPath;
    if (leaf[0] == '\0') {
        return nullptr;
    }
    const auto byLeaf = [](const std::pair<std::string, std::string>& entry,
                           const char* name) {
        return entry.first < name;
    };
    auto it = std::lower_bound(gRewrittenLists.begin(), gRewrittenLists.end(),
                               leaf, byLeaf);
    if (it == gRewrittenLists.end() || it->first != leaf) {
        return nullptr;
    }
    return it->second.c_str();
}

// WHAT THE SHARED OVERRIDE SET COST, AND WHAT IT COVERED.
struct OverrideSetStats {
    // The paths the guest has downloaded -- the set this port already had.
    unsigned recordedPaths{0};
    unsigned recordedUnresolved{0};
    // The atlas paths the two lists name, and the ones the resolver answered.
    unsigned atlasConsidered{0};
    unsigned atlasResolved{0};
    // The atlas paths a VERBATIM list also names, and which are therefore left
    // alone: overriding their digest would make two lists disagree and the
    // guest's merge would drop the entry.
    unsigned atlasExcluded{0};
    unsigned long long atlasBytes{0};
    unsigned long long elapsedMs{0};
};

std::vector<std::pair<std::string, std::string>> gSharedOverrides;
OverrideSetStats gSharedOverrideStats{};
bool gSharedOverridesBuilt = false;

// THE OVERRIDE SET EVERY CONSUMER SEES.
//
// Built ONCE per boot and shared, so the base-list rewrite, the per-category
// GUI list rewrite, the completed gui-assets manifest, the census, the
// inventory and the digest audit cannot disagree about a path they all name.
const std::vector<std::pair<std::string, std::string>>& sharedAssetOverrides() {
    if (gSharedOverridesBuilt) {
        return gSharedOverrides;
    }
    // The flag is set BEFORE the work, so a re-entrant call cannot build the
    // set twice; the set is complete when this returns.
    gSharedOverridesBuilt = true;
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();

    // 1. THE PATHS THE GUEST HAS DOWNLOADED -- the set this port already had.
    std::vector<std::pair<std::string, std::string>> overrides;
    collectAssetOverrides(overrides, &gSharedOverrideStats.recordedUnresolved);
    gSharedOverrideStats.recordedPaths = overrides.size();

    // 2. THE ATLAS PATHS THE LISTS NAME.
    std::vector<std::string> atlas;
    collectPathsFromList(kBaseListGuestPath, atlas, true);
    collectPathsFromList(kGuiBaseListGuestPath, atlas, true);
    // DEDUPLICATE BEFORE RESOLVING: the two lists name many of the same
    // atlases, so the same file would otherwise be resolved and hashed twice.
    std::sort(atlas.begin(), atlas.end());
    atlas.erase(std::unique(atlas.begin(), atlas.end()), atlas.end());
    gSharedOverrideStats.atlasConsidered = atlas.size();

    // 2b. THE OVERRIDE SET COVERS EVERY ATLAS PATH, AND EVERY LIST THE PORT
    //     SERVES IS REWRITTEN FROM THIS ONE SET.
    //
    // Leaving a path out of the set is NOT the fix. MEASURED 2026-10-26,
    // archive 20261026-230107-eden-f1h: with the paths a verbatim list also
    // names left out (atlas_considered=4289, atlas_excluded=2974,
    // atlas_resolved=1315), the base list fell back to EA's digest for those
    // paths while the per-category list the guest also reads still carried
    // EA's digest - and the guest compared the bytes it received against the
    // digest of the OTHER list. 844 md5 mismatches, the front end never
    // opened, and the run ended with Eden exiting on its own.
    //
    // The guest reads the per-category lists through the ordinary file path
    // (fopen-other: served=rr3core:/files/apk/res/asset_list_quest_*.txt), so
    // the port serves them VERBATIM unless it rewrites them. The rewrite is
    // what makes the lists agree: ONE set, applied to EVERY list, and then no
    // two lists can publish two digests for one path.
    //
    // See buildRewrittenAssetLists() for the other half.

    // 3. RESOLVE THEM, ONE AT A TIME, WITH THE SHARED CANDIDATE RULE.
    for (const std::string& path : atlas) {
        char source[0x600]{};
        char digest[33]{};
        unsigned long long size = 0;
        if (!resolveAssetLocalFile(path.c_str(), source, sizeof(source), digest,
                                   &size)) {
            // THE PORT CANNOT SERVE THIS ONE. The list keeps the shipped
            // digest for it, which is the honest answer: there are no bytes
            // here to describe.
            continue;
        }
        ++gSharedOverrideStats.atlasResolved;
        gSharedOverrideStats.atlasBytes += size;
        // BOTH SPELLINGS: the guest asks for "assets_<res>/..." and the lists
        // say "gui_assets/<res>/...", and the override set is keyed by the
        // path STRING.
        appendOverrideWithBothSpellings(overrides, path, std::string(digest));
    }
    // 4. SORTED AND UNIQUE, so every consumer can look an entry up with a
    //    binary search and so the cache key is a function of the SET.
    std::sort(overrides.begin(), overrides.end());
    overrides.erase(std::unique(overrides.begin(), overrides.end()), overrides.end());
    gSharedOverrides.swap(overrides);

    gSharedOverrideStats.elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                                     (tickFreq != 0 ? tickFreq : 1);
    // 5. THE COST, ON THE RECORD.
    char diag[768];
    std::snprintf(diag, sizeof(diag),
                  "recorded=%u recorded_unresolved=%u atlas_considered=%u "
                  "atlas_excluded=%u atlas_resolved=%u atlas_bytes=%llu ms=%llu total=%zu",
                  gSharedOverrideStats.recordedPaths,
                  gSharedOverrideStats.recordedUnresolved,
                  gSharedOverrideStats.atlasConsidered,
                  gSharedOverrideStats.atlasExcluded,
                  gSharedOverrideStats.atlasResolved,
                  (unsigned long long)gSharedOverrideStats.atlasBytes,
                  (unsigned long long)gSharedOverrideStats.elapsedMs,
                  gSharedOverrides.size());
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "asset-override-set", 0, 0, diag);
    return gSharedOverrides;
}

// The asset list the guest checks its downloads against -- its leaf name,
// its guest path and the card path of the rewritten copy are declared above
// withReadFallback, which is where the copy is served from.

bool buildConsistentAssetList(const char* listPath, const char* outputPath,
                               unsigned* rewrittenOut, unsigned* unresolvedOut) {
    if (rewrittenOut != nullptr) {
        *rewrittenOut = 0;
    }
    if (unresolvedOut != nullptr) {
        *unresolvedOut = 0;
    }
    if (listPath == nullptr || outputPath == nullptr) {
        return false;
    }
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();

    // 1. THE LIST, read through the port's own resolver, so the bytes are the
    //    bytes the guest would have read.
    Resolution resolution{};
    if (!resolvePath(listPath, false, resolution)) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-rewrite", 0, 0,
                            "FAILED reason=resolve list=files/.depot/asset_list_updates.txt");
        return false;
    }
    std::string text;
    char served[0x600]{};
    const int rc = withReadFallback(
        resolution, listPath,
        [&text, &served](const char* candidate) -> int {
            std::FILE* file = std::fopen(candidate, "rb");
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = (size > 0)
                ? std::fread(&text[0], 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                text.clear();
                return -1;
            }
            std::snprintf(served, sizeof(served), "%s", candidate);
            return 0;
        });
    if (rc != 0) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-rewrite", 0, 0,
                            "FAILED reason=read list=files/.depot/asset_list_updates.txt");
        return false;
    }

    // 2. THE LINES. Each one keeps its path text, its place in the file and
    //    its field layout; only the md5 (and the size fields, when the layout
    //    has them) is replaced, and only when the path resolves to a local
    //    file. An entry that resolves to nothing keeps the server's md5:
    //    there is no real digest to put there.
    std::string out;
    out.reserve(text.size() + 4096);
    unsigned rewritten = 0;
    unsigned unresolved = 0;
    unsigned entries = 0;
    unsigned logged = 0;
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
        const std::string line = text.substr(position, lineEnd - position);
        position = (end == std::string::npos) ? text.size() : end + 1;

        // The fields, split on TAB. The path is field 0, the md5 is field 1,
        // and the two size fields (when the layout has them) are 2 and 3.
        std::vector<std::string> fields;
        size_t fieldStart = 0;
        for (;;) {
            const size_t tab = line.find('\t', fieldStart);
            if (tab == std::string::npos) {
                fields.push_back(line.substr(fieldStart));
                break;
            }
            fields.push_back(line.substr(fieldStart, tab - fieldStart));
            fieldStart = tab + 1;
        }
        if (fields.size() < 2u || fields[0].empty()) {
            // A line that is not an entry (a header, a blank line) is copied
            // through untouched: the count and the order must not change.
            out += line;
            out += '\n';
            continue;
        }
        ++entries;
        const std::string& path = fields[0];
        const std::string& wanted = fields[1];
        char source[0x600]{};
        char digest[33]{};
        unsigned long long size = 0;
        if (!resolveAssetLocalFile(path.c_str(), source, sizeof(source), digest, &size)) {
            ++unresolved;
            out += line;
            out += '\n';
            if (logged < 80u) {
                ++logged;
                char diag[768];
                std::snprintf(diag, sizeof(diag),
                              "path=%s md5=%s kept=SERVER size=- source=none",
                              path.c_str(), wanted.c_str());
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "assetlist-entry", logged, 0, diag);
            }
            continue;
        }
        ++rewritten;
        out += fields[0];
        out += '\t';
        out += digest;
        for (size_t field = 2; field < fields.size(); ++field) {
            // The size fields describe the transfer. The port sends the file
            // RAW, so both fields are the number of bytes it sends: an entry
            // that claims a compressed size the port never uses is a claim
            // about bytes that are not on the wire.
            out += '\t';
            out += std::to_string(static_cast<unsigned long long>(size));
        }
        out += '\n';
        if (logged < 80u) {
            ++logged;
            char diag[1024];
            std::snprintf(diag, sizeof(diag),
                          "path=%s md5=%s->%s size=%llu source=%s",
                          path.c_str(), wanted.c_str(), digest, size, source);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "assetlist-entry", logged, 0, diag);
        }
    }

    // 3. WRITE IT, through a temporary name, so a reader never sees a
    //    half-written list.
    //
    //    THE TARGET IS REMOVED FIRST: libnx's fsdev rename does NOT overwrite
    //    an existing file (measured 2026-10-21, archive
    //    20261021-184611-eden-massetgate-manifest3), so a rename onto an
    //    existing target would leave the PREVIOUS run's list in place and the
    //    guest would read a stale list that looks like a fresh one.
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", outputPath);
    std::FILE* output = std::fopen(tempPath, "wb");
    bool written = false;
    int writeErrno = 0;
    if (output != nullptr) {
        const size_t bytes = out.empty() ? 0u : std::fwrite(out.data(), 1, out.size(), output);
        const bool flushed = (bytes == out.size());
        const int closeResult = std::fclose(output);
        writeErrno = errno;
        written = flushed && (closeResult == 0);
        if (written) {
            std::remove(outputPath);
            written = std::rename(tempPath, outputPath) == 0;
            if (!written) {
                writeErrno = errno;
            }
        }
    }
    else {
        writeErrno = errno;
    }
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    char diag[768];
    std::snprintf(diag, sizeof(diag),
                  "entries=%u rewritten=%u kept_server=%u bytes=%zu ms=%llu written=%d "
                  "errno=%d source=%s out=%s",
                  entries, rewritten, unresolved, out.size(),
                  (unsigned long long)elapsedMs, written ? 1 : 0, writeErrno,
                  served, outputPath);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-rewrite", 0, 0, diag);
    if (rewrittenOut != nullptr) {
        *rewrittenOut = rewritten;
    }
    if (unresolvedOut != nullptr) {
        *unresolvedOut = unresolved;
    }
    if (!written) {
        return false;
    }
    // COPIED, not aliased: the caller owns the buffer it passed in, and this
    // pointer outlives the call.
    static char servedPath[0x600]{};
    std::snprintf(servedPath, sizeof(servedPath), "%s", outputPath);
    gConsistentListServed = servedPath;
    return true;
}

// Rewrite the base asset list so that every entry the guest has ACTUALLY
// asked for describes the bytes the port serves.
//
// The entries the guest has not asked for keep the server's md5: the port is
// not claiming to have a digest for a file it has not resolved.
// WAIT FOR THE CARD TO ACTUALLY HOLD THE FILE.
//
// fclose() returning is NOT the file being on the card.
//
// MEASURED 2026-10-23, archive 20261023-005427-eden-gateB-r1: the port's own
// trace says the base-list rewrite took 198 ms ("ms=198"), and Eden's
// timestamps show the 4.2 MB file landing on the card 18.5 SECONDS later
// (t=5.23 the .tmp is written, t=23.75 the rename lands). The guest reads the
// base list at its 4th log ordinal -- inside that window -- so it read the
// SHIPPED list, whose digest for
// /gui_assets/960x640/common/sprites.android.atlas is
// bba63527235d60c88863fa10e111abc6, while the port serves
// c72568828875f4e2a6b11f5c944e27e0.
//
// That is the whole of the 24 "didn't match its MD5" lines, and it is why the
// guest never INSTALLED the atlas: the card's installed copy still carries its
// old mtime, because the guest gives up before the rename.
//
// The port is the only party that can arrange the ordering, and the guest has
// not started yet when this runs, so the wait is free.
bool waitForCardFile(const char* path, size_t expectedBytes, unsigned timeoutMs) {
    if (path == nullptr || expectedBytes == 0) {
        return false;
    }
    const uint64_t frequency = armGetSystemTickFreq();
    const uint64_t start = armGetSystemTick();
    for (;;) {
        struct stat info{};
        if (::stat(path, &info) == 0 &&
            static_cast<unsigned long long>(info.st_size) == expectedBytes) {
            return true;
        }
        const uint64_t elapsed = (armGetSystemTick() - start) * 1000ULL /
                                 (frequency != 0 ? frequency : 1);
        if (elapsed >= timeoutMs) {
            return false;
        }
        usleep(100 * 1000);
    }
}

// The md5 of a file, read straight off the card.
bool digestOfLocalFile(const char* path, char out[33]) {
    if (path == nullptr || out == nullptr) {
        return false;
    }
    out[0] = '\0';
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    Md5 md5;
    md5.begin();
    std::vector<unsigned char> chunk(64u * 1024u);
    for (;;) {
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), file);
        if (got > 0) {
            md5.update(chunk.data(), got);
        }
        if (got < chunk.size()) {
            break;
        }
    }
    std::fclose(file);
    md5.finish(out);
    return true;
}

// REPAIR THE CARD'S INSTALLED COPIES.
//
// WHY. The guest installs a download by renaming "<asset>.ast" onto
// "<asset>", and the rename only happens AFTER the downloaded bytes pass the
// guest's md5 check. So a stale file at the installed path is a standing
// "out of date" verdict: the guest re-downloads, the check keeps failing,
// and the stale file is never replaced. The port's own rename comment says
// the same thing: "the installed file stays STALE -- which is exactly what
// the guest then md5s against the manifest and rejects with 'didn't match
// its MD5'".
//
// MEASURED 2026-10-23, archive 20261023-133539-eden-astcard: the card's
// ".depot/assets_960x640/common/" still carried the 24,689 B revision
// (md5 43213527..., mtime 2026-10-22 01:14) that an EARLIER build of this
// port wrote there, while ".depot/assets_2048x1536/common/" and
// ".depot/assets_480x320/common/" had been refreshed to the revision the
// port serves (13:37). The guest reported "didn't match its MD5" 24 times
// for exactly the 960x640 pair, and for neither of the other two.
//
// WHAT IT DOES. For every asset the guest has recorded, the card's installed
// copy is compared with the bytes the port serves for that asset, and the
// installed copy is REPLACED when they differ. The replacement is the
// volume's own revision, read from the volume.
//
// WHAT IT DOES NOT DO. It does not invent a file, it does not touch a
// volume, and it does not touch an asset the port serves from the card
// itself -- those are already consistent by construction.
bool repairInstalledCopies(unsigned* repairedOut, unsigned* checkedOut) {
    if (repairedOut != nullptr) {
        *repairedOut = 0;
    }
    if (checkedOut != nullptr) {
        *checkedOut = 0;
    }
    // 1. THE RECORDED PATHS. The same file the base-list rewrite reads, so
    //    the two passes see the same set of assets.
    std::vector<std::string> requested;
    {
        std::FILE* file = std::fopen(kAssetPathFile, "rb");
        if (file != nullptr) {
            char line[1024];
            while (std::fgets(line, sizeof(line), file) != nullptr) {
                std::string path = line;
                while (!path.empty() &&
                       (path.back() == '\n' || path.back() == '\r')) {
                    path.pop_back();
                }
                size_t begin = 0;
                while (begin < path.size() && path[begin] == '/') {
                    ++begin;
                }
                if (begin >= path.size()) {
                    continue;
                }
                // WITH the leading separator, because that is the form the
                // resolver takes ("/assets_<res>/..."). The card path is
                // built from the tail, which has none.
                requested.push_back("/" + path.substr(begin));
            }
            std::fclose(file);
        }
    }
    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()),
                    requested.end());

    // 2. EACH ONE.
    unsigned logged = 0;
    for (const std::string& assetPath : requested) {
        const std::string tail = assetPath.substr(1);
        char source[0x600]{};
        char digest[33]{};
        unsigned long long size = 0;
        if (!resolveAssetLocalFile(assetPath.c_str(), source, sizeof(source),
                                   digest, &size)) {
            if (logged < 60u) {
                ++logged;
                char diag[320];
                std::snprintf(diag, sizeof(diag), "path=%s resolved=0", tail.c_str());
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "repair-candidate", logged, 0, diag);
            }
            continue;
        }
        // The card's installed copy of this asset, under BOTH spellings: the
        // one the guest asked for, and the other one it names.
        std::vector<std::string> spellings;
        spellings.push_back(tail);
        char other[0x500]{};
        if (alternateAssetSpelling(tail.c_str(), other, sizeof(other))) {
            spellings.push_back(other);
        }
        for (const std::string& spelling : spellings) {
            char cardPath[0x600]{};
            std::snprintf(cardPath, sizeof(cardPath), "%s/.depot/%s", kAppRoot,
                          spelling.c_str());
            struct stat info{};
            if (::stat(cardPath, &info) != 0 || !S_ISREG(info.st_mode)) {
                if (logged < 60u) {
                    ++logged;
                    char diag[900];
                    std::snprintf(diag, sizeof(diag),
                                  "path=%s resolved=1 digest=%.32s card=%s "
                                  "present=0 source=%s",
                                  tail.c_str(), digest, cardPath, source);
                    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                        "repair-candidate", logged, 0, diag);
                }
                continue;
            }
            if (checkedOut != nullptr) {
                ++*checkedOut;
            }
            char cardDigest[33]{};
            if (!digestOfLocalFile(cardPath, cardDigest)) {
                continue;
            }
            if (std::strcmp(cardDigest, digest) == 0) {
                continue;
            }
            // REPLACE IT WITH THE BYTES THE PORT SERVES.
            bool written = false;
            std::FILE* input = std::fopen(source, "rb");
            if (input != nullptr) {
                char tempPath[0x600]{};
                std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", cardPath);
                std::FILE* output = std::fopen(tempPath, "wb");
                if (output != nullptr) {
                    std::vector<unsigned char> chunk(64u * 1024u);
                    bool ok = true;
                    for (;;) {
                        const size_t got =
                            std::fread(chunk.data(), 1, chunk.size(), input);
                        if (got > 0 &&
                            std::fwrite(chunk.data(), 1, got, output) != got) {
                            ok = false;
                            break;
                        }
                        if (got < chunk.size()) {
                            break;
                        }
                    }
                    written = ok && std::fclose(output) == 0;
                    std::fclose(input);
                    if (written) {
                        // libnx's rename does NOT overwrite an existing file.
                        std::remove(cardPath);
                        written = std::rename(tempPath, cardPath) == 0;
                        if (written) {
                            // THE CARD NEEDS TIME. The bytes are not on the
                            // card when fclose() returns (MEASURED: the 4.2 MB
                            // base list lands ~18.5 s after the write), and
                            // the guest hashes the installed file at boot.
                            written = waitForCardFile(
                                cardPath, static_cast<size_t>(size), 30000u);
                        }
                    }
                }
                else {
                    std::fclose(input);
                }
            }
            char diag[900];
            std::snprintf(diag, sizeof(diag),
                          "spelling=%s card=%s old=%.32s new=%.32s bytes=%llu "
                          "written=%d source=%s",
                          spelling.c_str(), cardPath, cardDigest, digest, size,
                          written ? 1 : 0, source);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "installed-repair", 0,
                                written ? 1u : 0u, diag);
            if (written && repairedOut != nullptr) {
                ++*repairedOut;
            }
        }
    }
    return true;
}

// THE ENDPOINT'S ANSWER for a path, when the guest asks for `expected`.
//
// This is the endpoint's own selection -- the shared candidate list and the
// shared rule, with the endpoint's expected digest -- so what it returns is
// what the endpoint would send for that path. Returns false when nothing
// answers.
//
// `aliasOut` receives the resolution the answer came from when it is NOT the
// resolution that was asked for (the nearest lower resolution), and "-" when
// the answer is at the requested resolution.
bool endpointAnswerFor(const char* path, const char* expected,
                        asset::Selection& selectionOut, std::string& sourceOut,
                        std::string& aliasOut) {
    std::vector<asset::Candidate> candidates;
    asset::buildCandidates(path, candidates);
    std::vector<std::string> served(candidates.size());
    selectionOut = asset::selectCandidate(
        candidates, expected,
        [&served](const asset::Candidate& candidate, unsigned index, char outDigest[33],
                  unsigned long long& outSize, std::vector<unsigned char>& bytes) -> bool {
            return probeAssetCandidate(candidate, index, outDigest, outSize, bytes,
                                       served);
        });
    if (selectionOut.index < 0) {
        sourceOut = "-";
        aliasOut = "-";
        return false;
    }
    sourceOut = served[selectionOut.index];
    aliasOut = asset::aliasResolution(path, candidates[selectionOut.index]);
    return true;
}

// ---------------------------------------------------------------------------
// THE DIGEST INVARIANT, CHECKED DIRECTLY.
//
// THE INVARIANT. For every asset path the guest can request, the digest a
// manifest publishes for that path must be the md5 of the bytes the endpoint
// sends for that path.
//
// WHY IT NEEDS AN INSTRUMENT. The manifest side and the endpoint side share
// one candidate list and one selection rule now, so they cannot drift -- but
// "cannot drift" is a claim about the CODE, and the claim that matters is
// about the BYTES. This checks the bytes: for every path the port serves it
// takes the digest the manifests publish, asks the endpoint's own selection
// what it would send for that path when the guest asks for that digest, and
// compares the two.
//
// WHAT IT REPORTS. One line per asset ("asset-invariant") with the published
// digest, the served digest, whether they agree and which file answered; and
// a summary line with the counts. A disagreement is a broken invariant, and
// it is printed, not counted away.
//
// THE CONTROLS. "0 disagreements" is only a fact if the check RAN, so the
// audit carries two controls:
//
//   agree:  the volume's sentinel -- files/.depot/gui_assets/960x640/
//           agreement/sprites.android.atlas, which the data manifest
//           guarantees and which the endpoint's own control already uses --
//           is checked for agreement. It is a path that is KNOWN to agree,
//           and it can FAIL: the file can be missing, and the invariant can
//           be broken for it.
//
//   detect: the SAME path with ONE hex digit of the published digest
//           changed. The checker MUST report a disagreement. A checker that
//           reports agreement on a deliberately wrong digest cannot report
//           anything, and then "0 disagreements" would mean nothing.
//
// So a run with 0 disagreements and control_ok=1 is a real zero, and a run
// with 0 disagreements and control_ok=0 is a broken instrument.
// ---------------------------------------------------------------------------
// THE CHECK ITSELF: does the digest a manifest publishes for a path equal the
// digest the endpoint would send for that path?
//
// One function, used by the audit AND by the controls, so the "detect"
// control below exercises the very comparison the audit makes. A control that
// tested a different comparison would not prove that the audit can report a
// disagreement.
bool assetDigestsAgree(const asset::Selection& selection, const char* published) {
    return selection.index >= 0 && asset::digestEquals(selection.digest, published);
}

void auditAssetDigestInvariant() {
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();

    // 1. THE PUBLISHED DIGESTS: the override set the base list and the
    //    per-category gui list are rewritten from. It is the SHARED set, so
    //    this audit and those rewrites cannot disagree about a path.
    const std::vector<std::pair<std::string, std::string>>& published =
        sharedAssetOverrides();

    // 2. EACH ONE.
    unsigned checked = 0;
    unsigned agreed = 0;
    unsigned disagreed = 0;
    unsigned unresolved = 0;
    unsigned logged = 0;
    for (const std::pair<std::string, std::string>& entry : published) {
        asset::Selection selection{};
        std::string source;
        std::string alias;
        const bool found = endpointAnswerFor(entry.first.c_str(),
                                             entry.second.c_str(), selection, source,
                                             alias);
        ++checked;
        const bool agree = assetDigestsAgree(selection, entry.second.c_str());
        if (!found) {
            ++unresolved;
        }
        else if (agree) {
            ++agreed;
        }
        else {
            ++disagreed;
        }
        if (logged < 240u) {
            ++logged;
            char diag[1100];
            std::snprintf(diag, sizeof(diag),
                          "path=%s published=%.32s served=%.32s agree=%d found=%d "
                          "src=%.300s alias=%s cache=%d",
                          entry.first.c_str(), entry.second.c_str(),
                          found ? selection.digest : "-", agree ? 1 : 0,
                          found ? 1 : 0, found ? source.c_str() : "-",
                          found ? alias.c_str() : "-",
                          selection.fromDownloadCache ? 1 : 0);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "asset-invariant", logged, 0, diag);
        }
    }

    // 3. THE CONTROLS.
    //
    // The control path is the volume's sentinel: files/.depot/gui_assets/
    // 960x640/agreement/sprites.android.atlas. The data manifest names that
    // directory as the volume's own sentinel and the volume ships the file
    // (399 B), and the endpoint's own control already uses the same path.
    const char* const kControlPath =
        "/gui_assets/960x640/agreement/sprites.android.atlas";
    char controlPublished[33]{};
    char controlSource[0x600]{};
    unsigned long long controlSize = 0;
    const bool controlResolved =
        resolveAssetLocalFile(kControlPath, controlSource, sizeof(controlSource),
                               controlPublished, &controlSize);
    asset::Selection controlSelection{};
    std::string controlAnswer;
    std::string controlAlias;
    const bool controlFound =
        controlResolved && endpointAnswerFor(kControlPath, controlPublished,
                                             controlSelection, controlAnswer,
                                             controlAlias);
    const bool controlAgree = assetDigestsAgree(controlSelection, controlPublished);
    // THE DETECT CONTROL: the same path, with ONE hex digit of the published
    // digest changed. The checker MUST report a disagreement here.
    char corrupted[33]{};
    std::memcpy(corrupted, controlPublished, sizeof(corrupted));
    if (corrupted[0] != '\0') {
        corrupted[0] = (corrupted[0] == '0') ? '1' : '0';
    }
    asset::Selection detectSelection{};
    std::string detectAnswer;
    std::string detectAlias;
    const bool detectFound =
        controlResolved && endpointAnswerFor(kControlPath, corrupted, detectSelection,
                                             detectAnswer, detectAlias);
    const bool controlDetected =
        controlResolved && !assetDigestsAgree(detectSelection, corrupted);
    const int agreeControlOk = (controlResolved && controlAgree) ? 1 : 0;
    const int detectControlOk = (controlResolved && controlDetected) ? 1 : 0;
    {
        char diag[900];
        std::snprintf(diag, sizeof(diag),
                      "expect=agree observed=%s path=%s published=%.32s served=%.32s "
                      "resolved=%d agree=%d control_ok=%d",
                      controlAgree ? "agree" : "disagree", kControlPath,
                      controlPublished, controlFound ? controlSelection.digest : "-",
                      controlResolved ? 1 : 0, controlAgree ? 1 : 0, agreeControlOk);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "asset-invariant-control", 0, 0, diag);
    }
    {
        char diag[900];
        std::snprintf(diag, sizeof(diag),
                      "expect=disagree observed=%s path=%s published=%.32s served=%.32s "
                      "resolved=%d detected=%d control_ok=%d",
                      controlDetected ? "disagree" : "agree", kControlPath, corrupted,
                      detectFound ? detectSelection.digest : "-",
                      controlResolved ? 1 : 0, controlDetected ? 1 : 0,
                      detectControlOk);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "asset-invariant-control", 1, 0, diag);
    }

    // 4. THE SUMMARY.
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    {
        char diag[768];
        std::snprintf(diag, sizeof(diag),
                      "checked=%u agreed=%u disagreed=%u unresolved=%u published=%zu "
                      "controls=2 agree_control=%d detect_control=%d control_ok=%d "
                      "ms=%llu",
                      checked, agreed, disagreed, unresolved, published.size(),
                      agreeControlOk, detectControlOk,
                      (agreeControlOk && detectControlOk) ? 1 : 0,
                      (unsigned long long)elapsedMs);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "asset-invariant-summary", 0, disagreed, diag);
    }
}

bool buildConsistentBaseList(unsigned* rewrittenOut, unsigned* unresolvedOut) {
    if (rewrittenOut != nullptr) {
        *rewrittenOut = 0;
    }
    if (unresolvedOut != nullptr) {
        *unresolvedOut = 0;
    }
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();

    // 1. THE OVERRIDE SET -- THE SHARED SET, BUILT ONCE PER BOOT.
    //
    //    It is the recorded-download set this list has always been rewritten
    //    from, PLUS every atlas path the shipped lists name. The digest comes
    //    from the SAME resolver the pack endpoint uses, so the two sides
    //    cannot disagree about which file backs a path.
    //
    //    THE PATHS ARE DEDUPLICATED BEFORE THEY ARE RESOLVED. The card's
    //    asset_paths.txt is APPENDED to on every download, so it carries
    //    the same path many times over: MEASURED 2026-10-22, 1,388 lines
    //    and 47 distinct paths. Resolving every line meant reading and
    //    hashing the same file 1,388 times -- the pass cost 2,709 ms on
    //    Eden (archive 20261022-023023-eden-md5ship-r6) and the guest was
    //    waiting for it on the boot path. The distinct set is 30x smaller.
    //
    //    THE SAME SET THE PER-CATEGORY GUI LIST, THE COMPLETED MANIFEST, THE
    //    CENSUS, THE INVENTORY AND THE AUDIT USE, so none of them can
    //    disagree about a path they all name.
    const uint64_t overridesTick = armGetSystemTick();
    const std::vector<std::pair<std::string, std::string>>& overrides =
        sharedAssetOverrides();
    const uint64_t overridesMs = (armGetSystemTick() - overridesTick) * 1000ULL /
                                 (tickFreq != 0 ? tickFreq : 1);
    if (unresolvedOut != nullptr) {
        *unresolvedOut = gSharedOverrideStats.recordedUnresolved;
    }
    // The shared set is already sorted and duplicate-free, so the rewrite can
    // look an entry up with a binary search instead of scanning the map for
    // every one of the 40,862 entries.

    // 1b. THE CACHE KEY: a digest of the OVERRIDE SET.
    //
    // The rewrite is a pure function of two things: the shipped list (which
    // never changes -- it is inside a read-only volume) and the override
    // set. So when the override set is unchanged, the rewritten list on the
    // card is already correct and the whole pass -- read 4.2 MB, parse
    // 40,868 lines, write 4.2 MB back -- can be skipped.
    //
    // The key is the digest of the sorted "path\tdigest" lines, so a change
    // in EITHER the paths or the bytes behind them invalidates it.
    //
    // THE EXTENDED COVERAGE IS INSIDE THIS KEY. The key is computed over
    // EVERY entry of the shared override set, and that set now carries the
    // atlas paths the shipped lists name as well as the recorded-download
    // paths. So a card that still holds the OLD rewrite -- the one built from
    // 69 paths, with "entries=40862 rewritten=14 kept=40848" -- holds the
    // OLD key, the key does not match, and the list is rewritten from the
    // extended set instead of being served stale. A card that is NOT wiped
    // therefore gets the fix on the very next boot.
    //
    // MEASURED 2026-10-22, archive 20261022-023023-eden-md5ship-r6: the pass
    // cost 2,709 ms of the guest's boot. The guest is on the critical path
    // and hundreds of ms are acceptable while seconds are not.
    char overrideKey[33]{};
    {
        Md5 md5;
        md5.begin();
        for (const auto& entry : overrides) {
            const std::string line = entry.first + "\t" + entry.second + "\n";
            md5.update(line.data(), line.size());
        }
        md5.finish(overrideKey);
    }
    // The key is kept beside the rewritten list, in its own tiny file, so a
    // card that is wiped loses the cache with it and the next boot rebuilds
    // the list from scratch.
    if (baseListCacheIsCurrent(overrideKey)) {
        const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                                   (tickFreq != 0 ? tickFreq : 1);
        // AND PUT THE CARD'S COPY IN THE BULK RING.
        //
        // This branch writes nothing, and it is the branch the round takes
        // when the card's list is ALREADY the rewritten one -- which is the
        // common case after the first run.  Without this, the guest's second
        // ask for the 4.2 MB file has to be served from the SD card one small
        // piece at a time, and the round dies with no log line for 120 s
        // (measured in q43, q44 and q49; see registerBulkRead()).
        //
        // bulkOpenFile reads the file into memory AND registers it under the
        // path it was given, so one call does both.
        std::FILE* cached = nullptr;
        const bool cachedRead =
            bulkOpenFile(kConsistentBaseListPath, "rb", &cached);
        if (cached != nullptr) {
            std::fclose(cached);
        }
        char diag[768];
        std::snprintf(diag, sizeof(diag),
                      "entries=0 rewritten=0 overrides=%zu bytes=0 ms=%llu "
                      "overrides_ms=%llu written=1 cached=1 key=%.32s out=%s "
                      "ring=%d",
                      overrides.size(), (unsigned long long)elapsedMs,
                      (unsigned long long)overridesMs, overrideKey,
                      kConsistentBaseListPath, cachedRead ? 1 : 0);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "baselist-rewrite", 0, 0, diag);
        if (rewrittenOut != nullptr) {
            *rewrittenOut = 0;
        }
        if (unresolvedOut != nullptr) {
            *unresolvedOut = 0;
        }
        static char servedPath[0x600]{};
        std::snprintf(servedPath, sizeof(servedPath), "%s", kConsistentBaseListPath);
        gConsistentBaseListServed = servedPath;
        return true;
    }

    // 2. THE LIST, read through the port's own resolver.
    Resolution resolution{};
    if (!resolvePath(kBaseListGuestPath, false, resolution)) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "baselist-rewrite", 0, 0, "FAILED reason=resolve");
        return false;
    }
    std::string text;
    char served[0x600]{};
    const int rc = withReadFallback(
        resolution, kBaseListGuestPath,
        [&text, &served](const char* candidate) -> int {
            std::FILE* file = std::fopen(candidate, "rb");
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = (size > 0)
                ? std::fread(&text[0], 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                text.clear();
                return -1;
            }
            std::snprintf(served, sizeof(served), "%s", candidate);
            return 0;
        });
    if (rc != 0) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "baselist-rewrite", 0, 0, "FAILED reason=read");
        return false;
    }

    // 3. THE LINES. The path text, the order, the count and the field layout
    //    are preserved exactly; only the md5 of an entry the guest has asked
    //    for changes, and only to the digest of the bytes the port serves.
    const auto byPath = [](const std::pair<std::string, std::string>& entry,
                           const std::string& path) {
        return entry.first < path;
    };
    // THE KEY IS THE PATH THE GUEST ASKED FOR, AND NOTHING ELSE.
    //
    // THE FIRST VERSION OF THIS LOOKED THE ENTRY UP UNDER THE OTHER SPELLING
    // TOO -- "/assets_<res>/..." and "/gui_assets/<res>/..." name what a
    // reader would call the same asset -- and that was WRONG, and measurably
    // so. The two spellings do NOT resolve to the same file in this port:
    //
    //   "/assets_960x640/common/sprites.android.atlas"
    //       -> ".depot/assets_960x640/..." -> the CARD's loose tree, 24,689 B
    //          md5 43213527...
    //   "/gui_assets/960x640/common/sprites.android.atlas"
    //       -> ".depot/gui_assets/960x640/..." -> the gui VOLUME, 39,199 B
    //          md5 c7256882...
    //
    // Two different revisions of one logical asset. An override computed for
    // one of them and applied to the other puts the digest of one file next
    // to the bytes of the other, and the guest's md5 check then fails on an
    // asset the port answered CORRECTLY (measured, archive
    // 20261022-013604-eden-md5fix-final-r1: 12 "didn't match its MD5" on
    // /gui_assets/960x640/common/sprites.android.atlas, with the port's own
    // cc-asset-req line saying md5_match=1 for the bytes it sent).
    //
    // So the override is looked up by the EXACT path, and the digest in it is
    // the digest of the file the resolver picks for THAT path -- which is the
    // file the pack endpoint serves for it.
    const auto lookup = [&overrides, &byPath](const std::string& path) -> const std::string* {
        auto it = std::lower_bound(overrides.begin(), overrides.end(), path, byPath);
        if (it != overrides.end() && it->first == path) {
            return &it->second;
        }
        return nullptr;
    };
    std::string out;
    out.reserve(text.size() + 4096);
    unsigned entries = 0;
    unsigned rewritten = 0;
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
        const std::string line = text.substr(position, lineEnd - position);
        position = (end == std::string::npos) ? text.size() : end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            // A header line: copied through untouched.
            out += line;
            out += '\n';
            continue;
        }
        ++entries;
        const std::string path = line.substr(0, tab);
        const std::string* digest = lookup(path);
        if (digest == nullptr) {
            out += line;
            out += '\n';
            continue;
        }
        ++rewritten;
        out += path;
        out += '\t';
        out += *digest;
        // The rest of the line, if the layout has more fields.
        const size_t next = line.find('\t', tab + 1);
        if (next != std::string::npos) {
            out += line.substr(next);
        }
        out += '\n';
    }

    // 4. WRITE IT, through a temporary name, and REMOVE the target first:
    //    libnx's fsdev rename does NOT overwrite an existing file.
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", kConsistentBaseListPath);
    std::FILE* output = std::fopen(tempPath, "wb");
    bool written = false;
    int writeErrno = 0;
    if (output != nullptr) {
        const size_t bytes = out.empty() ? 0u : std::fwrite(out.data(), 1, out.size(), output);
        const bool flushed = (bytes == out.size());
        const int closeResult = std::fclose(output);
        writeErrno = errno;
        written = flushed && (closeResult == 0);
        if (written) {
            std::remove(kConsistentBaseListPath);
            written = std::rename(tempPath, kConsistentBaseListPath) == 0;
            if (!written) {
                writeErrno = errno;
            }
            else {
                // THE CARD NEEDS TIME. See waitForCardFile(): the 4.2 MB lands
                // on the card ~18.5 s after the write, and the guest reads the
                // base list long before that. Waiting here, before the guest
                // starts, is the difference between the guest reading the
                // REWRITTEN list and reading the SHIPPED one.
                const bool landed = waitForCardFile(kConsistentBaseListPath,
                                                    out.size(), 30000u);
                if (!landed) {
                    writeErrno = ETIMEDOUT;
                }
                // AND REGISTER THE BYTES WE JUST WROTE, so the guest's second
                // ask for this file is served from MEMORY instead of from the
                // SD card.  See registerBulkRead() for what the alternative
                // costs: a round that dies with no log line for 120 s.
                registerBulkRead(kConsistentBaseListPath, out.data(), out.size());
            }
        }
    }
    else {
        writeErrno = errno;
    }
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    char diag[768];
    std::snprintf(diag, sizeof(diag),
                  "entries=%u rewritten=%u overrides=%zu bytes=%zu ms=%llu "
                  "overrides_ms=%llu written=%d cached=0 key=%.32s errno=%d "
                  "source=%s out=%s",
                  entries, rewritten, overrides.size(), out.size(),
                  (unsigned long long)elapsedMs, (unsigned long long)overridesMs,
                  written ? 1 : 0, overrideKey, writeErrno,
                  served, kConsistentBaseListPath);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "baselist-rewrite", 0, 0, diag);
    if (rewrittenOut != nullptr) {
        *rewrittenOut = rewritten;
    }
    if (!written) {
        return false;
    }
    // THE KEY IS RECORDED ONLY AFTER THE LIST IT DESCRIBES IS ON THE CARD.
    // Writing it first would let a later boot bless a list that was never
    // written.
    writeBaseListCacheKey(overrideKey);
    static char servedPath[0x600]{};
    std::snprintf(servedPath, sizeof(servedPath), "%s", kConsistentBaseListPath);
    gConsistentBaseListServed = servedPath;
    return true;
}

// ---------------------------------------------------------------------------
// THE PER-CATEGORY GUI LIST: THE REWRITE, ITS INSTRUMENT, AND THE CENSUS.
// ---------------------------------------------------------------------------

// What a scan of one asset list found.
//
// "rewritten" counts the entries the port OVERRIDES -- the ones whose digest
// the port replaces with the digest of the bytes it serves. "kept" counts
// the entries it does NOT override: the shipped digest stays there, by
// design. "disagree" counts the overridden entries whose published digest is
// NOT the override set's digest -- the defect this whole change is about,
// in a form a run can count.
struct ListScan {
    unsigned entries{0};
    unsigned rewritten{0};
    unsigned kept{0};
    unsigned disagree{0};
    // THE ATLAS ENTRIES, COUNTED SEPARATELY.
    //
    // The atlas entries are the ones the guest actually failed on, and a
    // census that reported only a TOTAL could not tell "the scanner looked at
    // the atlas entries and they agree" from "the scanner never looked at
    // them". That distinction is the whole point: the census stayed green
    // over 40,848 kept entries while 125 atlases were never opened.
    unsigned atlasEntries{0};
    unsigned atlasRewritten{0};
    unsigned atlasDisagree{0};
    char firstRewritten[256]{};
    char firstKept[256]{};
    char firstDisagree[256]{};
    char disagreePublished[40]{};
    char disagreePort[40]{};
    // THE ATLAS CONTROL: one atlas entry the override set covers, the digest
    // the list publishes for it and the digest the port serves for it. The
    // pair is what a positive control compares, and it can be checked.
    char atlasSample[256]{};
    char atlasSamplePublished[40]{};
    char atlasSamplePort[40]{};
};

// THE DIGEST COLUMN OF ONE LINE, WITHOUT ITS TRAILING CARRIAGE RETURN.
//
// The lists are tab-separated and the md5 is the SECOND field. A shipped
// list that ends its lines with CRLF would otherwise publish "bba63527...\r"
// and every comparison against it would fail for a reason that has nothing
// to do with the bytes.
std::string digestFieldOf(const std::string& line, size_t tab) {
    const size_t next = line.find('\t', tab + 1);
    std::string digest = (next == std::string::npos)
        ? line.substr(tab + 1)
        : line.substr(tab + 1, next - tab - 1);
    while (!digest.empty() &&
           (digest.back() == '\r' || digest.back() == '\n' || digest.back() == ' ')) {
        digest.pop_back();
    }
    return digest;
}

// TWO DIGESTS, COMPARED WITHOUT REGARD TO CASE.
//
// The override set's digests are lowercase hex, because they come from the
// md5 implementation. A shipped list that published them in uppercase names
// the SAME bytes, and a case-sensitive comparison would report that list as
// DISAGREEING -- a loud false alarm in the one instrument that is supposed
// to be able to say "the lists agree".
bool digestsEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        const char left = (a[index] >= 'A' && a[index] <= 'F')
            ? static_cast<char>(a[index] - 'A' + 'a') : a[index];
        const char right = (b[index] >= 'A' && b[index] <= 'F')
            ? static_cast<char>(b[index] - 'A' + 'a') : b[index];
        if (left != right) {
            return false;
        }
    }
    return true;
}

// Scans one asset list against the override set.
//
// THE OVERRIDE SET IS A SORTED VECTOR, so the lookup is a binary search and
// the scan costs one pass over the list.
void scanAssetList(const std::string& text,
                   const std::vector<std::pair<std::string, std::string>>& overrides,
                   ListScan& out) {
    const auto byPath = [](const std::pair<std::string, std::string>& entry,
                           const std::string& path) {
        return entry.first < path;
    };
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
        const std::string line = text.substr(position, lineEnd - position);
        position = (end == std::string::npos) ? text.size() : end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            // A header line: it names no asset.
            continue;
        }
        const std::string path = line.substr(0, tab);
        ++out.entries;
        // THE ATLAS ENTRIES ARE COUNTED ON THEIR OWN, so a census can prove
        // it looked at the paths the guest failed on.
        const bool atlas = isGuiAtlasAssetPath(path.c_str());
        if (atlas) {
            ++out.atlasEntries;
        }
        auto it = std::lower_bound(overrides.begin(), overrides.end(), path, byPath);
        if (it == overrides.end() || it->first != path) {
            ++out.kept;
            if (out.firstKept[0] == '\0') {
                std::snprintf(out.firstKept, sizeof(out.firstKept), "%s", path.c_str());
            }
            continue;
        }
        ++out.rewritten;
        if (atlas) {
            ++out.atlasRewritten;
        }
        if (out.firstRewritten[0] == '\0') {
            std::snprintf(out.firstRewritten, sizeof(out.firstRewritten), "%s",
                          path.c_str());
        }
        const std::string published = digestFieldOf(line, tab);
        if (!digestsEqual(published, it->second)) {
            ++out.disagree;
            if (atlas) {
                ++out.atlasDisagree;
            }
            if (out.firstDisagree[0] == '\0') {
                std::snprintf(out.firstDisagree, sizeof(out.firstDisagree), "%s",
                              path.c_str());
                std::snprintf(out.disagreePublished, sizeof(out.disagreePublished),
                              "%s", published.c_str());
                std::snprintf(out.disagreePort, sizeof(out.disagreePort), "%s",
                              it->second.c_str());
            }
        }
        else if (atlas && out.atlasSample[0] == '\0') {
            // THE ATLAS CONTROL SAMPLE: an atlas entry the override set
            // covers AND the list publishes correctly. The control compares
            // this pair, so it is a pair that was really observed.
            std::snprintf(out.atlasSample, sizeof(out.atlasSample), "%s",
                          path.c_str());
            std::snprintf(out.atlasSamplePublished,
                          sizeof(out.atlasSamplePublished), "%s", published.c_str());
            std::snprintf(out.atlasSamplePort, sizeof(out.atlasSamplePort), "%s",
                          it->second.c_str());
        }
    }
}

// THE POSITIVE CONTROL for the per-category list instrument.
//
// The instrument reports every entry it rewrote. A run in which it rewrote
// NOTHING and a run in which the instrument was BROKEN would look the same
// if the instrument only ever reported rewrites. So the control reports BOTH
// a rewritten sample and a deliberately-known-UNCHANGED sample, plus the
// counts that make "nothing matched" and "nothing was looked at" two
// different observations.
//
// Both samples are known BEFORE the run: a rewritten sample is an entry the
// port overrides (the port's digest replaces the shipped one there, by
// design), and a known-unchanged sample is an entry the port does NOT
// override (the shipped digest stays there, by design).
void logGuiListControl(const char* status, const ListScan& scan, size_t overrides) {
    char diag[1400];
    std::snprintf(diag, sizeof(diag),
                  "status=%s entries=%u rewritten=%u kept=%u disagree=%u "
                  "overrides=%zu rewritten_sample=%s kept_sample=%s",
                  (status != nullptr && status[0] != '\0') ? status : "?",
                  scan.entries, scan.rewritten, scan.kept, scan.disagree,
                  overrides,
                  (scan.firstRewritten[0] != '\0') ? scan.firstRewritten : "none",
                  (scan.firstKept[0] != '\0') ? scan.firstKept : "none");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-gui-control", 0, 0, diag);
}

// Reads a whole file into `out`. Used for the card copies, whose path is
// already a real path.
bool readWholeFile(const char* path, std::string& out) {
    if (path == nullptr) {
        return false;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.assign(static_cast<size_t>(size), '\0');
    const size_t got = (size > 0)
        ? std::fread(&out[0], 1, static_cast<size_t>(size), file)
        : 0u;
    std::fclose(file);
    if (got != static_cast<size_t>(size)) {
        out.clear();
        return false;
    }
    return true;
}

// TRUE when the rewritten per-category list on the card was built from THIS
// override set, and the pass can be skipped.
//
// The rewritten list is a pure function of the shipped list (read-only, never
// changes) and the override set, so an unchanged override set means the file
// on the card is already correct. The key file is 33 bytes; reading it and
// the list's own size is the whole cost of the check.
//
// The list's OWN size is checked too, because the key alone would happily
// bless a list that was deleted or truncated after the key was written.
bool guiBaseListCacheIsCurrent(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    std::FILE* keyFile = std::fopen(kGuiBaseListKeyPath, "rb");
    if (keyFile == nullptr) {
        return false;
    }
    char stored[64]{};
    size_t got = std::fread(stored, 1, sizeof(stored) - 1, keyFile);
    std::fclose(keyFile);
    if (got == 0) {
        return false;
    }
    while (got > 0 && (stored[got - 1] == '\n' || stored[got - 1] == '\r' ||
                       stored[got - 1] == ' ')) {
        stored[--got] = '\0';
    }
    if (std::strcmp(stored, key) != 0) {
        return false;
    }
    struct stat info{};
    if (::stat(kConsistentGuiBaseListPath, &info) != 0 || info.st_size <= 0) {
        return false;
    }
    return true;
}

// Records the override set the rewritten per-category list was built from.
//
// IT IS RECORDED ONLY AFTER THE LIST IT DESCRIBES IS ON THE CARD. Writing it
// first would let a later boot bless a list that was never written.
void writeGuiBaseListCacheKey(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return;
    }
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", kGuiBaseListKeyPath);
    std::FILE* file = std::fopen(tempPath, "wb");
    if (file == nullptr) {
        return;
    }
    std::fprintf(file, "%s\n", key);
    std::fclose(file);
    std::remove(kGuiBaseListKeyPath);
    std::rename(tempPath, kGuiBaseListKeyPath);
}

// Rewrite the per-category GUI asset list so that every entry the guest has
// ACTUALLY asked for describes the bytes the port serves.
//
// THE SAME OVERRIDE SET AS THE BASE LIST. The shared set (sharedAssetOverrides())
// holds the recorded-download paths AND every atlas path the shipped lists
// name, each resolved with the same resolver and expanded into both
// spellings, so the per-category list and the base list are rewritten from ONE
// set and publish the SAME digest for a path they both name.
//
// THE SAME EXACT-PATH LOOKUP. An entry is rewritten only when the list names
// a path the override set holds, under that path's EXACT spelling. The
// alternate spelling is NOT tried here: the two spellings of one asset do
// not necessarily name one file, and a digest taken from the other spelling
// is a digest of bytes the guest is not looking at. The base list's own
// lookup works the same way, which is what makes the two lists agree.
//
// A FAILURE HERE COSTS THE CONSISTENCY AND NOTHING ELSE: the shipped list
// is still served, and the log line says why.
bool buildConsistentGuiBaseList(unsigned* rewrittenOut, unsigned* keptOut) {
    if (rewrittenOut != nullptr) {
        *rewrittenOut = 0;
    }
    if (keptOut != nullptr) {
        *keptOut = 0;
    }
    const uint64_t startTick = armGetSystemTick();
    const uint64_t tickFreq = armGetSystemTickFreq();

    // 1. THE OVERRIDE SET -- THE SAME SET THE BASE LIST IS REWRITTEN WITH.
    //
    // It is the SHARED set: the recorded-download paths plus every atlas path
    // the shipped lists name, each with the digest of the bytes the port
    // serves for it. One set, so the two lists are rewritten from ONE table
    // and publish the SAME digest for a path they both name.
    const std::vector<std::pair<std::string, std::string>>& overrides =
        sharedAssetOverrides();

    // 2. THE CACHE KEY: the digest of the OVERRIDE SET.
    //
    // The rewrite is a pure function of two things: the shipped list (which
    // never changes -- it is inside a read-only volume) and the override
    // set. So when the override set is unchanged, the rewritten list on the
    // card is already correct and the pass can be skipped.
    //
    // THE KEY IS A FUNCTION OF THE WHOLE SET, so the extended coverage
    // CHANGES IT: a card that still holds the OLD rewrite has the OLD key,
    // the key does not match, and the list is rewritten from the extended set
    // instead of being served stale.
    char overrideKey[33]{};
    {
        Md5 md5;
        md5.begin();
        for (const auto& entry : overrides) {
            const std::string line = entry.first + "\t" + entry.second + "\n";
            md5.update(line.data(), line.size());
        }
        md5.finish(overrideKey);
    }

    // 3. THE CACHED COPY.
    //
    // The counts and the samples are read back OUT OF THE CARD COPY rather
    // than remembered from the run that wrote it, so a cached run reports
    // the same thing a rebuilt run reports. A cached run that reported
    // zeroes would be indistinguishable from a list with no entries in it.
    if (guiBaseListCacheIsCurrent(overrideKey)) {
        std::string cached;
        ListScan scan{};
        if (readWholeFile(kConsistentGuiBaseListPath, cached)) {
            scanAssetList(cached, overrides, scan);
        }
        char source[0x600]{};
        Resolution resolution{};
        if (resolvePath(kGuiBaseListGuestPath, false, resolution)) {
            std::snprintf(source, sizeof(source), "%s", resolution.primary);
        }
        std::snprintf(gConsistentGuiList.source, sizeof(gConsistentGuiList.source),
                      "%s", source);
        std::snprintf(gConsistentGuiList.destination,
                      sizeof(gConsistentGuiList.destination), "%s",
                      kConsistentGuiBaseListPath);
        gConsistentGuiList.entries = scan.entries;
        gConsistentGuiList.rewritten = scan.rewritten;
        gConsistentGuiList.kept = scan.kept;
        gConsistentGuiList.served = gConsistentGuiList.destination;
        const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                                   (tickFreq != 0 ? tickFreq : 1);
        char diag[1100];
        std::snprintf(diag, sizeof(diag),
                      "entries=%u rewritten=%u kept=%u overrides=%zu bytes=- ms=%llu "
                      "written=1 cached=1 key=%.32s source=%s out=%s",
                      scan.entries, scan.rewritten, scan.kept, overrides.size(),
                      (unsigned long long)elapsedMs, overrideKey, source,
                      kConsistentGuiBaseListPath);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-rewrite-gui", 0, 0, diag);
        logGuiListControl("OK-CACHED", scan, overrides.size());
        if (rewrittenOut != nullptr) {
            *rewrittenOut = scan.rewritten;
        }
        if (keptOut != nullptr) {
            *keptOut = scan.kept;
        }
        return true;
    }

    // 4. THE LIST, read through the port's own resolver, so the bytes are
    //    the bytes the guest would have read.
    Resolution resolution{};
    if (!resolvePath(kGuiBaseListGuestPath, false, resolution)) {
        ListScan scan{};
        logGuiListControl("FAILED-RESOLVE", scan, overrides.size());
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-rewrite-gui", 0, 0,
                            "FAILED reason=resolve list=files/apk/res/asset_list_base_gui_assets.txt");
        return false;
    }
    std::string text;
    char served[0x600]{};
    const int rc = withReadFallback(
        resolution, kGuiBaseListGuestPath,
        [&text, &served](const char* candidate) -> int {
            std::FILE* file = std::fopen(candidate, "rb");
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = (size > 0)
                ? std::fread(&text[0], 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                text.clear();
                return -1;
            }
            std::snprintf(served, sizeof(served), "%s", candidate);
            return 0;
        });
    if (rc != 0) {
        ListScan scan{};
        logGuiListControl("FAILED-READ", scan, overrides.size());
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-rewrite-gui", 0, 0,
                            "FAILED reason=read list=files/apk/res/asset_list_base_gui_assets.txt");
        return false;
    }

    // 5. THE LINES. The path text, the order, the count and the field layout
    //    are preserved exactly; only the md5 of an entry the port overrides
    //    changes, and it changes to the digest of the bytes the port serves.
    const auto byPath = [](const std::pair<std::string, std::string>& entry,
                           const std::string& path) {
        return entry.first < path;
    };
    const auto lookup = [&overrides, &byPath](const std::string& path) -> const std::string* {
        auto it = std::lower_bound(overrides.begin(), overrides.end(), path, byPath);
        if (it != overrides.end() && it->first == path) {
            return &it->second;
        }
        return nullptr;
    };
    std::string out;
    out.reserve(text.size() + 4096);
    ListScan scan{};
    unsigned logged = 0;
    size_t position = 0;
    while (position < text.size()) {
        const size_t end = text.find('\n', position);
        const size_t lineEnd = (end == std::string::npos) ? text.size() : end;
        const std::string line = text.substr(position, lineEnd - position);
        position = (end == std::string::npos) ? text.size() : end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            // A header line: copied through untouched.
            out += line;
            out += '\n';
            continue;
        }
        ++scan.entries;
        const std::string path = line.substr(0, tab);
        const std::string* digest = lookup(path);
        if (digest == nullptr) {
            // THE DELIBERATELY-KNOWN-UNCHANGED ENTRY. The port does not
            // override this path, so the shipped digest stays, by design.
            ++scan.kept;
            if (scan.firstKept[0] == '\0') {
                std::snprintf(scan.firstKept, sizeof(scan.firstKept), "%s",
                              path.c_str());
            }
            out += line;
            out += '\n';
            continue;
        }
        ++scan.rewritten;
        if (scan.firstRewritten[0] == '\0') {
            std::snprintf(scan.firstRewritten, sizeof(scan.firstRewritten), "%s",
                          path.c_str());
        }
        const std::string published = digestFieldOf(line, tab);
        out += path;
        out += '\t';
        out += *digest;
        // The rest of the line, if the layout has more fields.
        const size_t next = line.find('\t', tab + 1);
        if (next != std::string::npos) {
            out += line.substr(next);
        }
        out += '\n';
        // ONE LINE PER OVERRIDDEN ENTRY: the path, the digest before and
        // after, and the file the new digest was taken from.
        //
        // The digest written is the OVERRIDE SET's digest, so the two lists
        // the guest reads cannot disagree. The re-resolve is here for the
        // "source=" field and for the check: if the file behind the path
        // changed since the override set was collected, the new digest and
        // the served one would differ, and the line says so.
        if (logged < 80u) {
            ++logged;
            char source[0x600]{};
            char servedDigest[33]{};
            unsigned long long size = 0;
            const bool resolved = resolveAssetLocalFile(
                path.c_str(), source, sizeof(source), servedDigest, &size);
            char diag[1700];
            std::snprintf(diag, sizeof(diag),
                          "path=%s md5=%s->%s size=%llu source=%s served_md5=%s "
                          "digest_match=%d",
                          path.c_str(), published.c_str(), digest->c_str(), size,
                          resolved ? source : "none",
                          resolved ? servedDigest : "-",
                          (resolved && servedDigest == *digest) ? 1 : 0);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "assetlist-entry-gui", logged, 0, diag);
        }
    }

    // 6. WRITE IT, through a temporary name, and REMOVE the target first:
    //    libnx's fsdev rename does NOT overwrite an existing file.
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", kConsistentGuiBaseListPath);
    std::FILE* output = std::fopen(tempPath, "wb");
    bool written = false;
    int writeErrno = 0;
    if (output != nullptr) {
        const size_t bytes = out.empty() ? 0u : std::fwrite(out.data(), 1, out.size(), output);
        const bool flushed = (bytes == out.size());
        const int closeResult = std::fclose(output);
        writeErrno = errno;
        written = flushed && (closeResult == 0);
        if (written) {
            std::remove(kConsistentGuiBaseListPath);
            written = std::rename(tempPath, kConsistentGuiBaseListPath) == 0;
            if (!written) {
                writeErrno = errno;
            }
            else {
                // THE CARD NEEDS TIME. See waitForCardFile(): the file lands
                // on the card long after the write returns, and the guest
                // reads this list early. Waiting here, before the guest
                // starts, is the difference between the guest reading the
                // REWRITTEN list and reading the SHIPPED one.
                const bool landed = waitForCardFile(kConsistentGuiBaseListPath,
                                                    out.size(), 30000u);
                if (!landed) {
                    writeErrno = ETIMEDOUT;
                }
            }
        }
    }
    else {
        writeErrno = errno;
    }
    const uint64_t elapsedMs = (armGetSystemTick() - startTick) * 1000ULL /
                               (tickFreq != 0 ? tickFreq : 1);
    char diag[1200];
    std::snprintf(diag, sizeof(diag),
                  "entries=%u rewritten=%u kept=%u overrides=%zu bytes=%zu ms=%llu "
                  "written=%d cached=0 key=%.32s errno=%d source=%s out=%s",
                  scan.entries, scan.rewritten, scan.kept, overrides.size(),
                  out.size(), (unsigned long long)elapsedMs, written ? 1 : 0,
                  overrideKey, writeErrno, served, kConsistentGuiBaseListPath);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-rewrite-gui", 0, 0, diag);
    logGuiListControl(written ? "OK" : "FAILED-WRITE", scan, overrides.size());
    if (rewrittenOut != nullptr) {
        *rewrittenOut = scan.rewritten;
    }
    if (keptOut != nullptr) {
        *keptOut = scan.kept;
    }
    if (!written) {
        return false;
    }
    std::snprintf(gConsistentGuiList.source, sizeof(gConsistentGuiList.source),
                  "%s", served);
    std::snprintf(gConsistentGuiList.destination, sizeof(gConsistentGuiList.destination),
                  "%s", kConsistentGuiBaseListPath);
    gConsistentGuiList.entries = scan.entries;
    gConsistentGuiList.rewritten = scan.rewritten;
    gConsistentGuiList.kept = scan.kept;
    gConsistentGuiList.served = gConsistentGuiList.destination;
    writeGuiBaseListCacheKey(overrideKey);
    return true;
}

// ---------------------------------------------------------------------------
// THE LAUNCHER-SIDE SETTINGS LAYER -- THE PS VITA LESSON, APPLIED.
//
// WHAT THE GUEST ACTUALLY DOES (measured 2026-10-31, archive
// 20261031-010828-eden-rr3-q30-res-base-2048-view-720, guest-view.log):
//
//   2088  ======= platform AndroidHigh
//   2089  map-app   rr3core:/files/.depot/eds/
//   2093  readdir   AndroidHigh.plist
//   2099  Searching for settings for device: AndroidHigh
//   2103  stat-path guest='sdmc:/switch/rr3-native-rehost/.depot/eds/AndroidHigh.plist'
//                   primary='rr3core:/files/.depot/eds/AndroidHigh.plist'
//                   fallback='rr3core:/files/apk/res/eds/AndroidHigh.plist'
//   2107  open-ok   e6ea60c54026f388a8a65a62e99576bf
//   2108  mmap-file-ok len=6808
//   2126  Loaded settings file: AndroidHigh.plist
//
// THREE THINGS IN THAT LIST DECIDE THE WHOLE DESIGN:
//
//  1. THE FILE MAY NOT BE RENAMED OR REMOVED.  The guest READDIRs the
//     directory to learn the name, so a profile the port does not publish is
//     a profile the guest never asks for.  The port can change the BYTES.
//
//  2. THE BYTES MAY NOT CHANGE SIZE.  The guest mmaps 6808 bytes and the
//     size comes from the stat, so the port has to serve a file of exactly
//     the same length.  That is why the translation keeps the length: the
//     ciphertext is the plaintext XORed with a stream of the same length.
//
//  3. THE HOOK BELONGS IN THE RESOLVER, NOT IN THE READER.  The guest reads
//     the profile through guestOpen() and then MMAPS the descriptor, so a
//     hook in guestFopen() would never fire at all.  And the asset list
//     publishes an md5 for "/eds/AndroidHigh.plist" that has to describe the
//     bytes the port serves, so the substitution has to happen where BOTH
//     the read and the digest computation see it -- resolvePath(), which is
//     the one place every path takes.
//
// WHAT THIS IS: the port acting as the LAUNCHER, the way the PS Vita port
// does.  The Vita launcher reads a plain text config.txt and TRANSLATES it
// into the guest's own encrypted device profile (eds/Vita*.plist); the
// guest never sees the plain text.  This does the same thing, with the
// port's rr3-settings.txt on the card.
//
// WHAT IT IS NOT: the host inventing a quality setting.  The keys are the
// GUEST'S OWN keys (the ones its native readers ask for), the profile is
// the guest's own profile, and the cipher is the guest's own cipher.
//
// THE DEFAULT IS "NOTHING HAPPENS".  A card with no rr3-settings.txt serves
// the stock profile, byte for byte, and the port logs that it did.  The
// settings layer can only change the picture when a user asks for it.
//
// THE PRICE, STATED PLAINLY: changing a QUALITY switch changes the SET of
// shaders the guest compiles, and the guest then recompiles all of them
// (measured: archive 20261031-003750, the run died at 42 s while it was
// still pulling .vsh/.fsh).  The PS Vita port had the same problem and
// shipped a shader cache and a "clear the cache" button with it.  So the
// settings file has to be CONSTANT from run to run -- which it is, because
// it is a file, and not something the port rewrites under the guest's feet
// the way editing the volume in place did.
// ---------------------------------------------------------------------------
namespace {

// The settings file, on the card, beside the port's other control files.
constexpr const char* kSettingsFile =
    "sdmc:/switch/rr3-native-rehost/rr3-settings.txt";

// Where the generated profile goes.  The cache root is already a writable
// root (ensureWritableRoots), so this needs no new directory.
constexpr const char* kProfileCacheRoot =
    "sdmc:/switch/rr3-native-rehost/cache";

// The profile the guest asks for, when the settings do not name one.
//
// The guest picks the tier from the device identity the port reports (the GL
// identity in gfx_gles.cpp), and MEASURED 2026-10-31 (the q30 archive, line
// 2099) that tier is "AndroidHigh".  A settings file may name another one
// with the "perfil" key.
constexpr const char* kDefaultProfileName = "AndroidHigh.plist";

// What the settings layer knows about this run.
struct ProfileSettingsState {
    bool loaded{false};
    bool usable{false};      // the settings say something at all
    GuestSettings settings{};
    char builtFor[128]{};     // the profile we generated
    char generated[0x600]{};  // the file we generated
    char source[0x600]{};     // where the stock profile came from
    int changed{0};
    char error[192]{};
};

ProfileSettingsState gProfileSettings;

// TRUE while the settings layer is building the profile.  The build resolves
// the profile path ITSELF, and that resolve would come straight back here.
bool gBuildingProfile = false;

// TRUE for a guest path whose LEAF is a "*.plist" file.
//
// The device profile is one of many plists the guest has, so this is only
// the first half of the test: the parent directory has to be "eds" as well.
//
// THE PREDICATE ITSELF LIVES IN THE SETTINGS MODULE, because that one CAN be
// built and run by a host compiler -- and the version that lived here was
// off by one byte ("base + length - 5" against a SIX-character suffix), so
// it was always false and the settings layer never fired.
bool hasPlistLeaf(const char* guestPath) {
    return rr3::guestProfileLeafIsPlist(guestPath);
}

// TRUE when the parent directory of the guest path is "eds".
bool parentDirectoryIsEds(const char* guestPath) {
    const char* slash = std::strrchr(guestPath, '/');
    if (slash == nullptr || slash == guestPath) {
        return false;
    }
    char parent[0x600]{};
    const size_t cut = static_cast<size_t>(slash - guestPath);
    if (cut >= sizeof(parent)) {
        return false;
    }
    std::memcpy(parent, guestPath, cut);
    parent[cut] = '\0';
    const char* parentBase = pathBasename(parent);
    return parentBase != nullptr && std::strcmp(parentBase, "eds") == 0;
}

// TRUE for a guest path that names a DEVICE PROFILE: a "*.plist" file inside
// an "eds" directory.
//
// The parent directory has to be "eds", because the guest has other plists
// (the asset lists are .txt, but the volume carries iPad/iPhone profiles
// under the same "eds" directory) and only the device profile carries the
// quality switches.  The two halves are separate functions because the hook
// has to tell "not a plist at all" (the common case, and quiet) from "a
// plist, but not the device profile" (worth a line in the log).

// Puts a different leaf on the same path: "rr3core:/files/.depot/eds/A.plist"
// with "B.plist" gives "rr3core:/files/.depot/eds/B.plist".
bool replaceBasename(const char* path, const char* newBase, char* out,
                     size_t outSize) {
    if (path == nullptr || newBase == nullptr || out == nullptr ||
        outSize == 0) {
        return false;
    }
    const char* slash = std::strrchr(path, '/');
    if (slash == nullptr) {
        return false;
    }
    const size_t prefix = static_cast<size_t>(slash - path) + 1;
    const int written = std::snprintf(out, outSize, "%.*s%s",
                                      static_cast<int>(prefix), path, newBase);
    return written > 0 && static_cast<size_t>(written) < outSize;
}

// Reads the settings file ONCE.
//
// They are a file on the card that a user edits BETWEEN runs, and the port
// must not see them change under it: the guest reads its profile during
// boot, and a profile that changes mid-boot is a profile the guest mmaps in
// two halves.
bool loadProfileSettings() {
    if (gProfileSettings.loaded) {
        return gProfileSettings.usable;
    }
    gProfileSettings.loaded = true;
    GuestSettings settings;
    const bool present = guestSettingsLoad(kSettingsFile, settings);
    gProfileSettings.settings = settings;
    gProfileSettings.usable =
        present && (settings.count > 0 || settings.profile[0] != '\0');    char diag[640];
    std::snprintf(diag, sizeof(diag),
                  "file=%s present=%d settings=%d tier=%s profile=%s target=%s "
                  "usable=%d error=%s",
                  kSettingsFile, present ? 1 : 0, settings.count,
                  settings.tier[0] != '\0' ? settings.tier : "-",
                  settings.profile[0] != '\0' ? settings.profile : "-",
                  settings.target[0] != '\0' ? settings.target : "-",
                  gProfileSettings.usable ? 1 : 0,
                  settings.error[0] != '\0' ? settings.error : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "settings-load", 0, 0, diag);
    return gProfileSettings.usable;
}

// The name of the profile to patch: the file name, ".plist" included.
const char* profileTargetName() {
    if (gProfileSettings.settings.target[0] != '\0') {
        // "perfil = AndroidHigh" and "perfil = AndroidHigh.plist" mean the
        // same profile, so the extension is added when it is missing.
        static char name[128]{};
        std::snprintf(name, sizeof(name), "%s", gProfileSettings.settings.target);
        const size_t length = std::strlen(name);
        if (length < 6 || std::strcmp(name + length - 6, ".plist") != 0) {
            std::snprintf(name + length, sizeof(name) - length, ".plist");
        }
        return name;
    }
    return kDefaultProfileName;
}

// Builds the profile the guest will read, with the settings applied.
//
// Returns true when the generated file is on the card and may be served.
bool buildDeviceProfile(const char* profileName) {
    if (profileName == nullptr || profileName[0] == '\0') {
        return false;
    }
    // THE GUEST PATH SPELLING, measured (q30 archive, line 2103):
    //   guest='sdmc:/switch/rr3-native-rehost/.depot/eds/AndroidHigh.plist'
    //   primary='rr3core:/files/.depot/eds/AndroidHigh.plist'
    //   fallback='rr3core:/files/apk/res/eds/AndroidHigh.plist'
    char guestPath[0x600]{};
    std::snprintf(guestPath, sizeof(guestPath),
                  "sdmc:/switch/rr3-native-rehost/.depot/eds/%s", profileName);
    Resolution resolution{};
    gBuildingProfile = true;
    const bool resolved = resolvePath(guestPath, false, resolution);
    gBuildingProfile = false;
    if (!resolved) {
        char diag[640];
        std::snprintf(diag, sizeof(diag), "profile=%s FAILED resolve", profileName);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "settings-profile", 0, 0, diag);
        return false;
    }
    // THE STOCK PROFILE, read the way the port serves it.
    std::string original;
    char source[0x600]{};
    if (!readWholeFile(resolution.primary, original)) {
        if (resolution.fallback[0] == '\0' ||
            !readWholeFile(resolution.fallback, original)) {
            char diag[640];
            std::snprintf(diag, sizeof(diag),
                          "profile=%s FAILED no-stock primary=%s fallback=%s",
                          profileName, resolution.primary, resolution.fallback);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "settings-profile", 0, 0, diag);
            return false;
        }
        std::snprintf(source, sizeof(source), "%s", resolution.fallback);
    } else {
        std::snprintf(source, sizeof(source), "%s", resolution.primary);
    }
    // THE BASE PROFILE: the one the quality tier selected, when the tier
    // selected one.  It lives in the same directory, so the path is the
    // stock path with a different leaf.
    std::string baseBytes = original;
    char baseSource[0x600]{};
    std::snprintf(baseSource, sizeof(baseSource), "%s", source);
    if (gProfileSettings.settings.profile[0] != '\0') {
        const std::string wanted =
            std::string(gProfileSettings.settings.profile) + ".plist";
        char candidate[0x600]{};
        bool got = false;
        const char* const sources[2] = {resolution.primary, resolution.fallback};
        for (int i = 0; i < 2 && !got; ++i) {
            if (sources[i] == nullptr || sources[i][0] == '\0') {
                continue;
            }
            if (!replaceBasename(sources[i], wanted.c_str(), candidate,
                                 sizeof(candidate))) {
                continue;
            }
            if (readWholeFile(candidate, baseBytes)) {
                std::snprintf(baseSource, sizeof(baseSource), "%s", candidate);
                got = true;
            }
        }
        if (!got) {
            char diag[640];
            std::snprintf(diag, sizeof(diag),
                          "profile=%s FAILED tier=%s names a profile that is not "
                          "here (looked for %s)",
                          profileName, gProfileSettings.settings.profile,
                          wanted.c_str());
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "settings-profile", 0, 0, diag);
            return false;
        }
    }
    // THE TRANSLATION.
    const std::vector<unsigned char> requested(original.begin(), original.end());
    const std::vector<unsigned char> base(baseBytes.begin(), baseBytes.end());
    std::string composed;
    GuestSettingsResult result;
    if (!guestSettingsComposeProfile(gProfileSettings.settings,
                                     requested.data(), requested.size(),
                                     base.data(), base.size(), composed,
                                     result)) {
        char diag[640];
        std::snprintf(diag, sizeof(diag),
                      "profile=%s FAILED compose error=%s", profileName,
                      result.error);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "settings-profile", 0, 0, diag);
        return false;
    }
    // WRITE IT.  Through a temporary name, and remove the target first:
    // libnx's fsdev rename does NOT overwrite an existing file.
    char generated[0x600]{};
    std::snprintf(generated, sizeof(generated), "%s/eds-%s", kProfileCacheRoot,
                  profileName);
    char tempPath[0x600]{};
    std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", generated);
    std::FILE* output = std::fopen(tempPath, "wb");
    bool written = false;
    int writeErrno = 0;
    if (output != nullptr) {
        const size_t bytes =
            composed.empty() ? 0u
                             : std::fwrite(composed.data(), 1, composed.size(),
                                           output);
        const bool flushed = (bytes == composed.size());
        const int closeResult = std::fclose(output);
        writeErrno = errno;
        written = flushed && (closeResult == 0);
        if (written) {
            std::remove(generated);
            written = std::rename(tempPath, generated) == 0;
            if (!written) {
                writeErrno = errno;
            } else {
                // THE CARD NEEDS TIME.  See waitForCardFile(): the file lands
                // on the card long after the write returns, and the guest
                // reads this profile during boot.
                const bool landed =
                    waitForCardFile(generated, composed.size(), 30000u);
                if (!landed) {
                    writeErrno = ETIMEDOUT;
                    written = false;
                }
            }
        }
    } else {
        writeErrno = errno;
    }
    char diag[1400];
    std::snprintf(diag, sizeof(diag),
                  "profile=%s tier=%s base=%s stock=%s out=%s bytes=%zu/%zu "
                  "changed=%d written=%d errno=%d error=%s",
                  profileName,
                  gProfileSettings.settings.tier[0] != '\0'
                      ? gProfileSettings.settings.tier
                      : "-",
                  baseSource, source, generated, composed.size(),
                  requested.size(), result.changed, written ? 1 : 0, writeErrno,
                  result.error[0] != '\0' ? result.error : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "settings-profile", 0, 0, diag);
    if (!written) {
        return false;
    }
    std::snprintf(gProfileSettings.builtFor, sizeof(gProfileSettings.builtFor),
                  "%s", profileName);
    std::snprintf(gProfileSettings.generated, sizeof(gProfileSettings.generated),
                  "%s", generated);
    std::snprintf(gProfileSettings.source, sizeof(gProfileSettings.source),
                  "%s", source);
    gProfileSettings.changed = result.changed;
    return true;
}

// THE HOOK.  Called from resolvePath(), which is the one place every path
// takes -- so the guest's read, the guest's stat and the asset list's md5
// all see the SAME file.
bool maybeSubstituteDeviceProfile(const char* guestPath, Resolution& resolution) {
    if (!hasPlistLeaf(guestPath)) {
        // Not a plist at all: the common case, and quiet.
        return false;
    }
    // IT IS A PLIST.  From here on every outcome is NAMED and written down,
    // because a hook that says nothing when it does not fire cannot be told
    // apart from "the guest never asked for it" -- and that is exactly the
    // question a failed substitution raises.
    static unsigned s_reported = 0;
    const char* why = "substituted";
    bool substituted = false;
    if (gBuildingProfile) {
        // The build resolves the profile path itself.
        why = "building-the-profile";
    } else if (!parentDirectoryIsEds(guestPath)) {
        why = "parent-is-not-eds";
    } else if (!loadProfileSettings()) {
        why = "no-settings";
    } else {
        const char* base = pathBasename(guestPath);
        const char* target = profileTargetName();
        // ONLY THE TARGET PROFILE IS SUBSTITUTED.
        //
        // The volume carries a profile for EVERY device the guest knows
        // (AndroidHigh, iPad2,1, iPhone4,1, ...), and the asset list names
        // all of them with a digest.  A port that rewrote every one of them
        // would change the bytes behind forty published digests to fix one
        // device's settings -- and the guest reads the OTHER profiles too.
        //
        // So the settings apply to the profile this port builds (the one the
        // guest asks for by name), and every other profile is served exactly
        // as it shipped.
        if (base == nullptr || base[0] == '\0') {
            why = "no-leaf-name";
        } else if (std::strcmp(base, target) != 0) {
            why = "not-the-target-profile";
        } else if (gProfileSettings.generated[0] == '\0') {
            why = "no-generated-file";
        } else {
            std::snprintf(resolution.primary, sizeof(resolution.primary),
                          "%s", gProfileSettings.generated);
            substituted = true;
        }
    }
    if (s_reported < 12) {
        ++s_reported;
        char diag[900];
        std::snprintf(diag, sizeof(diag),
                      "why=%s substituted=%d guest=%.240s primary=%.240s "
                      "generated=%s",
                      why, substituted ? 1 : 0,
                      guestPath != nullptr ? guestPath : "-", resolution.primary,
                      gProfileSettings.generated);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "settings-substitute", 0, 0, diag);
    }
    // THE STOCK PROFILE STAYS AS THE FALLBACK.  A card that cannot be
    // written then costs the settings and nothing else: the guest still
    // boots with the profile it shipped with.
    return substituted;
}

// Builds the profile at boot, before the asset lists are rewritten.
//
// THE ORDER MATTERS.  The asset lists publish an md5 for every path they
// name, and the guest checks the bytes it receives against it -- so the
// profile has to be the GENERATED one BEFORE the lists are built, or the
// list would publish the digest of the stock profile for a file the port
// serves differently.
void buildDeviceProfileFromSettings() {
    if (!loadProfileSettings()) {
        char diag[256];
        std::snprintf(diag, sizeof(diag),
                      "no settings: the stock device profile is served unchanged");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "settings-profile", 0, 0, diag);
        return;
    }
    buildDeviceProfile(profileTargetName());
}

}  // namespace

// Reads one asset list THE WAY THE PORT SERVES IT, and scans it against the
// override set.
//
// "THE WAY THE PORT SERVES IT" is the whole point: the census answers "what
// does the GUEST get", so the list has to be read through the same resolver
// and the same fallback chain the guest's own read goes through -- which
// means the rewritten copies come back rewritten, and an unre-written list
// comes back exactly as the volume ships it.
bool scanServedAssetList(
    const char* guestPath,
    const std::vector<std::pair<std::string, std::string>>& overrides,
    ListScan& scan, char* servedOut, size_t servedOutSize) {
    if (guestPath == nullptr) {
        return false;
    }
    if (servedOut != nullptr && servedOutSize > 0) {
        servedOut[0] = '\0';
    }
    Resolution resolution{};
    if (!resolvePath(guestPath, false, resolution)) {
        return false;
    }
    std::string text;
    char served[0x600]{};
    const int rc = withReadFallback(
        resolution, guestPath,
        [&text, &served](const char* candidate) -> int {
            // THE BULK PATH FIRST. The base list is 4,212,795 B, and libnx's
            // ordinary read path asks the FS service for the whole size in
            // one request, is refused, and falls back to 4 KiB pieces --
            // ~1029 service round trips for one file. The bulk path reads it
            // in pieces the service serves in one round trip each, and hands
            // back a memory-backed stream.
            std::FILE* file = nullptr;
            if (!bulkOpenFile(candidate, "rb", &file)) {
                file = std::fopen(candidate, "rb");
            }
            if (file == nullptr) {
                return -1;
            }
            if (std::fseek(file, 0, SEEK_END) != 0) {
                std::fclose(file);
                return -1;
            }
            const long size = std::ftell(file);
            if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                std::fclose(file);
                return -1;
            }
            text.assign(static_cast<size_t>(size), '\0');
            const size_t got = (size > 0)
                ? std::fread(&text[0], 1, static_cast<size_t>(size), file)
                : 0u;
            std::fclose(file);
            if (got != static_cast<size_t>(size)) {
                text.clear();
                return -1;
            }
            std::snprintf(served, sizeof(served), "%s", candidate);
            return 0;
        });
    if (rc != 0) {
        return false;
    }
    scanAssetList(text, overrides, scan);
    if (servedOut != nullptr && servedOutSize > 0) {
        std::snprintf(servedOut, servedOutSize, "%s", served);
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE DIGEST CENSUS: does any list the guest reads still publish a digest
// the port does not serve?
//
// THE QUESTION. The guest merges the lists it reads into one table, and a
// path that two lists describe with two DIFFERENT digests is dropped by the
// merge -- so the guest ends up with no digest at all for that asset and
// answers "didn't match its MD5" without ever comparing anything. The
// rewrite makes the lists the port CONTROLS agree; this census says whether
// a list the port does NOT control still disagrees.
//
// IT IS A SCANNER, SO IT CARRIES A POSITIVE CONTROL. The lists the port
// rewrites are scanned with it, and those MUST agree with the override set
// on every path they name -- so a census that finds them disagreeing, or
// that finds no agreement at all while the override set is not empty, is a
// broken scanner and not a finding.
//
// The lists are a FIXED table, not a directory walk: the census must not
// depend on being able to enumerate a read-only volume, and it must not
// read anything the guest does not read. The INVENTORY below is the other
// half -- it walks the volume and names every list it ships.
//
// EVERY LIST IS READ THE WAY THE PORT SERVES IT, so what the census
// reports is what the GUEST gets, not what the factory shipped.
// ---------------------------------------------------------------------------
void censusAssetListDigests() {
    const std::vector<std::pair<std::string, std::string>>& overrides =
        sharedAssetOverrides();

    struct CensusTarget {
        const char* label;
        const char* path;
        // TRUE when the port rewrites this list, so it MUST agree with the
        // override set on every path it names.
        bool controlled;
    };
    static const CensusTarget kTargets[] = {
        { "base",     "files/.depot/asset_list_base.txtCache.txt",     true  },
        { "gui",      "files/apk/res/asset_list_base_gui_assets.txt",  true  },
        { "core",     "files/apk/res/asset_list_gui_assets_core.txt",  true  },
        { "updates",  "files/.depot/asset_list_updates.txt",           false },
        { "base-apkres", "files/apk/res/asset_list_base.txt",          false },
    };

    ListScan guiScan{};
    bool guiSeen = false;
    for (const CensusTarget& target : kTargets) {
        ListScan scan{};
        char served[0x600]{};
        if (!scanServedAssetList(target.path, overrides, scan, served,
                                 sizeof(served))) {
            char diag[900];
            std::snprintf(diag, sizeof(diag),
                          "list=%s path=%s read=0 overrides=%zu",
                          target.label, target.path, overrides.size());
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "assetlist-census", 0, 0, diag);
            continue;
        }
        if (std::strcmp(target.label, "gui") == 0) {
            guiScan = scan;
            guiSeen = true;
        }
        char diag[1700];
        std::snprintf(diag, sizeof(diag),
                      "list=%s path=%s served=%s read=1 entries=%u rewritten=%u "
                      "kept=%u disagree=%u atlas_entries=%u atlas_rewritten=%u "
                      "atlas_disagree=%u overrides=%zu control=%s",
                      target.label, target.path, served, scan.entries,
                      scan.rewritten, scan.kept, scan.disagree,
                      scan.atlasEntries, scan.atlasRewritten, scan.atlasDisagree,
                      overrides.size(),
                      target.controlled
                          ? ((scan.disagree == 0) ? "MUST_AGREE-OK" : "MUST_AGREE-BROKEN")
                          : "REPORT");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-census", 0, 0, diag);
        if (scan.firstDisagree[0] != '\0') {
            char mismatch[900];
            std::snprintf(mismatch, sizeof(mismatch),
                          "list=%s asset=%s published=%s port=%s",
                          target.label, scan.firstDisagree,
                          scan.disagreePublished, scan.disagreePort);
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "assetlist-census-mismatch", 0, 0, mismatch);
        }
    }

    // THE POSITIVE CONTROL, on its own line, so that a census with no
    // findings is still distinguishable from a census that did not run.
    //
    // The rewritten per-category list was BUILT from the override set, so
    // the paths it names that the override set holds carry the override
    // set's digest. The control counts those agreements: a count of ZERO
    // while the override set is not empty means the scanner did not look,
    // not that the lists agree.
    const char* status = "NO-OVERRIDE-HIT";
    if (!guiSeen) {
        status = "LIST-UNREADABLE";
    }
    else if (overrides.empty()) {
        status = "NO-OVERRIDES";
    }
    else if (guiScan.rewritten == 0) {
        status = "NO-OVERRIDE-HIT";
    }
    else if (guiScan.disagree != 0) {
        status = "BROKEN";
    }
    else {
        status = "OK";
    }
    char control[1200];
    std::snprintf(control, sizeof(control),
                  "status=%s overrides=%zu control_agreements=%u "
                  "control_disagreements=%u atlas_agreements=%u "
                  "atlas_disagreements=%u",
                  status, overrides.size(),
                  (guiScan.rewritten > guiScan.disagree)
                      ? guiScan.rewritten - guiScan.disagree : 0u,
                  guiScan.disagree,
                  (guiScan.atlasRewritten > guiScan.atlasDisagree)
                      ? guiScan.atlasRewritten - guiScan.atlasDisagree : 0u,
                  guiScan.atlasDisagree);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-census-control", 0, 0, control);

    // THE ATLAS CONTROL, on its own line.
    //
    // The census above counts every entry the override set covers, and it
    // stayed green over 40,848 kept entries while 125 atlases were never
    // opened. So the ATLAS entries are counted on their own, and ONE of them
    // is checked BY NAME: the digest the list publishes for it, the digest
    // the port serves for it, and the SAME comparison with one hex digit
    // changed, which MUST disagree.
    //
    // A count of ZERO atlas entries means the scanner did not look at the
    // paths the guest failed on -- not that they agree.
    {
        const char* atlasStatus = "NO-ATLAS-SAMPLE";
        int agree = -1;
        int detect = -1;
        if (guiScan.atlasSample[0] != '\0') {
            agree = digestsEqual(guiScan.atlasSamplePublished,
                                 guiScan.atlasSamplePort) ? 1 : 0;
            // THE DETECT CONTROL: the same comparison with ONE hex digit of
            // the published digest changed. It MUST report a disagreement: a
            // checker that agrees with a deliberately wrong digest cannot
            // report anything, and then "0 disagreements" would mean nothing.
            char corrupted[40];
            std::snprintf(corrupted, sizeof(corrupted), "%s",
                          guiScan.atlasSamplePublished);
            if (corrupted[0] != '\0') {
                corrupted[0] = (corrupted[0] == '0') ? '1' : '0';
            }
            detect = digestsEqual(corrupted, guiScan.atlasSamplePort) ? 1 : 0;
            atlasStatus = (agree == 1 && detect == 0) ? "OK" : "BROKEN";
        }
        else if (guiScan.atlasEntries == 0) {
            atlasStatus = "NO-ATLAS-ENTRIES";
        }
        char atlasDiag[1200];
        std::snprintf(atlasDiag, sizeof(atlasDiag),
                      "status=%s atlas_entries=%u atlas_rewritten=%u "
                      "atlas_disagree=%u sample=%s published=%.40s port=%.40s "
                      "agree=%d detect=%d",
                      atlasStatus, guiScan.atlasEntries, guiScan.atlasRewritten,
                      guiScan.atlasDisagree,
                      (guiScan.atlasSample[0] != '\0') ? guiScan.atlasSample : "none",
                      guiScan.atlasSamplePublished, guiScan.atlasSamplePort,
                      agree, detect);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-census-atlas", 0, 0, atlasDiag);
    }
}

// ---------------------------------------------------------------------------
// THE INVENTORY: every asset list the volume ships under files/apk/res/.
//
// The census above works from a FIXED table, because a census that depends
// on being able to enumerate a read-only volume is a census that can fail
// for a reason of its own. This inventory is the other half: it names EVERY
// asset list the volume ships there, so "is there another list that
// publishes the stale digest" is a question with a list behind it instead of
// a question about a table somebody wrote down.
//
// It is bounded and it cannot fail the boot: a directory that cannot be
// opened is one log line, and the scan stops after a fixed number of lists.
//
// THE POSITIVE CONTROL is the cross-check with the census above: the
// inventory scans the rewritten per-category list too, and the two scanners
// have to report the SAME counts for it. Two scanners that disagree about
// the same file are two scanners, and that is visible in the log.
// ---------------------------------------------------------------------------
void censusAssetListInventory() {
    const std::vector<std::pair<std::string, std::string>>& overrides =
        sharedAssetOverrides();

    Resolution resolution{};
    if (!resolvePath("files/apk/res", false, resolution)) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-inventory", 0, 0,
                            "status=FAILED reason=resolve dir=files/apk/res");
        return;
    }
    DIR* directory = ::opendir(resolution.primary);
    if (directory == nullptr && errno == ENOENT && resolution.fallback[0] != '\0') {
        directory = ::opendir(resolution.fallback);
    }
    if (directory == nullptr) {
        char diag[600];
        std::snprintf(diag, sizeof(diag),
                      "status=FAILED reason=opendir dir=%s errno=%d",
                      resolution.primary, errno);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-inventory", 0, 0, diag);
        return;
    }
    std::vector<std::string> names;
    for (;;) {
        struct dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            break;
        }
        const char* name = entry->d_name;
        if (name == nullptr || name[0] == '.' ||
            std::strncmp(name, "asset_list", 10) != 0) {
            continue;
        }
        names.push_back(name);
    }
    ::closedir(directory);
    std::sort(names.begin(), names.end());

    // THE INVENTORY LINE: every name the volume ships, in one line.
    char inventory[3072]{};
    size_t used = 0;
    for (const std::string& name : names) {
        const int written = std::snprintf(inventory + used,
                                          sizeof(inventory) - used, "%s%s",
                                          used ? " " : "", name.c_str());
        if (written <= 0) {
            break;
        }
        used += static_cast<size_t>(written);
        if (used >= sizeof(inventory) - 1) {
            break;
        }
    }
    char summary[400];
    std::snprintf(summary, sizeof(summary),
                  "status=OK lists=%zu overrides=%zu names=%s",
                  names.size(), overrides.size(), inventory);
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-inventory", 0, 0, summary);

    // AND THEN EVERY ONE OF THEM IS SCANNED, so a list that publishes a
    // digest the port does not serve is found rather than suspected.
    constexpr unsigned kInventoryScanCap = 16;
    constexpr unsigned long long kInventoryScanMaxBytes = 1024ull * 1024ull;
    unsigned scanned = 0;
    unsigned skipped = 0;
    unsigned disagreements = 0;
    for (const std::string& name : names) {
        if (scanned >= kInventoryScanCap) {
            ++skipped;
            continue;
        }
        char guestPath[0x600]{};
        std::snprintf(guestPath, sizeof(guestPath), "files/apk/res/%s",
                      name.c_str());
        // THE SIZE, BEFORE THE READ. A list this big is not worth the boot
        // time, and the big base list is already in the census table above.
        Resolution probe{};
        long long bytes = -1;
        if (resolvePath(guestPath, false, probe)) {
            struct stat info{};
            if (::stat(probe.primary, &info) == 0) {
                bytes = static_cast<long long>(info.st_size);
            }
        }
        if (bytes < 0 ||
            static_cast<unsigned long long>(bytes) > kInventoryScanMaxBytes) {
            ++skipped;
            continue;
        }
        ListScan scan{};
        char served[0x600]{};
        if (!scanServedAssetList(guestPath, overrides, scan, served,
                                 sizeof(served))) {
            continue;
        }
        ++scanned;
        if (scan.disagree == 0) {
            continue;
        }
        ++disagreements;
        char diag[900];
        std::snprintf(diag, sizeof(diag),
                      "list=%s asset=%s published=%s port=%s entries=%u "
                      "rewritten=%u kept=%u disagree=%u bytes=%lld served=%s",
                      name.c_str(), scan.firstDisagree, scan.disagreePublished,
                      scan.disagreePort, scan.entries, scan.rewritten,
                      scan.kept, scan.disagree, bytes, served);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "assetlist-inventory-mismatch", 0, 0, diag);
    }
    char totals[400];
    std::snprintf(totals, sizeof(totals),
                  "status=%s scanned=%u skipped=%u disagreeing_lists=%u "
                  "overrides=%zu",
                  (disagreements == 0) ? "OK" : "DISAGREE", scanned, skipped,
                  disagreements, overrides.size());
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "assetlist-inventory-summary", 0, 0, totals);
}

// The dirent regression guard, public so that the boot line can report it.
//
// It depends on nothing but the converter and the real header: a known host
// entry goes in, and the bytes are read back at the offsets the GUEST reads
// (d_type at 0x12, d_name at 0x13), while the HOST's own fields are checked
// at the offsets they really have (d_type at 2, d_name at 3 -- a two-byte
// ino_t, MEASURED with this toolchain, not remembered). It needs no volume,
// no deployed data and no guest run, so a future edit that shifts a field
// fails here instead of silently making every directory entry invisible to
// the guest again.
bool bionicDirentLayoutSelfTest() {
    // 1. THE HOST LAYOUT, at the offsets MEASURED with this toolchain:
    //    d_type at 2, d_name at 3, and a TWO-BYTE ino_t. This is the half
    //    the old version of this test got wrong: it looked for the host's
    //    fields at 0x08/0x09, which is where they would be if ino_t were 8
    //    bytes. They are not, so the old check was reading bytes of d_ino
    //    and passed for the wrong reason.
    //
    //    The static_asserts next to BionicDirent already fail the BUILD if
    //    this layout moves. This test checks the same thing at RUN time, on
    //    the bytes themselves rather than on the compiler's word, so the
    //    boot line reports the layout that is actually in force.
    struct dirent host{};
    host.d_ino = 0x0123456789abULL;
    host.d_type = DT_DIR;
    std::strcpy(host.d_name, "480x320");
    unsigned char hostRaw[sizeof(struct dirent)];
    std::memcpy(hostRaw, &host, sizeof(hostRaw));
    if (hostRaw[2] != DT_DIR) {
        return false;
    }
    if (std::strcmp(reinterpret_cast<const char*>(hostRaw + 3), "480x320") != 0) {
        return false;
    }
    // 2. THE CONVERTED RECORD, at the offsets the GUEST reads: d_type at
    //    0x12 and d_name at 0x13.
    BionicDirent converted{};
    writeBionicDirent(host, converted);
    unsigned char raw[sizeof(BionicDirent)];
    std::memcpy(raw, &converted, sizeof(raw));
    if (raw[0x12] != DT_DIR) {
        return false;
    }
    if (std::strcmp(reinterpret_cast<const char*>(raw + 0x13), "480x320") != 0) {
        return false;
    }
    // The ino_t is 2 bytes on the host, so the value that goes in is the low
    // 16 bits of what was assigned -- 0x9ab -- and the converter widens it
    // to the guest's 64-bit d_ino. Asserting the WIDENED value is what
    // makes this a test of the conversion rather than of the assignment.
    if (converted.d_ino != 0x9abULL) {
        return false;
    }
    // 3. THE TWO LAYOUTS MUST DISAGREE, or the conversion is a no-op. The
    //    host's fields are at 2 and 3; the guest's are at 0x12 and 0x13.
    //    Read the host's offsets out of the CONVERTED record: they must NOT
    //    look like the host's entry.
    if (raw[2] == DT_DIR &&
        std::strcmp(reinterpret_cast<const char*>(raw + 3), "480x320") == 0) {
        return false;
    }
    // 4. AND THE CONVERSION MUST ACTUALLY MOVE THE NAME. The guest's d_name
    //    offset (0x13) minus the host's (3) is 16: that is the 16 bytes the
    //    guest used to lose off every name, and it is the number the
    //    track-name measurements were explained with.
    if (offsetof(BionicDirent, d_name) - offsetof(struct dirent, d_name) != 16) {
        return false;
    }
    return true;
}

ExternalDataStatus mountExternalData() {
    fsTrace("[rr3fs] enter\n");
    unmountExternalData();
    gStatus = {};
    gStatus.attempted = true;
    fsTrace("[rr3fs] status reset\n");
    gStatus.saveReady = ensureWritableRoots();
    gStatus.cacheReady = gStatus.saveReady;
    fsTrace("[rr3fs] writable roots checked\n");

    char looseSentinel[0x400]{};
    std::snprintf(looseSentinel, sizeof(looseSentinel),
                  "%s/files/.depot/langs.utf8", kLooseDataRoot);
    const bool forceLoose = pathExists(
        "sdmc:/switch/rr3-native-rehost/debug-force-loose.flag");
    fsTrace(forceLoose ? "[rr3fs] force loose=yes\n" : "[rr3fs] force loose=no\n");
    if (forceLoose) {
        gStatus.looseFallback = true;
        gStatus.mountedVolumes = kExternalDataVolumeCount;
        gStatus.ready = gStatus.saveReady && gStatus.cacheReady;
        fsTrace("[rr3fs] forced loose ready\n");
        return gStatus;
    }
    if (pathExists(looseSentinel)) {
        fsTrace("[rr3fs] loose scan begin\n");
        constexpr const char* kLooseChecks[] = {
            "files/.depot/gui_assets/2048x1536",
            "files/.depot/gui_assets/960x640",
            "files/.depot/Tracks/Menu",
            "files/.depot/vehicles/2013_bentley_cgtspeed",
            "files/.depot/vehicles/2018_porsche_911_gt2_rs",
            "files/.depot/vehicles/2024_toyota_yaris_wrc",
        };
        gStatus.looseFallback = true;
        gStatus.mountedVolumes = 1;
        for (const char* relative : kLooseChecks) {
            char check[0x500]{};
            std::snprintf(check, sizeof(check), "%s/%s", kLooseDataRoot, relative);
            if (pathExists(check)) ++gStatus.mountedVolumes;
        }
        gStatus.ready = gStatus.mountedVolumes == kExternalDataVolumeCount &&
            gStatus.saveReady && gStatus.cacheReady;
        if (!gStatus.ready) {
            std::snprintf(gStatus.error, sizeof(gStatus.error),
                          "loose fallback incomplete (%u/%u)",
                          gStatus.mountedVolumes,
                          static_cast<unsigned>(kExternalDataVolumeCount));
        }
        fsTrace("[rr3fs] loose scan end\n");
        return gStatus;
    }

    for (size_t index = 0; index < kExternalDataVolumeCount; ++index) {
        fsTrace("[rr3fs] volume mount call\n");
        char path[0x400]{};
        std::snprintf(path, sizeof(path), "%s/%s", kDataBase, kVolumes[index].file);
        gStatus.volumeResults[index] =
            romfsMountFromFsdev(path, 0, kVolumes[index].device);
        if (R_SUCCEEDED(gStatus.volumeResults[index])) {
            gMounted[index] = true;
            if (pathExists(kVolumes[index].sentinel)) {
                ++gStatus.mountedVolumes;
            }
            else {
                std::snprintf(gStatus.error, sizeof(gStatus.error),
                              "volume %s mounted but sentinel is missing",
                              kVolumes[index].file);
            }
        }
        else if (gStatus.error[0] == '\0') {
            std::snprintf(gStatus.error, sizeof(gStatus.error),
                          "could not mount %s (0x%08x)", kVolumes[index].file,
                          gStatus.volumeResults[index]);
        }
    }
    gStatus.ready = gStatus.mountedVolumes == kExternalDataVolumeCount &&
        gStatus.saveReady && gStatus.cacheReady;
    if (gStatus.ready) gStatus.error[0] = '\0';
    // Volumes are mounted, so the resolver can now be exercised for real.
    // One bounded line, once per process. Loose mode returns above and skips it.
    logApkResReachabilityOnce();
    // THE CARD'S INSTALLED COPIES, made to BE the bytes the port serves.
    //
    // THIS RUNS FIRST, BEFORE ANY MANIFEST IS BUILT. It used to run last.
    //
    // WHY THE ORDER IS THE WHOLE POINT. The manifests publish the digest of
    // the file the port serves for a path, and the card's installed copy is
    // the FIRST candidate for the "/assets_<res>/..." spelling -- so the
    // override map reads the card's copy and publishes ITS digest. When the
    // repair ran afterwards, the card's copy was REPLACED with the port's
    // authoritative revision, and the digest the manifest had already
    // published described the file that was just replaced:
    //
    //   published  4321352770b573a6b71d57547b2b3885  (24,689 B, the card's
    //             stale installed copy)
    //   served     c72568828875f4e2a6b11f5c944e27e0  (39,199 B, the volume's
    //             revision, which the repair put in its place)
    //
    // The guest asks for the published digest, the endpoint answers with the
    // other one, and the guest's md5 check fails on an asset that was correct
    // a moment earlier. So the repair comes first, and every manifest is
    // built from the card as it will be when the guest runs.
    //
    // It still runs BEFORE the guest starts, and it waits for each repaired
    // file to land on the card (waitForCardFile), because the guest hashes
    // the installed file while it decides what to download. The big lists
    // that follow keep their own waits: the 4.2 MB base list lands on the
    // card ~18.5 s after the write, and buildConsistentBaseList() waits for
    // it before the guest can read it.
    {
        unsigned repaired = 0;
        unsigned checked = 0;
        repairInstalledCopies(&repaired, &checked);
        char diag[256];
        std::snprintf(diag, sizeof(diag), "checked=%u repaired=%u", checked,
                      repaired);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "installed-repair-summary", 0, repaired, diag);
    }
    // THE COMPLETED GUI-ASSETS MANIFEST, built now because the volumes that
    // describe it are mounted and the card is writable.
    //
    // It is built HERE, once, on the boot thread, and not at the first
    // request: the build reads ~26 MB out of the volumes and hashes it, and
    // doing that inside a guest request would stall whichever guest thread
    // happened to ask for the manifest -- and would need a lock to keep two
    // of them from doing it at once.
    //
    // A failure here costs the completion and nothing else: the shipped
    // manifest is still served, and the log line says why.
    gCompletedManifestReady = buildCompletedGuiAssetsManifest();
    // THE LAUNCHER-SIDE SETTINGS LAYER: the guest's own device profile,
    // rebuilt with the settings from the card.
    //
    // It is built HERE, on the boot thread, and BEFORE the asset lists are
    // rewritten, because the lists publish an md5 for every path they name:
    // a list built against the stock profile would publish the stock digest
    // for a file the port serves differently, and the guest would answer
    // "didn't match its MD5" about a file it never even downloaded.
    buildDeviceProfileFromSettings();
    // THE ASSET LIST THE GUEST CHECKS ITS DOWNLOADS AGAINST, made to describe
    // the bytes the port will actually send.
    //
    // MEASURED 2026-10-21, archive 20261021-234544-eden-fsfix3: the md5 the
    // guest asks for is the one in files/.depot/asset_list_updates.txt (45
    // entries), and all 41 requested assets are in that list with the wanted
    // md5 -- 41 of 41. The 4,212,795 B asset_list_base.txtCache.txt names
    // none of them.
    //
    // It is built HERE, on the boot thread, with the volumes already mounted,
    // for the same reason the completed gui-assets manifest is: the build
    // hashes the files it describes, and doing that inside a guest request
    // would stall whichever guest thread happened to ask for the list.
    //
    // A failure here costs the consistency and nothing else: the shipped list
    // is still served, and the log line says why.
    {
        unsigned rewritten = 0;
        unsigned unresolved = 0;
        buildConsistentAssetList(kUpdatesListGuestPath, kConsistentListPath,
                                  &rewritten, &unresolved);
    }
    // THE BASE ASSET LIST, made to describe the bytes the port serves for the
    // assets the guest has asked for.
    //
    // The full rewrite of this list is 9.394 GB of hashing -- 210-420 seconds
    // on Eden -- so it cannot happen at boot. It is done LAZILY, for the
    // assets the guest has actually downloaded, which the port records from
    // the guest's own ".ast" writes. See the note on the base list above for
    // the measurement and for why the update list's 3.894 MB was affordable
    // and this list's 9.394 GB is not.
    {
        unsigned rewritten = 0;
        unsigned unresolved = 0;
        buildConsistentBaseList(&rewritten, &unresolved);
    }
    // THE PER-CATEGORY GUI ASSET LIST, made to describe the bytes the port
    // serves for the assets the guest has asked for.
    //
    // MEASURED 2026-10-23: this list (files/apk/res/
    // asset_list_base_gui_assets.txt, 298,223 B) published the FACTORY digest
    // bba63527235d60c88863fa10e111abc6 for
    // /gui_assets/960x640/common/sprites.android.atlas while the rewritten
    // base list published c72568828875f4e2a6b11f5c944e27e0 for the same
    // path -- the bytes the port actually serves. The guest merges the two
    // lists, the conflict DROPS the entry, and the download check has no
    // digest to compare against: "didn't match its MD5", 24 times.
    //
    // It is rewritten from the SAME override set as the base list, so the two
    // lists publish the SAME digest for a path they both name.
    {
        unsigned rewritten = 0;
        unsigned kept = 0;
        buildConsistentGuiBaseList(&rewritten, &kept);
    }
    // EVERY OTHER ASSET LIST THE PORT SERVES, rewritten from the same set.
    //
    // The guest merges the base list with ~220 per-category lists it reads
    // through the ordinary file path, and it validates every asset against
    // the digest one of those lists publishes. Rewriting the base list alone
    // makes the two sides disagree: MEASURED 2026-10-26, archives
    // 20261026-230107-eden-f1g and 20261026-233247-eden-f1h, 839 and then
    // 844 "didn't match its MD5" messages, the front end never opened, and
    // Eden exited on its own.
    //
    // One set, every list: then no two lists can publish two digests for one
    // path, and the guest's merge cannot drop an entry.
    buildRewrittenAssetLists();
    // THE DIGEST CENSUS: does any list the guest reads still publish a
    // digest the port does not serve?
    //
    // It runs here, after the rewrites, because it is the census of what
    // the GUEST GETS: every list is read the way the port serves it.
    censusAssetListDigests();
    // AND THE INVENTORY of the volume's asset lists, so "is there another
    // list that publishes the stale digest" is answered with a list of
    // lists rather than with a table somebody wrote down.
    censusAssetListInventory();
    // THE DIGEST INVARIANT, checked against the manifests that were just
    // built and the endpoint's own selection for the same paths.
    //
    // It runs here, after the rewrites, because it audits what the GUEST
    // GETS: the digest each manifest publishes against the digest the
    // endpoint would send for the same path.
    auditAssetDigestInvariant();
    return gStatus;
}

void unmountExternalData() {
    for (size_t index = kExternalDataVolumeCount; index-- > 0;) {
        if (gMounted[index]) {
            romfsUnmount(kVolumes[index].device);
            gMounted[index] = false;
        }
    }
}

const ExternalDataStatus& externalDataStatus() { return gStatus; }
bool externalDataReady() { return gStatus.ready; }

uint64_t guestFilesystemSelfTest() {
    uint64_t bits = 0;
    Resolution value{};
    // POSITIVE CONTROL for the asset-list trace.
    //
    // The trace fires only for a path that contains "asset_list", so a run in
    // which the guest never asks for an asset list produces ZERO
    // assetlist-req lines -- and "the guest never asked" and "the trace is
    // broken" would then be the same observation. This one call makes the
    // trace fire at least once per run, so its absence in a later run is a
    // fact about the guest instead of a fact about the instrument.
    //
    // It asserts nothing: the bit is NOT taken from the mask, because the
    // existence of an asset list is not a contract. The path is the one the
    // guest itself uses, so the line is also a sample of the real thing.
    // gResolveApi is set so the line says where it came from.
    {
        const char* const kControlPath = "files/apk/res/asset_list_base.txt";
        gResolveApi = "self-test";
        value = {};
        if (resolvePath(kControlPath, false, value)) {
            struct stat probe{};
            withReadFallback(value, kControlPath,
                             [&probe](const char* candidate) {
                                 return ::stat(candidate, &probe);
                             });
        }
        // resolvePath() logs the failure itself when it refuses the path, so
        // the control produces exactly one assetlist-req line either way.
        // The resolution is dropped afterwards: the assertions below each
        // start from an empty Resolution, and the control's candidates would
        // otherwise survive into the first one.
        value = {};
    }
    if (resolvePath("/storage/emulated/0/Android/data/com.ea.games.r3_row/files/.depot/langs.utf8", false, value) &&
        std::strcmp(value.primary, "rr3core:/files/.depot/langs.utf8") == 0) bits |= 1;
    value = {};
    if (resolvePath(".depot/gui_assets/2048x1536/probe.bin", false, value) &&
        // THE VOLUME'S COPY IS THE PRIMARY: it is the file the guest's own
        // runtime read serves and the file the port publishes the digest of.
        startsWith(value.primary, "rr3gui0:/files/.depot/gui_assets/2048x1536/") &&
        // AND THE CARD'S INSTALLED COPY IS THE FALLBACK, for the assets the
        // volumes do not ship (the live weekly-event tree).
        startsWith(value.fallback, "sdmc:/switch/rr3-native-rehost/.depot/assets_2048x1536/")) {
        bits |= 2;
    }
    value = {};
    // THE TWO SPELLINGS OF ONE LOGICAL ASSET MUST MEET AT ONE FILE.
    //
    // "files/.depot/gui_assets/960x640/X" and "files/.depot/assets_960x640/X"
    // are the same asset, and the port's manifest rewrite, the pack endpoint
    // and the guest's read-back all have to name the same bytes. When they
    // did not, the guest wrote one revision and compared it with another's
    // digest and answered "didn't match its MD5" -- 24 times, on that one
    // asset (archive 20261022-054635-eden-gfsalias14-r1).
    //
    // THE FILE THEY NOW MEET AT IS THE VOLUME'S COPY, because that is the
    // copy the guest's own runtime read serves (the app-root form is not
    // aliased to the card), and the digest the port publishes has to be the
    // digest of the file the guest can actually read back. The card's
    // "assets_<res>" copy is the fallback: it is a STALE revision of the
    // 960x640 atlas (24,689 B, 165 image entries, against the volume's
    // 39,199 B, 248 entries) and it is the only copy of the live weekly-event
    // assets, which no volume ships.
    bool spellingsMeet = false;
    if (resolvePath(".depot/gui_assets/960x640/probe.bin", false, value) &&
        startsWith(value.primary, "rr3gui1:/files/.depot/gui_assets/960x640/") &&
        startsWith(value.fallback, "sdmc:/switch/rr3-native-rehost/.depot/assets_960x640/")) {
        Resolution alias{};
        if (resolvePath(".depot/assets_960x640/probe.bin", false, alias) &&
            // THE SAME FILE, from the other spelling.
            startsWith(alias.primary, "rr3gui1:/files/.depot/gui_assets/960x640/") &&
            startsWith(alias.fallback, "sdmc:/switch/rr3-native-rehost/.depot/assets_960x640/")) {
            spellingsMeet = true;
        }
    }
    // AND A ".ast" -- THE GUEST'S OWN DOWNLOAD CACHE -- IS NOT ALIASED AT ALL.
    //
    // The alias above exists because the volume's "gui_assets/<res>" copy is the
    // COMPLETE revision and the card's "assets_<res>" copy is the game's STALE
    // INSTALLED revision of the same logical asset. A download cache has no
    // volume copy at all -- the seven volumes ship no ".ast" -- so there is no
    // revision to reconcile and the swap can only move the request off the one
    // file that exists.
    //
    // MEASURED 2026-10-27, archive 20261027-121931-eden-f1j: the card's ONLY
    // ".ast" is
    //     <app root>/.depot/assets_480x320/events/map/featured/weekly_events/
    //         lexus_is_f350_catalunya/sprites_0.rgb.pvr.z.ast
    // (65,102 B, md5 d367c6f1b923e6551f2692bfa58b05bd -- the very digest the
    // guest asked for on the wire), and the endpoint could not answer it:
    // "answered=0 reason=no-known-answer-shape detail=no-local-file", with all
    // five candidates tried. The alias had moved the request to the
    // "gui_assets/480x320" spelling, which no card has, and the ".ast" rule
    // had then wiped the fallback -- the card's "assets_480x320" copy, the one
    // that exists. Both ".ast" candidates collapsed onto the same dead path.
    //
    // So the cache must resolve to the CARD, under the spelling the guest
    // named it with, and with no second candidate behind it.
    Resolution cache{};
    const bool cacheKeepsItsSpelling =
        resolvePath(".depot/assets_960x640/probe.ast", false, cache) &&
        std::strcmp(cache.primary,
                    "sdmc:/switch/rr3-native-rehost/.depot/assets_960x640/probe.ast") == 0 &&
        cache.fallback[0] == '\0';
    if (spellingsMeet && cacheKeepsItsSpelling) {
        bits |= 4;
    }
    value = {};
    if (resolvePath("Tracks/Menu/processed/menu_transition.banim", false, value) &&
        startsWith(value.primary, "rr3track:/")) bits |= 8;
    value = {};
    if (resolvePath("vehicles/2013_bentley_cgtspeed/a", false, value) &&
        startsWith(value.primary, "rr3veh0:/")) bits |= 16;
    value = {};
    if (resolvePath("vehicles/2018_porsche_911_gt2_rs/a", false, value) &&
        startsWith(value.primary, "rr3veh1:/")) bits |= 32;
    value = {};
    if (resolvePath("vehicles/2024_toyota_yaris_wrc/a", false, value) &&
        startsWith(value.primary, "rr3veh2:/")) bits |= 64;
    value = {};
    if (resolvePath("/data/data/com.ea.games.r3_row/files/doc/profile", true, value) &&
        startsWith(value.primary, kSaveRoot)) bits |= 128;
    value = {};
    if (resolvePath("/data/user/0/com.ea.games.r3_row/cache/item", true, value) &&
        startsWith(value.primary, kCacheRoot)) bits |= 256;
    value = {};
    // Regression guard for the leaf-scoped save exclusion added 2026-10-04.
    //
    // Asserted as a NEGATIVE on purpose. resolvePath's device-path branch
    // (guest_filesystem.cpp:703-708) sets primary to the RAW guest path when
    // mapAppRootToContent declines -- it does not rewrite it to kAppRoot -- so
    // "primary is under kAppRoot" is not a true statement and asserting it
    // blocked auto-boot on the first run (fsTest=fff, mask 0x1fff).
    //
    // The property that actually matters, and the one the bug violated, is that
    // a save backup must not be routed into one of the read-only content
    // volumes. Every volume scheme in this file starts with "rr3"
    // (rr3core: / rr3gui: / rr3track: / rr3veh0..2:) and nothing on the SD side
    // does, so the scheme prefix is the discriminator.
    //
    // BIT 4096, NOT 512. The mask is 0xfff, i.e. bits 0..11, so 512 (= 1<<9) is
    // already the /dev/null assertion two lines below. The first version of
    // this guard used 512 and therefore looked satisfied while asserting
    // nothing; the mask was raised to 0x1fff and auto-boot stopped with
    // fsTest=fff. Bits 0..11 are all taken -- read the whole function before
    // picking one.
    //
    // The path must be spelled the way the GUEST spells it. The guest asks for
    // "sdmc:/switch/rr3-native-rehost/.depot/sv.2.dat.bak" (guest-view.log,
    // api-touch), which is the kAppRoot spelling and the only one that reaches
    // mapAppRootToContent -- the leaf exclusion being guarded lives in there.
    // The first version used the /storage/emulated/0/... spelling, which
    // resolvePath routes through afterPackageFiles instead, so the guard
    // exercised a different branch and reported fsTest=fff twice.
    {
        char saveBak[0x200];
        std::snprintf(saveBak, sizeof(saveBak), "%s/.depot/sv.2.dat.bak", kAppRoot);
        if (resolvePath(saveBak, true, value) &&
            std::strncmp(value.primary, "rr3", 3) != 0) bits |= 4096;
    }
    value = {};
    if (resolvePath("/dev/null", true, value) &&
        std::strcmp(value.primary, "/dev/null") == 0) bits |= 512;
    value = {};
    // TWO REFUSALS, ONE BIT: the resolver must refuse a path it must refuse.
    //
    // (a) A traversal. Unchanged since 2026-10-04.
    //
    // (b) A DOUBLED path -- one that names a device twice, or that repeats
    // the app root. MEASURED, archive 20261022-012740-eden-md5list-base-r3:
    // 60 of them in one run, every one answered with errno=22 (EINVAL) and
    // printed "unknown error stat(22) checking the path '...'". The answer
    // has to be ENOENT, because that is what the guest's other probes of the
    // same asset get, and the guest moves on to the next root. See
    // isDoubledDevicePath().
    {
        const bool traversalRefused =
            !resolvePath("files/.depot/../secret", false, value) && errno == EACCES;
        value = {};
        char doubled[0x600];
        std::snprintf(doubled, sizeof(doubled), "%s/.depot/%s/.depot/vehicles/x",
                      kAppRoot, kAppRoot);
        const bool doubledRefused =
            !resolvePath(doubled, false, value) && errno == ENOENT;
        if (traversalRefused && doubledRefused) bits |= 1024;
    }
    value = {};
    if (resolvePath(".depot/text_cn-Hans.txt", false, value) && value.readOnly) bits |= 2048;
    return bits;
}

// Defined further down, next to kStatBionicMode where the A/B switch and the
// stub list live. Declared here because the path-taking entry points that
// consult it are all above that point in the file.
bool stubSuppressed(const char* path);

std::FILE* guestFopen(const char* path, const char* mode) {
    gResolveApi = "guestFopen";
    if (stubSuppressed(path)) {
        errno = kGuestErrnoENOENT;
        return nullptr;
    }
    const bool writing = mode != nullptr && std::strpbrk(mode, "wa+") != nullptr;
    Resolution resolution{};
    if (!resolvePath(path, writing, resolution)) {
        if (path != nullptr && path[0] != '\0') {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fopen-fail-resolve", 0, 0, path);
        }
        return nullptr;
    }
    if (rejectReadOnlyWrite(resolution, writing)) {
        // Split out of the combined check so the read-only rejection can be
        // told apart from a resolve failure: the resolve failure is already
        // reported by resolvePath(), and reporting it twice would double the
        // asset-list lines for one request.
        if (isAssetListPath(path)) {
            logAssetListRequest(path, resolution, true, -1, errno);
        }
        return nullptr;
    }
    std::FILE* file = nullptr;
    // WHICH CANDIDATE ANSWERED. The resolver builds up to three of them
    // (the volume primary, the volume alias, the SD card) and the caller
    // used to see only the RESULT, so a line could not say which tree served
    // the bytes.
    //
    // THE BUFFER IS THIS FUNCTION'S, NOT withReadFallback's. The third
    // candidate -- the app-local card copy -- is a LOCAL buffer inside
    // withReadFallback, so the pointer it hands back dies with the call.
    // Reading it afterwards was a read of a dead stack slot, and it is read
    // AFTERWARDS: the size probe and the "served=" field of the open trace
    // both come after withReadFallback has returned. So the path is copied
    // out here, while it is still alive, into a buffer this function owns.
    char servedPath[0x600]{};
    const char* served = nullptr;
    if (writing) {
        file = std::fopen(resolution.primary, mode);
        served = (file != nullptr) ? resolution.primary : nullptr;
        if (file != nullptr) {
            // THE ASSET THE GUEST ASKED FOR. A downloaded asset is written to
            // "<asset path>.ast" before the guest checks it, so this is the
            // one place in the port that learns which assets the guest wants
            // -- and the base asset list's md5 for those assets has to
            // describe the bytes the port serves. See the note on the base
            // list above.
            recordAssetPath(path);
        }
        if (isAssetListPath(path)) {
            // The write path does not go through withReadFallback, so the
            // write is traced here instead.
            logAssetListRequest(path, resolution, true,
                                file == nullptr ? -1 : 0, errno);
        }
    } else {
        // Same reason as guestOpen: the read path needs the trailing-".z"
        // retry, and that lives only in withReadFallback. See the long note
        // there -- this is the same 71-failure shape reached through fopen.
        //
        // The bulk read comes FIRST inside the callback, so it applies to
        // whichever candidate answers: the volume copy, the alias, or the
        // card copy. It only takes over for a file big enough to be worth
        // it, and it hands back a memory-backed stream, so the guest's own
        // freads cost nothing.
        const char* servedPointer = nullptr;
        withReadFallback(resolution, path, [&file, mode, path](const char* candidate) {
            // THE PROBE COMES FIRST. For a ".ast" stream -- and for the two
            // known-good control streams -- the port serves the guest from a
            // buffer whose digest it has computed, so the port knows what
            // the guest is about to hash and can say so. Every other file is
            // served exactly as before.
            if (probeAstStream(path, candidate, mode, &file)) {
                return 0;
            }
            if (bulkOpenFile(candidate, mode, &file)) {
                return 0;
            }
            file = std::fopen(candidate, mode);
            return file == nullptr ? -1 : 0;
        }, &servedPointer);
        // Copy the served path out of withReadFallback's frame FIRST. See the
        // note on servedPath above: for the card candidate that pointer is a
        // local of that function, and it is dead by the time anything below
        // runs. Reading it afterwards was a read of a dead stack slot.
        if (servedPointer != nullptr) {
            std::snprintf(servedPath, sizeof(servedPath), "%s", servedPointer);
            served = servedPath;
        }
        // THE DIGEST OF THE BYTES THE GUEST WILL READ ON THIS STREAM.
        //
        // "fopen-asset" reports the SIZE of the file the resolver picked. The
        // guest then hashes what it reads from the stream, so the digest of
        // THAT stream is the number the guest's own md5 check uses. If the
        // two ever disagree, the guest rejects a download the port believes
        // it answered correctly -- and nothing in the port's own read-back
        // would show it, because the read-back reads the file again.
        //
        // IT IS THE SERVED FILE THAT IS HASHED, NOT THE PRIMARY.
        //
        // The first version of this line hashed resolution.primary, which is
        // only the FIRST of up to three candidates: when the alias or the
        // card answered, the line reported the digest of a file the guest
        // never read. MEASURED 2026-10-23: it printed the factory file's
        // digest eb7f95be... for a request the guest actually read as
        // e89f6ac8... -- an instrument that lies about the very thing it
        // exists to measure. A lying instrument is worse than no instrument,
        // so the digest is taken from the path that was SERVED.
        if (file != nullptr && mode != nullptr && std::strchr(mode, 'w') == nullptr &&
            std::strchr(mode, 'a') == nullptr && served != nullptr) {
            char streamDigest[33]{};
            if (digestOfLocalFile(served, streamDigest)) {
                char details[900];
                std::snprintf(details, sizeof(details),
                              "primary=%s served=%s stream_md5=%.32s",
                              resolution.primary,
                              served,
                              streamDigest);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "served-digest", 0, 0, details);
            }
        }
    }
    // THE GUI-ASSET CENSUS. The open is what the guest actually CONSUMED: a
    // key with a stat hit and no open hit is a file the guest found and
    // could not read.
    censusGuiAssetRequest(path, file != nullptr ? 0 : -1, true);
    if (file == nullptr && path != nullptr && path[0] != '\0') {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "fopen-fail", 0, static_cast<unsigned long long>(errno),
                            path);
    }
    else if (file != nullptr && path != nullptr) {
        // Graphics/depot content only — skip save sidecars that also contain ".depot".
        static unsigned s_assetOpens = 0;
        const bool assetLike =
            std::strstr(path, "gui_assets") != nullptr ||
            std::strstr(path, "Tracks/") != nullptr ||
            std::strstr(path, "vehicles/") != nullptr ||
            std::strstr(path, "audio/") != nullptr ||
            std::strstr(path, "MenuTransitions") != nullptr ||
            std::strstr(path, "text_cn") != nullptr ||
            std::strstr(path, "langs.utf8") != nullptr;
        // Caps raised 40/80 -> 1200 on 2026-10-04, and the size of what was
        // opened is now recorded. The question being chased is "which file does
        // the guest parse as JSON and how big is it", and both answers were
        // being discarded: the size was never recorded at all, and the ordinal
        // cap silently truncated the sequence long before the interesting open.
        // A cap that hides the answer is worse than a big log -- the guest log
        // budget is 4000 lines and the whole file is well under it.
        // THE SIZE OF THE FILE THAT WAS ACTUALLY OPENED, AND WHICH ONE IT WAS.
        //
        // The old line stat'ed resolution.primary, so "size=-1" meant "the
        // PRIMARY does not exist" -- NOT "the open failed" -- and "size=N"
        // said nothing about how many bytes the guest read. The primary is
        // only the FIRST of up to three candidates, so the number was about
        // the wrong file whenever an alias or the card answered.
        //
        // "size=" is now the on-disk size of the SERVED candidate. It is
        // still not a byte count the guest read -- the guest may read less,
        // and this probe never sees the read -- and the field name says
        // "size" of a file, not of a transfer.
        struct stat opened{};
        const long long openedSize =
            (served != nullptr && ::stat(served, &opened) == 0)
                ? static_cast<long long>(opened.st_size) : -1LL;
        char odiag[512];
        __builtin_snprintf(odiag, sizeof(odiag), "size=%lld served=%s primary=%s",
                           openedSize,
                           served != nullptr ? served : "-",
                           resolution.primary[0] != '\0' ? resolution.primary : "-");
        if (assetLike && s_assetOpens < 1200) {
            ++s_assetOpens;
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fopen-asset", s_assetOpens, 0, odiag);
        }
        else if (!assetLike) {
            // Skip cloudcell webview noise so the log budget stays available
            // for real menu/sprite opens after hideSplash.
            static unsigned s_otherOpens = 0;
            if (s_otherOpens < 1200 && path != nullptr &&
                std::strstr(path, "rr3-native-rehost") != nullptr &&
                std::strstr(path, "CC_Data") == nullptr) {
                ++s_otherOpens;
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "fopen-other", s_otherOpens, 0, odiag);
            }
        }
    }
    return file;
}

int guestOpen(const char* path, int flags, ...) {
    gResolveApi = "guestOpen";
    if (stubSuppressed(path)) {
        errno = kGuestErrnoENOENT;
        return -1;
    }
    const bool writing = (flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT) != 0 ||
        (flags & O_TRUNC) != 0 || (flags & O_APPEND) != 0;
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    Resolution resolution{};
    if (!resolvePath(path, writing, resolution)) {
        logWatchedResult("guestOpen", path, resolution.primary, -1, errno, -1, 0u);
        return -1;
    }
    if (rejectReadOnlyWrite(resolution, writing)) {
        // Split out of the combined check so the read-only rejection can be
        // told apart from a resolve failure: the resolve failure is already
        // reported by resolvePath(), and reporting it twice would double the
        // asset-list lines for one request.
        if (isAssetListPath(path)) {
            logAssetListRequest(path, resolution, true, -1, errno);
        }
        logWatchedResult("guestOpen", path, resolution.primary, -1, errno, -1, 0u);
        return -1;
    }
    int fd = -1;
    if (writing) {
        fd = (flags & O_CREAT) != 0
            ? ::open(resolution.primary, flags, mode)
            : ::open(resolution.primary, flags);
        if (isAssetListPath(path)) {
            // The write path does not go through withReadFallback, so the
            // write is traced here instead.
            logAssetListRequest(path, resolution, true, fd < 0 ? -1 : 0, errno);
        }
    } else {
        // Route the READ path through withReadFallback, like guestStat,
        // guestAccess and friends. This used to hand-roll primary + fallback,
        // which silently dropped the one thing the read path most needs:
        // the trailing-".z" retry.
        //
        // That is why 71 guest resource loads failed in a row, all of one shape
        // (gui10, archive 20261011-1543*gui10):
        //     mtResourceCache: Failed to load resource file:
        //         vehicles/shaders/fresnel_gloss.rgb.pvr.z
        //     Failed opening file: .../.depot/vehicles/shaders/fresnel_gloss.rgb.pvr.z
        // The guest probes each texture as a FORMAT LIST and the one that ships
        // is the plain one -- core.romfs has /shaders/fresnel_gloss* (26 hits)
        // and the zip has files/.depot/vehicles/shaders/*.rgb.pvr with NO ".z".
        // guestStat found it (that is why the stat hit rate moved when the strip
        // landed); guestOpen never asked, so the guest reported the load as
        // failed and carried on without the texture.
        //
        // Routing was verified correct and is NOT the bug here:
        // "files/.depot/vehicles/shaders/..." resolves to rr3veh0 (contentDevice's
        // vehicle-year rule), and the volume really does carry the tree.
        // The callback must return 0 for success. Returning the fd itself --
        // which is what this did on the first attempt -- makes EVERY successful
        // open look like a failure, because a real fd is >= 1. That is not a
        // subtle regression: gui11 measured open-ok collapsing 77 -> 1 and
        // mmap-file-ok 58 -> 1, with Shaders/rr2 files that had always loaded
        // now reporting "Failed opening file". Same run also proved the .z
        // retry itself works -- mtResourceCache failures went 71 -> 0 and the
        // pixel readback turned from 000000ff to 00ffffff.
        const int rc = withReadFallback(resolution, path, [&fd, flags, mode](const char* candidate) {
            fd = (flags & O_CREAT) != 0
                ? ::open(candidate, flags, mode)
                : ::open(candidate, flags);
            return fd < 0 ? -1 : 0;
        });
        if (rc != 0) {
            fd = -1;
        }
    }
    // THE GUI-ASSET CENSUS, the open side. The guest reads its texture
    // pages through open/mmap, so this is the count of what it really
    // consumed.
    censusGuiAssetRequest(path, fd >= 0 ? 0 : -1, true);
    if (path != nullptr &&
        (std::strstr(path, "Shaders") != nullptr ||
         std::strstr(path, ".vsh") != nullptr ||
         std::strstr(path, ".fsh") != nullptr ||
         std::strstr(path, ".gui") != nullptr ||
         std::strstr(path, ".ttf") != nullptr ||
         std::strstr(path, "data.bin") != nullptr ||
         std::strstr(path, "materials") != nullptr ||
         std::strstr(path, "gui_assets") != nullptr ||
         std::strstr(path, "sprites") != nullptr ||
         std::strstr(path, ".banim") != nullptr ||
         std::strstr(path, ".dds") != nullptr ||
         std::strstr(path, ".png") != nullptr ||
         std::strstr(path, ".plist") != nullptr)) {
        char peek[32]{};
        long size = 0;
        if (fd >= 0) {
            const ssize_t n = ::read(fd, peek, 16);
            const off_t end = ::lseek(fd, 0, SEEK_END);
            size = static_cast<long>(end);
            ::lseek(fd, 0, SEEK_SET);
            char hex[40]{};
            for (int i = 0; i < 16 && i < n; ++i) {
                static const char* kHex = "0123456789abcdef";
                hex[i * 2] = kHex[(static_cast<unsigned char>(peek[i]) >> 4) & 0xf];
                hex[i * 2 + 1] = kHex[static_cast<unsigned char>(peek[i]) & 0xf];
            }
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                fd >= 0 ? "open-ok" : "open-fail",
                                static_cast<unsigned long long>(fd >= 0 ? fd : 0),
                                static_cast<unsigned long long>(size), hex);
        }
        else {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "open-fail", 0,
                                static_cast<unsigned long long>(errno),
                                resolution.primary);
        }
    }
    // Watched sidecars report the fd, the size and the first 16 bytes through
    // the same open-ok / open-fail shape, from a bounded gate of their own.
    // Placed after the asset block so it cannot change what that block sees.
    if (watchedPathClass(path) >= 0) {
        logWatchedOpen(path, resolution, fd);
    }
    // Record which guest path this descriptor came from. bionicMmap only ever
    // receives the descriptor, so without this its probe line would name a
    // number and leave the reader to match it against the guest's own error
    // text by call ordering -- which is a guess, not a key. Best-effort: a
    // failure here must never change what this open returns.
    if (fd >= 0) {
        bionicNoteOpenedFd(fd, resolution.primary);
    }
    return fd;
}

int guestOpen2(const char* path, int flags) {
    gResolveApi = "guestOpen2";
    if ((flags & O_CREAT) != 0) {
        errno = kGuestErrnoEINVAL;
        return -1;
    }
    return guestOpen(path, flags);
}

int guestAccess(const char* path, int mode) {
    gResolveApi = "guestAccess";
    if (stubSuppressed(path)) {
        errno = kGuestErrnoENOENT;
        return -1;
    }
    Resolution resolution{};
    const bool writing = (mode & W_OK) != 0;
    if (!resolvePath(path, writing, resolution) ||
        rejectReadOnlyWrite(resolution, writing)) {
        logWatchedResult("guestAccess", path, resolution.primary, -1, errno, -1, 0u);
        return -1;
    }
    const int rc = withReadFallback(resolution, path,
        [mode](const char* candidate) { return ::access(candidate, mode); });
    logWatchedResult("guestAccess", path, resolution.primary, rc,
                     rc == 0 ? 0 : errno, -1, 0u);
    return rc;
}

// ---------------------------------------------------------------------------
// stat layout probe
// ---------------------------------------------------------------------------
// WHY: the guest calls stat into a buffer on its own stack, then takes its
// mmap length from a fixed offset of that buffer -- ELF 0x12a4a24,
//   ldr x21,[sp,#0x30]
// loaded raw, with no zero test, and passed straight to
//   mmap(NULL, x21, PROT_READ, MAP_SHARED, fd, 0)
// Every file-backed mmap in the mmap probe comes back as reason=zero-length,
// so the value the guest reads at +0x30 is 0 for every file it maps.
//
// guestStat hands the guest buffer straight to the host ::stat, so the buffer
// holds the HOST TOOLCHAIN's struct stat while the guest reads it as a
// BIONIC struct stat64. Those two layouts disagree about where every field
// sits. Which one lands on 0x30 is not answerable from either header alone --
// it depends on what the host toolchain actually wrote into those bytes -- so
// it is measured here, on the real resolved path, and both values are printed
// side by side. Every candidate offset in the file's first 128 bytes is dumped,
// not just 0x30, so the correct offset is identified by measurement instead of
// by picking the one a header suggests.
// Reports what the GUEST will read out of the converted buffer, next to the
// true size, so the two can be compared directly. This is the control for the
// fix: before it, o30_is_size was 0 on every file; after it, it must be 1 on
// every non-empty file, and the mmap probe must stop reporting zero-length.
constexpr unsigned kStatLayoutBudget = 4000;
constexpr unsigned kStatProbeOffsets[] = {0x18, 0x28, 0x30, 0x38, 0x40};
unsigned gStatLayoutLogged = 0;
unsigned gStatLayoutMismatched = 0;

// Distinct paths, so "is the guest making progress or looping" is answerable
// without reading thousands of near-identical lines.
//
// MEASURED 2026-10-19, archive 20261019-165014-rollback2. With the old budget
// of 200 the census read exactly 200 -- the cap, not a fact about the guest --
// and the tail was three consecutive repeats of fresnel_dash_matte_dull.rgb.pvr
// then three of spec_dash_matte_dull.rgb.pvr. Whether the guest was walking
// forward through vehicle assets or spinning on the same two files could not be
// told, because the instrument counted requests and the question is about
// distinct files.
//
// Stored as a 64-bit FNV-1a of the path rather than the path itself: this runs
// on the stat path, and a miss only costs one distinct count, never a wrong
// line. gStatLayoutDistinct is the number to read -- if it is still climbing at
// the end of a run the guest is progressing; if it flatlined, it is looping.
constexpr size_t kStatSeenSlots = 4096;
uint64_t gStatSeen[kStatSeenSlots]{};

unsigned statNoteDistinctPath(const char* path) {
    uint64_t hash = 1469598103934665603ull;
    for (const char* p = path; *p != '\0'; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 1099511628211ull;
    }
    if (hash == 0) {
        hash = 1;
    }
    const size_t slot = static_cast<size_t>(hash % kStatSeenSlots);
    if (gStatSeen[slot] == hash) {
        return 0;
    }
    gStatSeen[slot] = hash;
    return 1;
}

unsigned gStatLayoutDistinct = 0;

void logStatLayout(const char* resolvedPath, const void* guestBuffer,
                   const struct stat& host) {
    const auto trueSize = static_cast<unsigned long long>(host.st_size);
    const auto* bytes = static_cast<const unsigned char*>(guestBuffer);
    unsigned long long values[sizeof(kStatProbeOffsets) / sizeof(*kStatProbeOffsets)];
    for (size_t i = 0; i < sizeof(kStatProbeOffsets) / sizeof(*kStatProbeOffsets); ++i) {
        std::memcpy(&values[i], bytes + kStatProbeOffsets[i], sizeof(unsigned long long));
    }
    const bool o30IsSize = values[2] == trueSize;
    // Never budgeted away when it disagrees: the whole point of the probe is
    // the case where the guest's offset does not hold the size. Repeats of a
    // path already seen are dropped for the same reason the distinct counter
    // exists: N lines for one file says nothing about progress.
    const unsigned fresh = statNoteDistinctPath(resolvedPath);
    gStatLayoutDistinct += fresh;
    if ((fresh != 0 || !o30IsSize) &&
        (gStatLayoutLogged < kStatLayoutBudget || !o30IsSize)) {
        char details[320];
        int n = __builtin_snprintf(details, sizeof(details),
                                   "true=%llu distinct=%u repeat=%u size_at=[", trueSize,
                                   gStatLayoutDistinct, fresh != 0 ? 0u : 1u);
        for (size_t i = 0;
             i < sizeof(kStatProbeOffsets) / sizeof(*kStatProbeOffsets) &&
             n > 0 && n < static_cast<int>(sizeof(details)); ++i) {
            n += __builtin_snprintf(details + n, sizeof(details) - n, "%s%02x=%llu",
                                    i == 0 ? "" : ",", kStatProbeOffsets[i], values[i]);
        }
        if (n > 0 && n < static_cast<int>(sizeof(details))) {
            __builtin_snprintf(details + n, sizeof(details) - n,
                               "] o30_is_size=%d %s", o30IsSize ? 1 : 0, resolvedPath);
        }
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "stat-layout", ++gStatLayoutLogged,
                            gStatLayoutMismatched += o30IsSize ? 0u : 1u, details);
    }
}

// A/B switch for the stat layout, because this change alters what the guest
// reads back and "it looks more correct" is not the same claim as "it is more
// useful on this path".
//   0 = pass the host struct through unchanged (the behaviour that produced
//       reason=zero-length on every file)
//   1 = full bionic layout
//   2 = MINIMAL: leave the host layout alone and additionally place st_size at
//       +0x30, changing exactly one field the guest demonstrably loads.
// Mode 2 was the right discriminator when only st_size was known to matter,
// and it fixed the mmap length problem. But the guest reads a SECOND field
// out of the same buffer and mode 2 leaves it as host-layout garbage.
//
// Measured 2026-10-11 (gui8, archive 20261011-1533*gui8), stat-layout trace:
//     size_at=[18=1791704056, 28=1791704056, 30=6193, 38=1791704056, 40=0]
//     o30_is_size=1
// Offset 0x30 holds the size correctly. Offset 0x18 -- which is st_mode in the
// bionic layout (see the static_assert below) -- holds 1791704056 = 0x6ABCBE98,
// a pointer-shaped value, not 0100644. Its type bits are 0xE000.
//
// And the guest consumes exactly that field. Its own disassembly, recorded at
// the top of this file:
//     0x12a4c08  bl stat
//     0x12a4c0c  cbz w0, ok
//     0x12a4c18  cmp w1, #2            <- ENOENT
//     0x12a4c1c  b.eq fail
//     ok: 0x12a4c44  and w8,w8,#0xf000 ; cmp w8,#4,lsl #12
//
// So the sequence is: stat succeeds, errno is not ENOENT, and then the type
// bits are compared. With 0xE000 that comparison fails and the guest declares
// "LoadReadOnlyMappedFile failed getting full path" -- which is exactly what it
// prints for gui/processed/*.gui and for effects/filters/tone_curves.rgb.pvr.z,
// both of which our own stat answered successfully.
//
// The decisive observation that no path fix could have produced: for those
// files there is NO open-ok and NO open-fail, even though ".gui" and ".dds" are
// both in guestOpen's trace filter. The guest never opened them. It gave up
// between stat and open, which is precisely where this check sits.
//
// Acceptance, fixed before running:
//   gui/processed/*.gui stops printing "unable to load file"  -> mode 1 is right
//   it still does                                                  -> the check
//     is not the gate and the field/offset assumption above is wrong
// The early-boot files that load today (no3g.xml, AndroidHigh.plist,
// 2dflat.vsh, 1.dat, ...) go through a different path -- their mmap lines say
// path=<fd-path-unverified> -- so a regression there would be a separate
// finding, not a reason to distrust this one.
constexpr int kStatBionicMode = 1;

// ---------------------------------------------------------------------------
// The A/B, made runnable without touching the SD card
// ---------------------------------------------------------------------------
// The zero-byte stubs above are the suspected cause of the guest's own
//   Bad config. Corrupted JSON config: unexpected end of input
// but they already exist on any card that has run this port once, so "stop
// creating them" cannot be tested without first deleting files off the user's
// card. Suppressing them at the filesystem boundary tests exactly the same
// thing -- the guest sees them as absent -- with no deletion, and the flag
// makes it a one-line revert.
//
// player_profile.2.dat is deliberately NOT in the list: it is the save, and
// "the config will not parse" and "the save is gone" are different experiments.
// A run that conflates them cannot attribute anything.
//
// Acceptance, fixed before the run:
//   P1  "Corrupted JSON config"                       -> 0
//   P2  "startMusic disabled during download"         -> 0
//   P3  no new ENOENT stall replaces it
//   P4  the five files are still on disk afterwards
//
// RESULT: NULL, 2026-10-04. Archive 20261004-175246-stubab against baseline
// 20261004-174528-restore-evidence, every counter identical:
//
//                        baseline   stubs-hidden
//   Corrupted JSON config      1              1
//   startMusic disabled       2              2
//   LoadReadOnlyMappedFile    6              6
//   stat ENOENT            1133           1133
//   mmap ok                 124            124
//
// Hiding the five files must have turned any access to them into ENOENT, and
// the ENOENT count did not move: the guest was not stat-ing them at all. So the
// original comment's claim that the guest tolerates these being absent is
// CORRECT. What was wrong was only my added assumption that the empty file was
// what "Corrupted JSON config" was about. It is about some other file, and
// these stubs are innocent.
//
// REVERTED to 1, the pre-investigation behaviour and the one the (correct)
// original comment describes:
//   1 = the stubs are created and stubSuppressed() always returns false, so
//       every path API sees them normally.
//   0 = the stubs are NOT created AND every path API reports them as ENOENT.
// 0 was not a no-op relative to 1, which is what made the A/B valid, and is why
// an earlier comment here claiming 0 meant "stubs are created" was wrong.
// The single definition of the macro is near the top of this file, next to
// ensureWritableRoots(), which is its first use.

// True when the path is one of the zero-byte config stand-ins and the A/B says
// the guest must not see it. Matching is on the tail of the path so it cannot
// be triggered by an unrelated directory that happens to contain the name.
bool stubSuppressed(const char* path) {
    if (RR3_TOUCH_STUBS != 0 || path == nullptr) {
        return false;
    }
    static const char* const kLeafs[] = {
        "telemetry_optin.bin", "telemetry_opt_in.bin",
        "ccsn.bin", "pcsf.bin", "rgn.bin",
    };
    for (const char* leaf : kLeafs) {
        const size_t n = std::strlen(leaf);
        if (std::strcmp(path, leaf) == 0) {
            return true;
        }
        const size_t len = std::strlen(path);
        if (len > n && path[len - n - 1] == '/' &&
            std::strcmp(path + len - n, leaf) == 0) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// bionic stat layout
// ---------------------------------------------------------------------------
// The guest calls stat() into a buffer on its own stack and then takes its mmap
// length from a fixed offset of that buffer:
//
//   012a4970  bl  0x1e18d10     ; stat(path, sp)      -- GOT slot -> "stat"
//   012a4a24  ldr  x21,[sp,#0x30]; length, loaded raw, never tested for zero
//   012a4a38  mov  x1,x21
//   012a4a44  bl  0x1e18d30     ; mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0)
//
// So on this ABI the guest's size field is an 8-byte value at +0x30. That is
// not read out of a header: it is what shipping commercial code does on real
// Android hardware, where the same path maps these files successfully. The
// 8-byte width agrees -- the guest uses `ldr x`, not `ldr w`, so the field it
// reads is 64-bit. A survey of all 25 `bl stat` sites in the image reads
// offsets 0x8/0x10/0x18/0x20/0x28/0x30/0x38/0x3c/0x40; every one is a field of
// this layout and none falls outside it.
//
// Handing the host toolchain's struct through unchanged put a zero in that
// field, because the two layouts disagree at every offset. Measured on device
// (stat-layout probe) for a 162-byte file: the guest's +0x30 held 0, so mmap
// returned EINVAL, and the asset pipeline aborted with the guest's own
// "Failed memory-mapping file" message.
struct BionicStat {
    uint64_t st_dev;
    uint64_t reservedPad0;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    int64_t st_atim_sec;
    uint64_t st_atim_nsec;
    int64_t st_mtim_sec;
    uint64_t st_mtim_nsec;
    int64_t st_ctim_sec;
    uint64_t st_ctim_nsec;
    // Sized so this struct is exactly as large as the host struct stat that the
    // old passthrough wrote. Writing MORE bytes than the previous code wrote is
    // an overflow the guest never had to tolerate, and every call site has a
    // buffer sized by whoever wrote the caller, not by us.
    uint64_t reservedTail[1];
};

static_assert(sizeof(BionicStat) == 128, "bionic stat must not exceed the old write size");
static_assert(offsetof(BionicStat, st_ino) == 0x10, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_mode) == 0x18, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_rdev) == 0x28, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_size) == 0x30, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_blksize) == 0x38, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_blocks) == 0x40, "bionic stat layout drifted");
static_assert(offsetof(BionicStat, st_atim_sec) == 0x48, "bionic stat layout drifted");

// Values are read through the host's own field NAMES, so where the host
// toolchain happens to place them is the compiler's problem, not this file's.
void writeBionicStat(const struct stat& source, void* destination) {
    if (kStatBionicMode == 0) {
        std::memcpy(destination, &source, sizeof(source));
        return;
    }
    if (kStatBionicMode == 2) {
        // Host layout, plus st_size where the guest loads it. One field moves.
        std::memcpy(destination, &source, sizeof(source));
        const uint64_t size = static_cast<uint64_t>(source.st_size);
        std::memcpy(static_cast<unsigned char*>(destination) + 0x30, &size, sizeof(size));
        return;
    }
    auto* target = static_cast<BionicStat*>(destination);
    target->st_dev = source.st_dev;
    target->reservedPad0 = 0;
    target->st_ino = source.st_ino;
    target->st_mode = source.st_mode;
    target->st_nlink = source.st_nlink;
    target->st_uid = source.st_uid;
    target->st_gid = source.st_gid;
    target->st_rdev = source.st_rdev;
    target->st_size = static_cast<uint64_t>(source.st_size);
    target->st_blksize = static_cast<uint64_t>(source.st_blksize);
    target->st_blocks = static_cast<uint64_t>(source.st_blocks);
    target->st_atim_sec = source.st_atim.tv_sec;
    target->st_atim_nsec = source.st_atim.tv_nsec;
    target->st_mtim_sec = source.st_mtim.tv_sec;
    target->st_mtim_nsec = source.st_mtim.tv_nsec;
    target->st_ctim_sec = source.st_ctim.tv_sec;
    target->st_ctim_nsec = source.st_ctim.tv_nsec;
    target->reservedTail[0] = 0;
}

int statBionicModeForLog() { return kStatBionicMode; }

// Every guestStat path, with what it resolved to and what came back.
//
// WHY: the guest's own failure message reads "LoadReadOnlyMappedFile failed
// getting full path", and its disassembly shows why that name is misleading.
// The resolver (0x12bf4dc) builds a path, and the predicate it actually
// returns is 0x12a4b60's result and-ed with 1. That function is:
//
//   0x12a4c08  bl  0x1e18d10      <- the `stat` PLT slot, resolved to "stat"
//   0x12a4c0c  cbz w0, ok
//   0x12a4c10  bl  0x1e18b10      <- __errno
//   0x12a4c18  cmp w1, #2         <- ENOENT
//   0x12a4c1c  b.eq fail          <- w20 = 0
//   ok: 0x12a4c44 and w8,w8,#0xf000 ; cmp w8,#4,lsl #12  <- S_IFREG
//
// So "failed getting full path" means "the composed path stat'd ENOENT", and
// the guest DOES come to the host for it -- through this very guestStat. It
// was invisible because logWatchedResult only records the watched path classes
// (hbn / Cloudcell sidecars), and these are ordinary content assets.
//
// Logged under a separate phase with its own budget so it cannot starve the
// sidecar trace, and failures are never budgeted: a stat that returns ENOENT
// is the thing being hunted.
constexpr unsigned kStatPathBudget = 600;
unsigned gStatPathLogged = 0;

void logStatPath(const char* guestPath, const Resolution& resolution,
                 int rc, int error) {
    ++gStatPathLogged;
    if (gStatPathLogged > kStatPathBudget && rc == 0) {
        return;
    }
    char details[384];
    __builtin_snprintf(details, sizeof(details),
                       "rc=%d errno=%d guest='%s' primary='%s' fallback='%s'",
                       rc, error,
                       guestPath != nullptr ? guestPath : "-",
                       resolution.primary[0] != '\0' ? resolution.primary : "-",
                       resolution.fallback[0] != '\0' ? resolution.fallback : "-");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "stat-path", gStatPathLogged,
                        static_cast<unsigned long long>(error), details);
}

// The guest also imports fstat, and it resolves to a passthrough in
// symbol_resolver.cpp that writes the host layout into the guest buffer just
// like the old guestStat did -- so any guest file-size query that goes through
// fstat is still wrong, and the mmap call sites that use fstat instead of stat
// still get length 0. That file is frozen by a byte-for-byte guard, so it
// cannot be corrected here. Recorded rather than silently left.

// A regression guard that depends on nothing but the lines above: build a stat
// with a size no plausible real file would have, convert it, and read the
// bytes back at the offset the guest actually loads. It needs no volume, no
// deployed data and no guest run, so a future edit that shifts a field fails
// here instead of silently returning a zero length to every mmap.
//
// It asserts the property the SHIPPED mode guarantees, not the property of the
// nicest layout. Mode 2 deliberately leaves every other field alone, and a
// guard that demanded the full layout would report mode 2 as broken -- i.e. it
// would be switched off in the configuration that actually ships, which is the
// one place a guard has to work.
bool bionicStatLayoutSelfTest() {
    struct stat host{};
    host.st_size = 0x0123456789abLL;
    host.st_mode = 0100644;
    unsigned char buffer[sizeof(BionicStat)]{};
    writeBionicStat(host, buffer);
    uint64_t at30 = 0;
    std::memcpy(&at30, buffer + 0x30, sizeof(at30));
    // Mode 0 deliberately leaves the host layout in place, so it is excluded
    // here: the guard is a claim about the converting modes, not about mode 0.
    if (kStatBionicMode == 0) return true;
    // Common to every converting mode: the guest must find the size where it
    // loads it. This is the property the whole fix rests on.
    if (at30 != 0x0123456789abULL) return false;
    if (kStatBionicMode == 2) return true;
    const auto* written = reinterpret_cast<const BionicStat*>(buffer);
    return written->st_size == 0x0123456789abULL && written->st_mode == 0100644;
}

// The guest also imports fstat, and it resolves to a passthrough in
// symbol_resolver.cpp that writes the host layout into the guest buffer just
// like the old guestStat did -- so any guest file-size query that goes through
// fstat is still wrong. That file is frozen by a byte-for-byte guard, so it
// cannot be corrected here. Recorded rather than silently left.
// Guest-PC feed. Only three handlers in the port report their guest return
// address (censusNoteImportFrom), so after the guest stops calling JNI the
// census ring goes stale and starts naming a call that has already returned.
// Measured under Eden: svcGetThreadContext3 hands back the READER's own
// registers -- sampler-selftest-b shows pc=0x80cf8a18 lr=0x807c8d44, both
// host-NRO -- so a guest address exists only while our code is on the guest's
// stack. guestStat is the highest-traffic custom path still in use at the
// point the second onViewRenderJNI call stopped (the last guest lines are
// map-app / stat-path), so it is where the ring gets refreshed next.
//
// No namespace qualifier on the declaration: this file is already inside
// `namespace rr3`, and writing `namespace rr3 { ... }` here created rr3::rr3.
void censusNoteImportFrom(const void* returnAddress, const char* symbol);
int guestStat(const char* path, struct stat* output) {
    censusNoteImportFrom(__builtin_return_address(0), "fs:guestStat");
    gResolveApi = "guestStat";
    if (stubSuppressed(path)) {
        errno = kGuestErrnoENOENT;
        return -1;
    }
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) {
        logWatchedResult("guestStat", path, resolution.primary, -1, errno, -1, 0u);
        return -1;
    }
    // The host struct is now a local: it is converted into the guest's buffer
    // rather than written there directly. Passing `output` straight to ::stat
    // is what put st_atim.tv_nsec where the guest reads its file size.
    struct stat host{};
    const int rc = withReadFallback(resolution, path,
        [&host](const char* candidate) { return ::stat(candidate, &host); });
    const bool ok = rc == 0 && output != nullptr;
    if (ok) {
        writeBionicStat(host, output);
        logStatLayout(resolution.primary, output, host);
    }
    logStatPath(path, resolution, rc, rc == 0 ? 0 : errno);
    // THE GUI-ASSET CENSUS. This is the guest's EXISTENCE PROBE, and it is
    // the only place where "the guest asked for this resolution in this
    // format" and "the port could answer it" are both known at once.
    censusGuiAssetRequest(path, rc, false);
    logWatchedResult("guestStat", path, resolution.primary, rc, rc == 0 ? 0 : errno,
                     ok ? static_cast<long long>(host.st_size) : -1LL,
                     ok ? static_cast<unsigned>(host.st_mode) : 0u);
    return rc;
}

int guestLstat(const char* path, struct stat* output) {
    gResolveApi = "guestLstat";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    struct stat host{};
    const int rc = withReadFallback(resolution, path,
        [&host](const char* candidate) { return ::lstat(candidate, &host); });
    if (rc == 0 && output != nullptr) writeBionicStat(host, output);
    return rc;
}

// Counts the EEXIST that mkdiridem=1 converts into success, so the conversion
// is auditable rather than invisible. Reported on the mkdir-eexist line.
unsigned gGuestMkdirEexist = 0;

// Every mkdir the guest makes, with the reason it failed.
//
// The mkdir-eexist line turned out to be the wrong instrument: it never fired
// in the run that mattered (20261019-203213-mkdir1, zero lines), because the
// guest's "[FileManager::CreateDir] Error creating directory: .../CC_Data" is
// NOT an EEXIST from ::mkdir. The failure happens earlier -- resolvePath
// refused the path, or rejectReadOnlyWrite did -- and both of those return
// before mkdir is ever called, so nothing downstream could tell them apart.
//
// Bounded: the guest mkdirs a lot, and this is a diagnostic. The first 64
// outcomes are written; the rest are counted.
constexpr unsigned kMkdirLogBudget = 64;
unsigned gGuestMkdirLogged = 0;

void logMkdirOutcome(const char* path, int rc, int err, const char* why) {
    if (gGuestMkdirLogged >= kMkdirLogBudget) {
        return;
    }
    ++gGuestMkdirLogged;
    char details[256];
    __builtin_snprintf(details, sizeof(details),
                       "rc=%d errno=%d why=%s path=%s", rc, err, why,
                       path != nullptr ? path : "(null)");
    appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                        "mkdir", gGuestMkdirLogged,
                        static_cast<unsigned long long>(err), details);
}

int guestMkdir(const char* path, mode_t mode) {
    gResolveApi = "guestMkdir";
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) {
        logMkdirOutcome(path, -1, 0, "no-resolve");
        return -1;
    }
    const int rc = ::mkdir(resolution.primary, mode);
    const int err = rc != 0 ? errno : 0;
    // "mkdiridem=1": an existing directory is success, not EEXIST.
    //
    // MEASURED 2026-10-19, archive 20261019-170132-distinct. The guest logs
    //     [FileManager::CreateDir] Error creating directory:
    //     sdmc:/switch/rr3-native-rehost/.depot/CC_Data
    // and CC_Data is 55 files that the HOST staged out of the game data packs.
    // On real hardware the game creates that directory itself on first run and
    // mkdir succeeds; here the directory is already there, so the shape the
    // guest sees differs from the one it was written against.
    //
    // Idempotent, not silent: the conversion is counted and reported, and a
    // mkdir failing for any other reason still returns its error.
    if (rc != 0 && err == EEXIST && ::rr3::gfxMkdirIdempotent()) {
        gGuestMkdirEexist++;
        logMkdirOutcome(path, 0, EEXIST, "eexist-as-success");
        return 0;
    }
    logMkdirOutcome(path, rc, err, rc == 0 ? "ok" : "failed");
    return rc;
}

DIR* guestOpendir(const char* path) {
    gResolveApi = "guestOpendir";
    if (stubSuppressed(path)) {
        errno = kGuestErrnoENOENT;
        return nullptr;
    }
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) {
        if (path != nullptr && std::strstr(path, "gui") != nullptr) {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "opendir-fail-resolve", 0, 0, path);
        }
        return nullptr;
    }
    DIR* directory = ::opendir(resolution.primary);
    if (directory == nullptr && errno == ENOENT && resolution.fallback[0] != '\0') {
        directory = ::opendir(resolution.fallback);
    }
    if (isAssetListPath(path)) {
        // A directory request is a request too: the guest can find its asset
        // lists by listing a directory instead of naming a file, and this is
        // the only path-taking entry point that does not go through
        // withReadFallback.
        logAssetListRequest(path, resolution, false,
                            directory != nullptr ? 0 : -1,
                            directory != nullptr ? 0 : errno);
    }
    if (path != nullptr && std::strstr(path, "gui") != nullptr) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            directory != nullptr ? "opendir-ok" : "opendir-fail",
                            0,
                            static_cast<unsigned long long>(directory != nullptr ? 0 : errno),
                            resolution.primary);
        if (directory != nullptr) {
            static char s_lastGuiDir[0x400]{};
            std::snprintf(s_lastGuiDir, sizeof(s_lastGuiDir), "%s", resolution.primary);
            g_lastOpendirPath = s_lastGuiDir;
            // Only the bare gui_assets root, not 960x640 itself.
            const char* leaf = resolution.primary;
            const char* slash = nullptr;
            for (const char* p = resolution.primary; *p; ++p) {
                if (*p == '/') slash = p;
            }
            if (slash != nullptr) leaf = slash + 1;
            // Only the bare gui_assets root, not 960x640 itself.
            g_injectGui2048 = (leaf != nullptr &&
                               std::strcmp(leaf, "gui_assets") == 0);
            g_injectIndex = 0;
            g_guiListingChildren[0] = '\0';
            g_guiListingInjected[0] = '\0';
            g_guiListingOpen = g_injectGui2048;
        }
    }
    return directory;
}

void* guestReaddir(DIR* directory) {
    gResolveApi = "guestReaddir";
    struct dirent* entry = ::readdir(directory);
    if (entry != nullptr && g_injectGui2048 && entry->d_name[0] != '\0') {
        noteGuiListingChild(entry->d_name);
    }
    // THE INJECTION: 2048x1536 ONLY.
    //
    // WHY 2048x1536 AND NOT 1280x720. 2048x1536 is the one resolution the
    // SD volume (rr3gui1) does not list: it lives on the HD volume
    // (rr3gui0), and AndroidHigh.plist selects High -> 2048x1536, so the
    // guest has to be able to SEE it. That reason is unchanged by anything
    // below, so the injection stays.
    //
    // WHY 1280x720 IS GONE. It was injected because "1280x720 is the Switch
    // panel size", and the guest runs at 1280x720 -- but a folder the port
    // invents is a folder the volumes do not ship, and the guest's own
    // banner then shows what that costs. MEASURED on the baseline run
    // (archive 20261023-175553-eden-vptr, guest-view.log L2236-L2242), the
    // guest printed:
    //
    //     Res order:
    //     (1280.000000, 720.000000) - 1.777778 - gui_assets/1280x720/
    //     (960.000000, 640.000000)   - 1.500000 - gui_assets/960x640/
    //     (480.000000, 320.000000)   - 1.500000 - gui_assets/480x320/
    //
    // and it then asked for THAT tree's texture leaves:
    //
    //     .depot/gui_assets/1280x720/splash/sprites_0.dxt.dds.z   rc=-1
    //     .depot/gui_assets/1280x720/splash/sprites_0.etc.dds.z   rc=-1
    //     ... 108 such requests, 0 of them served.
    //
    // The reason it asked for ".dxt.dds.z" and not ".rgb.pvr.z" is that the
    // guest has NO published leaf name for 1280x720: the base asset list
    // names 8,472 gui_assets files and NOT ONE of them is under 1280x720
    // (MEASURED on the card, cache/asset_list_base.txtCache.txt). So the
    // guest falls back to probing the texture formats its GL stack claims
    // (S3TC + ETC1, no PVRTC), and every one of those probes misses,
    // because the volumes ship .rgb.pvr.z and .rgb.pvr.z is not in the
    // probe list.
    //
    // The entry is not the current resolution by itself. MEASURED in the
    // archives from BEFORE the bionic-dirent fix (20261021-165605-eden-
    // assetalias and its three neighbours): the guest printed
    //
    //     Res order:
    //     share) Current res: 1280, 720
    //     original [2] (960.000000, 640.000000) - 1.500000
    //
    // -- an EMPTY "Res order:" next to a printed current resolution. The
    // "Res order:" list is therefore built OUT OF THE LISTING, and the
    // 1280x720 entry in it was this injection and nothing else.
    //
    // WITHOUT THE ENTRY the guest's own table takes over, and it already
    // ranks the resolution the volumes ship first ("actual [2] (960.000000,
    // 640.000000)"), so the guest reads the 960x640 tree and takes its leaf
    // names from the atlas it finds there (texture_formats=".rgb.pvr") --
    // the names the volumes actually ship.
    //
    // The resolution ALIAS is untouched: rewriteGuiResolutionPath() still
    // answers a guest path that names 1280x720 (or 1024x768, 768x576,
    // 1920x1080, 1080p, 720p) from the 960x640 tree, so a guest that asks
    // for 1280x720 ANYWAY still gets served.
    //
    // AND THE INJECTION NO LONGER DUPLICATES A REAL ENTRY: it adds
    // 2048x1536 only when the listing does not already contain it (the core
    // tree really does ship one, and the baseline run showed it twice:
    // guest-view.log L2228 and L2232).
    if (entry == nullptr && g_injectIndex < 1 &&
        g_lastOpendirPath != nullptr &&
        std::strstr(g_lastOpendirPath, "gui_assets") != nullptr) {
        if (!g_injectGui2048 || guiListingHasChild("2048x1536")) {
            g_injectIndex = 1;
            entry = nullptr;
        }
        else {
            ++g_injectIndex;
            std::memset(&g_fakeGuiEntry, 0, sizeof(g_fakeGuiEntry));
            std::strcpy(g_fakeGuiEntry.d_name, "2048x1536");
            g_fakeGuiEntry.d_type = DT_DIR;
            std::snprintf(g_guiListingInjected, sizeof(g_guiListingInjected),
                          "2048x1536");
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "readdir-inject", 0,
                                static_cast<unsigned long long>(DT_DIR),
                                "2048x1536");
            entry = &g_fakeGuiEntry;
        }
    }
    if (entry == nullptr) {
        // The listing is over. Say what it contained, once, so the next run
        // can read the resolution the guest was able to choose from without
        // having to reconstruct it from the readdir lines.
        logGuiAssetsListing();
        return nullptr;
    }
    if (entry->d_name[0] != '\0') {
        static unsigned s_readdirLog = 0;
        // RomFS may report DT_UNKNOWN; guests that filter DT_DIR then skip
        // every child. Recover type via stat on the last gui opendir path.
        //
        // NOTE: the recovery writes the HOST's d_type, at the host's offset
        // 0x08. That is the right place for the host's layout, and the
        // conversion below copies it to the guest's offset 0x12. Recovering
        // it into the guest's offset without converting would have been the
        // same mistake the whole conversion exists to fix.
        if (entry->d_type == DT_UNKNOWN && g_lastOpendirPath != nullptr) {
            char full[0x480]{};
            std::snprintf(full, sizeof(full), "%s/%s", g_lastOpendirPath,
                          entry->d_name);
            struct stat info {};
            if (::stat(full, &info) == 0) {
                entry->d_type = S_ISDIR(info.st_mode) ? DT_DIR
                    : S_ISREG(info.st_mode) ? DT_REG : DT_UNKNOWN;
            }
        }
        if (s_readdirLog < 200) {
            ++s_readdirLog;
            const bool guiDir = g_lastOpendirPath != nullptr &&
                std::strstr(g_lastOpendirPath, "gui_assets") != nullptr;
            if (guiDir || s_readdirLog <= 8) {
                // The numbers logged here are the ones the GUEST will read:
                // the converted d_type and the converted d_name.
                BionicDirent converted{};
                writeBionicDirent(*entry, converted);
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    guiDir ? "readdir-gui" : "readdir",
                                    s_readdirLog,
                                    static_cast<unsigned long long>(converted.d_type),
                                    converted.d_name);
            }
        }
    }
    // Publish the entry in BIONIC's layout. The guest reads d_type at 0x12
    // and d_name at 0x13; the host's entry has d_type at 0x08 and d_name at
    // 0x09, so handing the host's pointer over would have made every entry
    // invisible to the guest. See the BionicDirent comment above.
    BionicDirent& slot =
        g_bionicDirentPool[g_bionicDirentNext.fetch_add(1, std::memory_order_relaxed) %
                           kBionicDirentPoolSize];
    writeBionicDirent(*entry, slot);
    return &slot;
}

long guestPathconf(const char* path, int name) {
    gResolveApi = "guestPathconf";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::pathconf(resolution.primary, name);
}

ssize_t guestReadlink(const char* path, char* output, size_t size) {
    gResolveApi = "guestReadlink";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::readlink(resolution.primary, output, size);
}

char* guestRealpath(const char* path, char* output) {
    gResolveApi = "guestRealpath";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return nullptr;
    return ::realpath(resolution.primary, output);
}

int guestStatvfs(const char* path, struct statvfs* output) {
    gResolveApi = "guestStatvfs";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::statvfs(resolution.primary, output);
}

int guestRemove(const char* path) {
    gResolveApi = "guestRemove";
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) return -1;
    return ::remove(resolution.primary);
}

int guestRename(const char* oldPath, const char* newPath) {
    gResolveApi = "guestRename";
    Resolution oldResolution{};
    Resolution newResolution{};
    if (!resolvePath(oldPath, true, oldResolution) ||
        !resolvePath(newPath, true, newResolution) ||
        rejectReadOnlyWrite(oldResolution, true) ||
        rejectReadOnlyWrite(newResolution, true)) {
        // THE INSTALL FAILED BEFORE THE RENAME. Log it: the guest installs a
        // download by renaming "<asset>.ast" onto "<asset>", and if the two
        // paths do not resolve the installed file stays STALE -- which is
        // exactly what the guest then md5s against the manifest and rejects
        // with "didn't match its MD5".
        char diag[640];
        std::snprintf(diag, sizeof(diag),
                      "resolve-failed old=%s new=%s oldp=%s newp=%s",
                      oldPath != nullptr ? oldPath : "-",
                      newPath != nullptr ? newPath : "-",
                      oldResolution.primary, newResolution.primary);
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "rename", 0, 0, diag);
        return -1;
    }
    // BOTH ARGUMENTS ARE CARD PATHS, AND THAT IS THE POINT.
    //
    // The guest installs a download by renaming "<asset>.ast" onto
    // "<asset>". resolvePath() is called with writing=true, so
    // redirectWriteToAppLocal() has already moved a volume path onto the
    // card -- and the ".ast" rule in resolvePath() has already made the OLD
    // path the card's own ".ast", the very file the guest wrote. So ::rename
    // moves THAT file, and never tries to rename a read-only volume path.
    const int rc = ::rename(oldResolution.primary, newResolution.primary);
    {
        char diag[640];
        std::snprintf(diag, sizeof(diag),
                      "rc=%d errno=%d old=%s new=%s oldg=%s newg=%s",
                      rc, errno, oldResolution.primary, newResolution.primary,
                      oldPath != nullptr ? oldPath : "-",
                      newPath != nullptr ? newPath : "-");
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "rename", 0, (unsigned long long)rc, diag);
    }
    return rc;
}

int guestUnlink(const char* path) {
    gResolveApi = "guestUnlink";
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) return -1;
    return ::unlink(resolution.primary);
}

int guestLink(const char* oldPath, const char* newPath) {
    gResolveApi = "guestLink";
    Resolution oldResolution{};
    Resolution newResolution{};
    if (!resolvePath(oldPath, true, oldResolution) ||
        !resolvePath(newPath, true, newResolution) ||
        rejectReadOnlyWrite(oldResolution, true) ||
        rejectReadOnlyWrite(newResolution, true)) return -1;
    return ::link(oldResolution.primary, newResolution.primary);
}

int guestSymlink(const char* target, const char* linkPath) {
    gResolveApi = "guestSymlink";
    Resolution targetResolution{};
    Resolution linkResolution{};
    if (!resolvePath(target, false, targetResolution) ||
        !resolvePath(linkPath, true, linkResolution) ||
        rejectReadOnlyWrite(linkResolution, true)) return -1;
    return ::symlink(targetResolution.primary, linkResolution.primary);
}

int guestBionicStatfs(const char* path, void* output) {
    gResolveApi = "guestBionicStatfs";
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return bionicStatfs(resolution.primary, output);
}

} // namespace rr3
