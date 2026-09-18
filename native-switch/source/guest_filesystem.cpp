#include "rr3/guest_filesystem.hpp"

#include "rr3/bionic_misc_shims.hpp"
#include "rr3/recovery_guard.hpp"

#include <cerrno>
#include <cstdarg>
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
    static const char* kTouchFiles[] = {
        "sdmc:/switch/rr3-native-rehost/doc/telemetry_optin.bin",
        "sdmc:/switch/rr3-native-rehost/doc/telemetry_opt_in.bin",
        "sdmc:/switch/rr3-native-rehost/doc/ccsn.bin",
        "sdmc:/switch/rr3-native-rehost/doc/pcsf.bin",
        "sdmc:/switch/rr3-native-rehost/doc/rgn.bin",
        "sdmc:/switch/rr3-native-rehost/doc/player_profile.2.dat",
    };
    for (const char* path : kTouchFiles) {
        if (!pathExists(path)) {
            std::FILE* file = std::fopen(path, "wb");
            if (file != nullptr) {
                std::fclose(file);
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
        errno = EACCES;
        return false;
    }
    const int written = std::snprintf(output, outputSize, "%s/%s", root, relative);
    if (written < 0 || static_cast<size_t>(written) >= outputSize) {
        errno = ENAMETOOLONG;
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
    return false;
}

bool writeContentPath(char* output, size_t outputSize, const char* relative) {
    if (containsTraversal(relative)) {
        errno = EACCES;
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
        errno = ENAMETOOLONG;
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
    if (startsWith(rest, "doc/") || startsWith(rest, "save/") ||
        startsWith(rest, "cache/") || startsWith(rest, "data/") ||
        startsWith(rest, "proc/")) {
        return false;
    }
    // Only true save/cloudcell sidecars stay on SD. Do NOT exclude every
    // *.bin — .depot/data.bin is real content.
    if (startsWith(rest, ".depot/CC_Data") ||
        std::strstr(rest, "player_profile") != nullptr) {
        return false;
    }
    char contentRelative[0x500]{};
    if (startsWith(rest, "apk/files/")) {
        std::snprintf(contentRelative, sizeof(contentRelative), "%s", rest + 4);
    }
    else if (startsWith(rest, "apk/res/")) {
        // UnpackAssets writes res under apk/; volumes store it under .depot/.
        std::snprintf(contentRelative, sizeof(contentRelative),
                      "files/.depot/%s", rest + 8);
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
    const bool ok = writeContentPath(resolution.primary, sizeof(resolution.primary),
                                     contentRelative);
    if (ok) {
        appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                            "map-app", 0, 0, resolution.primary);
    }
    return ok;
}

bool resolvePath(const char* path, bool writing, Resolution& resolution) {
    if (path == nullptr || *path == '\0') {
        errno = ENOENT;
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
    if (containsTraversal(path)) {
        errno = EACCES;
        return false;
    }
    // Android content URIs have no Horizon equivalent. Transparent passthrough
    // used to produce a colon path that fopen always failed on with a confusing
    // error; fail closed with ENOENT so the guest treats it as missing data.
    if (startsWith(path, "content://") || startsWith(path, "file://") ||
        startsWith(path, "android.resource://")) {
        errno = ENOENT;
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

template <typename Callback>
int withReadFallback(const Resolution& resolution, Callback callback) {
    int result = callback(resolution.primary);
    if (result != 0 && errno == ENOENT && resolution.fallback[0] != '\0') {
        result = callback(resolution.fallback);
    }
    return result;
}

bool rejectReadOnlyWrite(const Resolution& resolution, bool writing) {
    if (writing && resolution.readOnly) {
        errno = EROFS;
        return true;
    }
    return false;
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
    if (resolvePath("/dev/null", true, value) &&
        std::strcmp(value.primary, "/dev/null") == 0) bits |= 512;
    value = {};
    if (!resolvePath("files/.depot/../secret", false, value) && errno == EACCES) bits |= 1024;
    value = {};
    if (resolvePath(".depot/text_cn-Hans.txt", false, value) && value.readOnly) bits |= 2048;
    return bits;
}

std::FILE* guestFopen(const char* path, const char* mode) {
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
    std::FILE* file = std::fopen(resolution.primary, mode);
    if (file == nullptr && !writing && errno == ENOENT && resolution.fallback[0] != '\0') {
        file = std::fopen(resolution.fallback, mode);
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
        if (assetLike && s_assetOpens < 40) {
            ++s_assetOpens;
            appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                "fopen-asset", s_assetOpens, 0, path);
        }
        else if (!assetLike) {
            // Skip cloudcell webview noise so the log budget stays available
            // for real menu/sprite opens after hideSplash.
            static unsigned s_otherOpens = 0;
            if (s_otherOpens < 80 && path != nullptr &&
                std::strstr(path, "rr3-native-rehost") != nullptr &&
                std::strstr(path, "CC_Data") == nullptr) {
                ++s_otherOpens;
                appendGuestBootLine("sdmc:/switch/rr3-native-rehost.guest-view.log",
                                    "fopen-other", s_otherOpens, 0, path);
            }
        }
    }
    return file;
}

int guestOpen(const char* path, int flags, ...) {
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
        rejectReadOnlyWrite(resolution, writing)) return -1;
    int fd = (flags & O_CREAT) != 0
        ? ::open(resolution.primary, flags, mode)
        : ::open(resolution.primary, flags);
    if (fd < 0 && !writing && errno == ENOENT && resolution.fallback[0] != '\0') {
        fd = ::open(resolution.fallback, flags);
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
    return fd;
}

int guestOpen2(const char* path, int flags) {
    if ((flags & O_CREAT) != 0) {
        errno = EINVAL;
        return -1;
    }
    return guestOpen(path, flags);
}

int guestAccess(const char* path, int mode) {
    Resolution resolution{};
    const bool writing = (mode & W_OK) != 0;
    if (!resolvePath(path, writing, resolution) ||
        rejectReadOnlyWrite(resolution, writing)) return -1;
    return withReadFallback(resolution,
        [mode](const char* candidate) { return ::access(candidate, mode); });
}

int guestStat(const char* path, struct stat* output) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return withReadFallback(resolution,
        [output](const char* candidate) { return ::stat(candidate, output); });
}

int guestLstat(const char* path, struct stat* output) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return withReadFallback(resolution,
        [output](const char* candidate) { return ::lstat(candidate, output); });
}

int guestMkdir(const char* path, mode_t mode) {
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) return -1;
    return ::mkdir(resolution.primary, mode);
}

DIR* guestOpendir(const char* path) {
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
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::pathconf(resolution.primary, name);
}

ssize_t guestReadlink(const char* path, char* output, size_t size) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::readlink(resolution.primary, output, size);
}

char* guestRealpath(const char* path, char* output) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return nullptr;
    return ::realpath(resolution.primary, output);
}

