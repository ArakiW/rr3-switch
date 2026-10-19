#include "rr3/guest_filesystem.hpp"

#include "rr3/bionic_misc_shims.hpp"
#include "rr3/gfx_backend.hpp"
#include "rr3/recovery_guard.hpp"
#include "bionic_errno.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace rr3 {
namespace {

constexpr const char* kDataBase = "sdmc:/switch/rr3-native-rehost/data";
constexpr const char* kSaveRoot = "sdmc:/switch/rr3-native-rehost/save";
constexpr const char* kCacheRoot = "sdmc:/switch/rr3-native-rehost/cache";
constexpr const char* kLooseDataRoot =
    "sdmc:/switch/rr3-native-rehost/data/loose";
// Last successfully opened gui directory (for DT_UNKNOWN recovery in readdir).
const char* g_lastOpendirPath = nullptr;
// When listing the bare gui_assets folder, synthesize children the guest
// expects. 2048x1536 lives on the HD volume (rr3gui0) and does not appear in
// the SD listing; AndroidHigh.plist selects High → 2048x1536. 1280x720 is
// the Switch panel size. contentDevice already routes 2048x1536 to rr3gui0
// and rewriteGuiResolutionPath sends 1280x720 to 960x640.
bool g_injectGui1280 = false;
bool g_injectGui2048 = false;
int g_injectIndex = 0;
dirent g_fakeGuiEntry{};

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
        "files/.depot/gui_assets/1920x1080",
        "files/.depot/gui_assets/1080p",
        "files/.depot/gui_assets/720p",
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

// getAppPath/getExternalStorageDir both return kAppRoot. Guest then opens
// absolute sdmc: paths under it. isDevicePath used to pass those straight to
// FAT, so files/.depot/gui_assets/** never reached the seven romfs volumes.
// Save trees (doc/, cache/, .depot/CC_Data, *.dat) stay on SD.
bool mapAppRootToContent(const char* path, Resolution& resolution) {
    if (path == nullptr || !startsWith(path, kAppRoot)) {
        return false;
    }
    const char* rest = path + std::strlen(kAppRoot);
    while (*rest == '/') {
        ++rest;
    }
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
    if (ok) {
        if (appResLeaf != nullptr) {
            // The legacy .depot target becomes the second candidate, not the
            // only one, so no name that resolved before stops resolving.
            char alias[0x500]{};
            std::snprintf(alias, sizeof(alias), "files/.depot/%s", appResLeaf);
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

bool resolvePath(const char* path, bool writing, Resolution& resolution) {
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
        if (mapAppRootToContent(path, resolution)) {
            return true;
        }
        std::snprintf(resolution.primary, sizeof(resolution.primary), "%s", path);
        return true;
    }

    if (const char* relative = afterPackageFiles(path)) {
        char contentRelative[0x500]{};
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
        return writeContentPath(resolution.primary, sizeof(resolution.primary), prefixed);
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
        return writeContentPath(resolution.primary, sizeof(resolution.primary), contentRelative);
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

template <typename Callback>
int withReadFallback(const Resolution& resolution, Callback callback) {
    const bool probe = resolution.primary != nullptr &&
                       std::strstr(resolution.primary, kProbeSubtree) != nullptr;
    // Counts the attempts this one logical lookup makes, so the trace can say
    // "3 candidates, last errno=2" instead of leaving the reader to infer it.
    unsigned attempts = 0;
    const auto attempt = [&](const char* candidate) {
        ++attempts;
        const int rc = callback(candidate);
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
    const char* const candidates[2] = {resolution.primary, resolution.fallback};
    int result = -1;
    int firstError = ENOENT;
    for (int i = 0; i < 2; ++i) {
        const char* candidate = candidates[i];
        if (candidate == nullptr || candidate[0] == '\0') {
            continue;
        }
        result = attempt(candidate);
        if (result == 0) {
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
    const char* primary = resolution.primary;
    if (primary != nullptr && std::strncmp(primary, "sdmc:", 5) != 0) {
        const char* rel = std::strstr(primary, ":/files/");
        if (rel != nullptr) {
            rel += 8;   // past ":/files/"
            char card[0x500]{};
            const int n = std::snprintf(card, sizeof(card),
                                       "sdmc:/switch/rr3-native-rehost/%s", rel);
            if (n > 0 && static_cast<size_t>(n) < sizeof(card)) {
                result = callback(card);
                if (result == 0) {
                    return 0;
                }
                // Same retraction as the volume candidate above, and for the
                // same reason: a ".z" request must not be answered with the
                // uncompressed file. The guest inflates unconditionally.
            }
        }
    }
    if (result != 0 && errno == 0) {
        errno = firstError;
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

} // namespace

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
    if (resolvePath("/storage/emulated/0/Android/data/com.ea.games.r3_row/files/.depot/langs.utf8", false, value) &&
        std::strcmp(value.primary, "rr3core:/files/.depot/langs.utf8") == 0) bits |= 1;
    value = {};
    if (resolvePath("files/.depot/gui_assets/2048x1536/test", false, value) &&
        startsWith(value.primary, "rr3gui0:/")) bits |= 2;
    value = {};
    if (resolvePath("files/.depot/gui_assets/960x640/test", false, value) &&
        startsWith(value.primary, "rr3gui1:/")) bits |= 4;
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
    if (!resolvePath("files/.depot/../secret", false, value) && errno == EACCES) bits |= 1024;
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
    if (!resolvePath(path, writing, resolution) ||
        rejectReadOnlyWrite(resolution, writing)) {
        if (path != nullptr && path[0] != '\0') {
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fopen-fail-resolve", 0, 0, path);
        }
        return nullptr;
    }
    std::FILE* file = nullptr;
    if (writing) {
        file = std::fopen(resolution.primary, mode);
    } else {
        // Same reason as guestOpen: the read path needs the trailing-".z"
        // retry, and that lives only in withReadFallback. See the long note
        // there -- this is the same 71-failure shape reached through fopen.
        withReadFallback(resolution, [&file, mode](const char* candidate) {
            file = std::fopen(candidate, mode);
            return file == nullptr ? -1 : 0;
        });
    }
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
        struct stat opened{};
        const long long openedSize =
            ::stat(resolution.primary, &opened) == 0
                ? static_cast<long long>(opened.st_size) : -1LL;
        char odiag[288];
        __builtin_snprintf(odiag, sizeof(odiag), "size=%lld primary=%s",
                           openedSize,
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
    if (!resolvePath(path, writing, resolution) ||
        rejectReadOnlyWrite(resolution, writing)) {
        logWatchedResult("guestOpen", path, resolution.primary, -1, errno, -1, 0u);
        return -1;
    }
    int fd = -1;
    if (writing) {
        fd = (flags & O_CREAT) != 0
            ? ::open(resolution.primary, flags, mode)
            : ::open(resolution.primary, flags);
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
        const int rc = withReadFallback(resolution, [&fd, flags, mode](const char* candidate) {
            fd = (flags & O_CREAT) != 0
                ? ::open(candidate, flags, mode)
                : ::open(candidate, flags);
            return fd < 0 ? -1 : 0;
        });
        if (rc != 0) {
            fd = -1;
        }
    }
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
    const int rc = withReadFallback(resolution,
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
    const int rc = withReadFallback(resolution,
        [&host](const char* candidate) { return ::stat(candidate, &host); });
    const bool ok = rc == 0 && output != nullptr;
    if (ok) {
        writeBionicStat(host, output);
        logStatLayout(resolution.primary, output, host);
    }
    logStatPath(path, resolution, rc, rc == 0 ? 0 : errno);
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
    const int rc = withReadFallback(resolution,
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
            g_injectGui1280 = (leaf != nullptr &&
                               std::strcmp(leaf, "gui_assets") == 0);
            g_injectGui2048 = g_injectGui1280;
            g_injectIndex = 0;
        }
    }
    return directory;
}

struct dirent* guestReaddir(DIR* directory) {
    gResolveApi = "guestReaddir";
    struct dirent* entry = ::readdir(directory);
    if (entry == nullptr && g_injectIndex < 2 &&
        g_lastOpendirPath != nullptr &&
        std::strstr(g_lastOpendirPath, "gui_assets") != nullptr) {
        const char* name = (g_injectIndex == 0) ? "2048x1536" : "1280x720";
        if (g_injectIndex == 0 && !g_injectGui2048) {
            g_injectIndex = 1;
            name = "1280x720";
        }
        if (g_injectIndex == 1 && !g_injectGui1280) {
            g_injectIndex = 2;
            return nullptr;
        }
        ++g_injectIndex;
        std::memset(&g_fakeGuiEntry, 0, sizeof(g_fakeGuiEntry));
        std::strcpy(g_fakeGuiEntry.d_name, name);
        g_fakeGuiEntry.d_type = DT_DIR;
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "readdir-inject", 0,
                            static_cast<unsigned long long>(DT_DIR), name);
        return &g_fakeGuiEntry;
    }
    if (entry != nullptr && entry->d_name[0] != '\0') {
        static unsigned s_readdirLog = 0;
        // RomFS may report DT_UNKNOWN; guests that filter DT_DIR then skip
        // every child. Recover type via stat on the last gui opendir path.
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
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    guiDir ? "readdir-gui" : "readdir",
                                    s_readdirLog,
                                    static_cast<unsigned long long>(entry->d_type),
                                    entry->d_name);
            }
        }
    }
    return entry;
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
        rejectReadOnlyWrite(newResolution, true)) return -1;
    return ::rename(oldResolution.primary, newResolution.primary);
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