int guestStatvfs(const char* path, struct statvfs* output) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return ::statvfs(resolution.primary, output);
}

int guestRemove(const char* path) {
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) return -1;
    return ::remove(resolution.primary);
}

int guestRename(const char* oldPath, const char* newPath) {
    Resolution oldResolution{};
    Resolution newResolution{};
    if (!resolvePath(oldPath, true, oldResolution) ||
        !resolvePath(newPath, true, newResolution) ||
        rejectReadOnlyWrite(oldResolution, true) ||
        rejectReadOnlyWrite(newResolution, true)) return -1;
    return ::rename(oldResolution.primary, newResolution.primary);
}

int guestUnlink(const char* path) {
    Resolution resolution{};
    if (!resolvePath(path, true, resolution) || rejectReadOnlyWrite(resolution, true)) return -1;
    return ::unlink(resolution.primary);
}

int guestLink(const char* oldPath, const char* newPath) {
    Resolution oldResolution{};
    Resolution newResolution{};
    if (!resolvePath(oldPath, true, oldResolution) ||
        !resolvePath(newPath, true, newResolution) ||
        rejectReadOnlyWrite(oldResolution, true) ||
        rejectReadOnlyWrite(newResolution, true)) return -1;
    return ::link(oldResolution.primary, newResolution.primary);
}

int guestSymlink(const char* target, const char* linkPath) {
    Resolution targetResolution{};
    Resolution linkResolution{};
    if (!resolvePath(target, false, targetResolution) ||
        !resolvePath(linkPath, true, linkResolution) ||
        rejectReadOnlyWrite(linkResolution, true)) return -1;
    return ::symlink(targetResolution.primary, linkResolution.primary);
}

int guestBionicStatfs(const char* path, void* output) {
    Resolution resolution{};
    if (!resolvePath(path, false, resolution)) return -1;
    return bionicStatfs(resolution.primary, output);
}

} // namespace rr3
