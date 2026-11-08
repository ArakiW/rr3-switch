#include "rr3/native_ui.hpp"
#include "rr3/ui_dashboard.hpp"
#include "rr3/gfx_backend.hpp"
#include "rr3/guest_filesystem.hpp"

#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace rr3 {
namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr uint32_t kAsphalt = RGBA8_MAXALPHA(17, 19, 24);
constexpr uint32_t kCarbon = RGBA8_MAXALPHA(37, 42, 49);
constexpr uint32_t kPorcelain = RGBA8_MAXALPHA(244, 244, 242);
constexpr uint32_t kSilver = RGBA8_MAXALPHA(201, 205, 208);
constexpr uint32_t kSteel = RGBA8_MAXALPHA(135, 150, 174);
constexpr uint32_t kBlue = RGBA8_MAXALPHA(31, 159, 229);
constexpr uint32_t kOrange = RGBA8_MAXALPHA(255, 163, 19);
constexpr uint32_t kGreen = RGBA8_MAXALPHA(107, 207, 72);
constexpr uint32_t kRed = RGBA8_MAXALPHA(237, 61, 61);
// Source: original MenuTransitions.xml. Do not replace these authored windows
// with generic duration/easing values when the game renderer is connected.
constexpr unsigned kOriginalMenuTransitionFirstFrame = 0;
constexpr unsigned kOriginalMenuTransitionLastFrame = 40;
constexpr unsigned kOriginalCarSelectFirstFrame = 0;
constexpr unsigned kOriginalCarSelectLastFrame = 90;

struct UiState {
    Framebuffer framebuffer{};
    FT_Library library{};
    FT_Face regular{};
    FT_Face bold{};
    FT_Face numeric{};
    FT_Face chinese{};
    bool active{};
    bool originalChineseCatalog{};
    bool originalChineseExternal{};
    bool originalEnglishCatalog{};
    bool originalMotionManifest{};
    bool originalMotionTracks{};
    uint32_t* pixels{};
    int stride{};
};

UiState gUi;

bool fileExists(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    std::fclose(file);
    return true;
}

bool fileStartsWith(const char* path, const unsigned char* expected, size_t count) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    unsigned char actual[16]{};
    const bool matched = count <= sizeof(actual) &&
        std::fread(actual, 1, count, file) == count &&
        std::memcmp(actual, expected, count) == 0;
    std::fclose(file);
    return matched;
}

bool fileContainsText(const char* path, const char* needle) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr || needle == nullptr) {
        if (file != nullptr) std::fclose(file);
        return false;
    }
    char content[0x4000]{};
    const size_t bytes = std::fread(content, 1, sizeof(content) - 1, file);
    std::fclose(file);
    content[bytes] = '\0';
    return std::strstr(content, needle) != nullptr;
}

const char* localizedStage(const char* stage) {
    if (stage == nullptr) return "正在载入";
    if (std::strcmp(stage, "Runtime services") == 0) return "初始化系统服务";
    if (std::strcmp(stage, "Main ELF audit") == 0) return "校验原版主程序";
    if (std::strcmp(stage, "libc++ closure") == 0) return "闭合原版 C++ 运行库依赖";
    if (std::strcmp(stage, "Core DSO link and permissions") == 0) return "链接核心模块并设置内存权限";
    if (std::strcmp(stage, "Finalizing dashboard") == 0) return "准备原生界面";
    return stage;
}

uint32_t blend(uint32_t destination, uint32_t source, uint8_t alpha) {
    const unsigned inv = 255U - alpha;
    const unsigned r = ((source & 0xffU) * alpha + (destination & 0xffU) * inv) / 255U;
    const unsigned g = (((source >> 8) & 0xffU) * alpha + ((destination >> 8) & 0xffU) * inv) / 255U;
    const unsigned b = (((source >> 16) & 0xffU) * alpha + ((destination >> 16) & 0xffU) * inv) / 255U;
    return RGBA8_MAXALPHA(r, g, b);
}

void rect(int x, int y, int width, int height, uint32_t color) {
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(kWidth, x + width);
    const int bottom = std::min(kHeight, y + height);
    for (int row = top; row < bottom; ++row) {
        std::fill(gUi.pixels + row * gUi.stride + left,
                  gUi.pixels + row * gUi.stride + right, color);
    }
}

void angled(int x, int y, int width, int height, int cut, uint32_t color) {
    for (int row = 0; row < height; ++row) {
        const int inset = cut * (height - row) / std::max(1, height);
        rect(x + inset, y + row, width - inset, 1, color);
    }
}

uint32_t nextUtf8(const char*& text) {
    const unsigned char first = static_cast<unsigned char>(*text++);
    if (first < 0x80) return first;
    if ((first & 0xe0) == 0xc0) {
        const uint32_t value = ((first & 0x1f) << 6) |
            (static_cast<unsigned char>(*text++) & 0x3f);
        return value;
    }
    if ((first & 0xf0) == 0xe0) {
        const unsigned char second = static_cast<unsigned char>(*text++);
        const unsigned char third = static_cast<unsigned char>(*text++);
        const uint32_t value = ((first & 0x0f) << 12) |
            ((second & 0x3f) << 6) | (third & 0x3f);
        return value;
    }
    return '?';
}

FT_Face faceFor(uint32_t codepoint, bool strong, bool telemetry) {
    if (codepoint >= 0x80 && gUi.chinese != nullptr) return gUi.chinese;
    if (telemetry && gUi.numeric != nullptr) return gUi.numeric;
    return strong && gUi.bold != nullptr ? gUi.bold : gUi.regular;
}

void text(int x, int baseline, const char* value, int size, uint32_t color,
          bool strong = false, bool telemetry = false) {
    if (value == nullptr) return;
    const char* cursor = value;
    while (*cursor != '\0') {
        const uint32_t codepoint = nextUtf8(cursor);
        FT_Face face = faceFor(codepoint, strong, telemetry);
        if (face == nullptr) continue;
        FT_Set_Pixel_Sizes(face, 0, size);
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) continue;
        const FT_GlyphSlot glyph = face->glyph;
        const int gx = x + glyph->bitmap_left;
        const int gy = baseline - glyph->bitmap_top;
        for (unsigned row = 0; row < glyph->bitmap.rows; ++row) {
            const int py = gy + static_cast<int>(row);
            if (py < 0 || py >= kHeight) continue;
            for (unsigned column = 0; column < glyph->bitmap.width; ++column) {
                const int px = gx + static_cast<int>(column);
                if (px < 0 || px >= kWidth) continue;
                const uint8_t alpha = glyph->bitmap.buffer[row * glyph->bitmap.pitch + column];
                if (alpha != 0) {
                    uint32_t& destination = gUi.pixels[py * gUi.stride + px];
                    destination = blend(destination, color, alpha);
                }
            }
        }
        x += static_cast<int>(glyph->advance.x >> 6);
    }
}

void beginFrame() {
    u32 strideBytes = 0;
    gUi.pixels = static_cast<uint32_t*>(framebufferBegin(&gUi.framebuffer, &strideBytes));
    gUi.stride = static_cast<int>(strideBytes / sizeof(uint32_t));
    rect(0, 0, kWidth, kHeight, kAsphalt);
}

void endFrame() {
    framebufferEnd(&gUi.framebuffer);
    gUi.pixels = nullptr;
}

void chrome(const char* section, int page) {
    rect(0, 0, kWidth, 74, kPorcelain);
    angled(0, 0, 410, 74, 32, kBlue);
    text(38, 49, "REAL RACING 3", 36, kPorcelain, true);
    text(438, 44, section, 30, kAsphalt, true);
    text(1110, 44, page == 0 ? "PORT" : "DEV", 22, kAsphalt, true, true);
    rect(0, 74, kWidth, 5, kOrange);
}

void progressBar(int x, int y, int width, unsigned percent) {
    rect(x, y, width, 18, kCarbon);
    rect(x, y, static_cast<int>(width * std::min(percent, 100U) / 100U), 18, kBlue);
    rect(x, y + 18, width, 2, kSilver);
}

void statusRow(int y, const char* label, const char* value, uint32_t color) {
    rect(74, y, 760, 58, kPorcelain);
    angled(834, y, 350, 58, 24, color);
    text(102, y + 39, label, 27, kAsphalt, true);
    text(888, y + 39, value, 25, color == kOrange ? kAsphalt : kPorcelain, true);
}

} // namespace

// ---------------------------------------------------------------------------
// THE PORT'S OWN FILES, WITH THE CARD FALLBACK.
//
// A "clean" build ships no EA material in the RomFS: the guest's faces and
// its .depot text come from the user's own copy of the application, which
// the user puts next to the NRO on the card.  The ROMFS path is therefore
// only the FIRST place to look, and the card is the second.
//
// WHY IT MATTERS, MEASURED: a clean build hung at eglCreateWindowSurface
// and rendered nothing, while the same source with a full RomFS ran fine.
// The difference was this file: nativeUiInit() opened the faces straight
// out of "romfs:/ui/..." with no fallback, and the read of a file that is
// not there is what stopped the port before it ever made a window.
//
// THE SAME CLASS OF BUG, IN ONE PLACE: glyph_vector.cpp already had this
// chain, and native_ui.cpp did not, and the two were written at different
// times.  A rule that lives in one file is not a rule.
// ---------------------------------------------------------------------------
namespace {

// The port's own file, wherever it lives: the RomFS first, the card
// second.
const char* portPath(const char* romfsPath, char* buffer, size_t size) {
    rr3::resolvePortAssetPath(romfsPath, buffer, size);
    return buffer;
}

// Open a face, and say whether it was the face that was asked for or the
// OFL bootstrap one.
//
// The bootstrap face is opened ONCE and shared, so the fallback does not
// open it four times.  It is opened on demand because the fallback is only
// reached when the asked-for face was missing.
bool openFaceOrBootstrap(const char* romfsPath, char* pathBuffer, size_t pathSize,
                          FT_Face* out) {
    if (FT_New_Face(gUi.library, portPath(romfsPath, pathBuffer, pathSize), 0,
                     out) == 0) {
        return true;
    }
    *out = nullptr;
    return false;
}

} // namespace

bool nativeUiInit() {
    if (gUi.active) return true;
    if (FT_Init_FreeType(&gUi.library) != 0) {
        nativeUiExit();
        return false;
    }
    // THE FACES, ONE AT A TIME, WITH A FALLBACK.
    //
    // WHY NOT ONE EXPRESSION: a failure anywhere in it meant the WHOLE
    // dashboard failed, and the port then took main.cpp's diagnostic path --
    // consoleInit(nullptr) -- which binds deko3d to nwindowGetDefault().  The
    // console was never released, so Mesa's later eglCreateWindowSurface
    // blocked on a window the console still owned.  MEASURED 2026-11-09: the
    // clean loader hung exactly there and rendered nothing.
    //
    // So each face is opened on its own, and a face that is not there is
    // replaced by the OFL bootstrap face -- which the clean build always
    // carries, because it is Noto CJK under the SIL OFL and this project owns
    // it.  A missing game face must never be able to take the whole port
    // down the console path.
    char regularPath[512];
    char boldPath[512];
    char numericPath[512];
    char chinesePath[512];
    const bool haveRegular = openFaceOrBootstrap(
        "romfs:/ui/ConduitITCPro-Regular.otf", regularPath, sizeof(regularPath),
        &gUi.regular);
    const bool haveBold = openFaceOrBootstrap(
        "romfs:/ui/ConduitITCPro-Bold.otf", boldPath, sizeof(boldPath),
        &gUi.bold);
    const bool haveNumeric = openFaceOrBootstrap(
        "romfs:/ui/EurostileLTStd-Bold.otf", numericPath, sizeof(numericPath),
        &gUi.numeric);
    // The Chinese face is the bootstrap one itself, so it has no fallback: if
    // it is missing the port cannot draw the Simplified Chinese catalogue and
    // there is nothing to fall back TO.
    const bool haveChinese =
        FT_New_Face(gUi.library,
                    portPath("romfs:/ui/NotoSansCJKsc-RR3Bootstrap.otf",
                             chinesePath, sizeof(chinesePath)), 0,
                    &gUi.chinese) == 0;
    if (!haveChinese) {
        nativeUiExit();
        return false;
    }
    // The faces that were not found fall back to the one that was, so the
    // dashboard always has a face to draw with.
    if (!haveRegular) gUi.regular = gUi.chinese;
    if (!haveBold) gUi.bold = gUi.chinese;
    if (!haveNumeric) gUi.numeric = gUi.chinese;
    constexpr unsigned char kOriginalCatalogHeader[] = {0x86, 0x5f, 0xed};
    const bool looseData = externalDataStatus().looseFallback;
    // The .depot paths, resolved through the card fallback.  They are
    // buffers because a resolved path is a different string from the RomFS
    // one, and the calls below take a const char*.
    char chineseCatalogPath[512];
    char languagesPath[512];
    char englishCatalogPath[512];
    char motionManifestPath[512];
    char motionPrefixPath[512];
    const char* externalChinese = looseData
        ? "sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/text_cn-Hans.txt"
        : "rr3core:/files/.depot/text_cn-Hans.txt";
    const char* externalLanguages = looseData
        ? "sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/langs.utf8"
        : "rr3core:/files/.depot/langs.utf8";
    gUi.originalChineseExternal = fileStartsWith(
        externalChinese,
        kOriginalCatalogHeader, sizeof(kOriginalCatalogHeader)) &&
        fileContainsText(externalLanguages,
                         "Chinese simplified,cn-Hans");
    gUi.originalChineseCatalog = gUi.originalChineseExternal ||
        (fileStartsWith(portPath("romfs:/original/.depot/text_cn-Hans.txt",
                                 chineseCatalogPath, sizeof(chineseCatalogPath)),
                        kOriginalCatalogHeader,
                        sizeof(kOriginalCatalogHeader)) &&
         fileContainsText(portPath("romfs:/original/.depot/langs.utf8",
                                  languagesPath, sizeof(languagesPath)),
                          "Chinese simplified,cn-Hans"));
    gUi.originalEnglishCatalog = fileStartsWith(
        portPath("romfs:/original/.depot/text_en.txt",
                 englishCatalogPath, sizeof(englishCatalogPath)),
        kOriginalCatalogHeader, sizeof(kOriginalCatalogHeader));
    const char* externalMotionManifest = looseData
        ? "sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/MenuTransitions.xml"
        : "rr3core:/files/.depot/MenuTransitions.xml";
    const char* motionManifest = fileExists(externalMotionManifest)
        ? externalMotionManifest
        : portPath("romfs:/original/.depot/MenuTransitions.xml",
                   motionManifestPath, sizeof(motionManifestPath));
    gUi.originalMotionManifest = fileContainsText(
        motionManifest,
        "<KeyFrame start=\"0\" end=\"40\"/>") &&
        fileContainsText(motionManifest,
                         "<KeyFrame start=\"0\" end=\"90\"/>");
    const char* externalMotionProbe = looseData
        ? "sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/Tracks/Menu/processed/menu_transition.banim"
        : "rr3track:/files/.depot/Tracks/Menu/processed/menu_transition.banim";
    const char* motionPrefix = fileExists(externalMotionProbe)
        ? (looseData
            ? "sdmc:/switch/rr3-native-rehost/data/loose/files/.depot/Tracks/Menu/processed/"
            : "rr3track:/files/.depot/Tracks/Menu/processed/")
        : portPath("romfs:/original/.depot/Tracks/Menu/processed/",
                   motionPrefixPath, sizeof(motionPrefixPath));
    char motionPath[0x180]{};
    std::snprintf(motionPath, sizeof(motionPath), "%smenu_transition.banim", motionPrefix);
    gUi.originalMotionTracks = fileExists(motionPath);
    std::snprintf(motionPath, sizeof(motionPath),
                  "%smenu_carselect_transition.banim", motionPrefix);
    gUi.originalMotionTracks = gUi.originalMotionTracks && fileExists(motionPath);

    // A/B SWITCH, 2026-10-11: "nofb=1" in sdmc:/switch/rr3-gfx.conf.
    //
    // This framebuffer is the only thing the parent process touches on the GPU
    // before runGuestViewProbe() calls eglInitialize(), and the Mesa run blocks
    // there forever (gui4/gui5/mesa1 all terminate at "view-egl-init-begin",
    // 412 s, no later EGL line). The child that WORKS -- runWindowGraphicsProbe
    // at main.cpp:566 -- runs before any of this and never touches a
    // framebuffer. So "create a framebuffer on the default NWindow, close it,
    // then let Mesa initialise EGL on that same NWindow" is the one remaining
    // difference, and it is a single-variable question worth one run.
    //
    // Reports SUCCESS rather than failing: a false return sends main.cpp down
    // the consoleInit(nullptr) fallback, which starts deko3d and would occupy
    // the very frontend this experiment is trying to leave free.
    if (rr3::gfxSkipFramebufferUi()) {
        gUi.active = false;
        return true;
    }

    if (R_FAILED(framebufferCreate(&gUi.framebuffer, nwindowGetDefault(),
                                   kWidth, kHeight, PIXEL_FORMAT_RGBA_8888, 2)) ||
        R_FAILED(framebufferMakeLinear(&gUi.framebuffer))) {
        nativeUiExit();
        return false;
    }
    gUi.active = true;
    return true;
}

void nativeUiExit() {
    // THE FACES MAY SHARE ONE POINTER.
    //
    // nativeUiInit() falls back to the OFL bootstrap face when a game face
    // is not there, so regular, bold and numeric can all BE the chinese
    // face.  Closing the same FT_Face three times is a double free, and a
    // double free in a destructor path is exactly the kind of crash that
    // looks like "the console froze for no reason".
    //
    // So each pointer is closed ONCE, and a pointer that is the same object
    // as one already closed is skipped.
    if (gUi.chinese) FT_Done_Face(gUi.chinese);
    if (gUi.numeric && gUi.numeric != gUi.chinese) FT_Done_Face(gUi.numeric);
    if (gUi.bold && gUi.bold != gUi.chinese && gUi.bold != gUi.numeric) {
        FT_Done_Face(gUi.bold);
    }
    if (gUi.regular && gUi.regular != gUi.chinese && gUi.regular != gUi.numeric &&
        gUi.regular != gUi.bold) {
        FT_Done_Face(gUi.regular);
    }
    if (gUi.library) FT_Done_FreeType(gUi.library);
    if (gUi.framebuffer.has_init) framebufferClose(&gUi.framebuffer);
    gUi = {};
}

bool nativeUiActive() { return gUi.active; }

void nativeUiRenderLoading(const char* stage, unsigned int percent) {
    if (!gUi.active) return;
    beginFrame();
    chrome("正在载入原生运行环境", 0);
    text(74, 205, "REAL RACING 3", 60, kPorcelain, true);
    text(78, 250, "NATIVE REHOST", 25, kSteel, true, true);
    text(78, 390, "移植组件初始化", 34, kPorcelain, true);
    text(78, 438, localizedStage(stage), 24, kSilver);
    progressBar(78, 474, 820, percent);
    text(936, 493, "请稍候", 25, kPorcelain, true);
    endFrame();
}

void nativeUiRenderDashboard(const DashboardData& data, int page, const char* notice) {
    if (!gUi.active) return;
    beginFrame();
    if (page == 0) {
        chrome("原版移植状态", 0);
        text(74, 142, "欢迎回来", 26, kSteel);
        text(74, 193, "REAL RACING 3", 55, kPorcelain, true);
        text(76, 231, "Nintendo Switch 原生移植", 24, kSilver);
        char volumeStatus[32]{};
        std::snprintf(volumeStatus, sizeof(volumeStatus), "%u/7 %s",
                      data.externalData.mountedVolumes,
                      data.externalData.ready ? "已挂载" : "缺失");
        statusRow(286, "完整游戏数据卷", volumeStatus, data.externalData.ready ? kGreen : kRed);
        statusRow(354, "简体中文本地化", gUi.originalChineseExternal ? "原包优先" : (gUi.originalChineseCatalog ? "内置回退" : "缺失"), gUi.originalChineseExternal ? kGreen : kOrange);
        statusRow(422, "四核心 DSO / 路径 VFS", data.loader.activationReady && data.filesystemTest == 0xfff ? "就绪" : "受限", data.loader.activationReady && data.filesystemTest == 0xfff ? kBlue : kRed);
        statusRow(490, "原版菜单动效数据", gUi.originalMotionManifest && gUi.originalMotionTracks ? "原轨保留" : "等待资源", gUi.originalMotionManifest && gUi.originalMotionTracks ? kGreen : kOrange);
        text(76, 604, "L/R  页面", 25, kPorcelain, true);
        text(876, 604, "+  安全退出", 25, kPorcelain, true);
    }
    else if (page == 1) {
        chrome("兼容性与恢复", 1);
        const bool recovery = data.recoveryMode && !data.guestInitialization.active;
        angled(62, 124, 1150, 120, 36, recovery ? kRed : kGreen);
        text(106, 182, recovery ? "检测到上次启动中断" : "运行环境状态正常", 38, kPorcelain, true);
        text(106, 220, recovery ? "已阻止重复执行；复制报告后按 B 清除。" : "所有危险动作均受恢复保护。", 24, kPorcelain);
        statusRow(292, "Guest TPIDR_EL0", data.guestTlsTest == 0xf ? "通过" : "失败", data.guestTlsTest == 0xf ? kGreen : kRed);
        statusRow(360, "映射 / 反向卸载", "4 / 4", kBlue);
        statusRow(428, "缺失强符号", data.loader.missingRelocations == 0 ? "0" : "存在", data.loader.missingRelocations == 0 ? kGreen : kRed);
        text(74, 604, "B  清除恢复状态", 25, kPorcelain, true);
        text(830, 604, "ZR  验证依赖初始化", 23, kPorcelain, true);
    }
    else {
        chrome(page == 2 ? "开发者：加载器" : "开发者：诊断", page);
        text(74, 145, "隐藏工程信息", 32, kOrange, true);
        char line[192]{};
        std::snprintf(line, sizeof(line), "IMAGES %llu   PATCHED %llu/%llu   INVALID %llu",
                      static_cast<unsigned long long>(data.loader.imageCount),
                      static_cast<unsigned long long>(data.loader.patchedRelocations),
                      static_cast<unsigned long long>(data.loader.verifiedWrites),
                      static_cast<unsigned long long>(data.loader.invalidRelocations));
        text(74, 214, line, 24, kPorcelain, false, true);
        std::snprintf(line, sizeof(line), "PLAN %016llX   CTORS %llu/%llu",
                      static_cast<unsigned long long>(data.loader.resolutionPlanHash),
                      static_cast<unsigned long long>(data.guestInitialization.constructorsCalled),
                      static_cast<unsigned long long>(data.guestInitialization.constructorsPlanned));
        text(74, 262, line, 24, kPorcelain, false, true);
        std::snprintf(line, sizeof(line), "CLEANUP %llu/%llu   FAIL %llu   RC %08X",
                      static_cast<unsigned long long>(data.unload.unmappedMappings),
                      static_cast<unsigned long long>(data.unload.activeMappings),
                      static_cast<unsigned long long>(data.unload.failures),
                      data.unload.firstFailureResult);
        text(74, 310, line, 24, kPorcelain, false, true);
        std::snprintf(line, sizeof(line), "ORIGINAL MOTION MENU %u-%u   CAR SELECT %u-%u",
                      kOriginalMenuTransitionFirstFrame,
                      kOriginalMenuTransitionLastFrame,
                      kOriginalCarSelectFirstFrame,
                      kOriginalCarSelectLastFrame);
        text(74, 358, line, 24,
             gUi.originalMotionManifest && gUi.originalMotionTracks
                 ? kGreen : kOrange,
             false, true);
        std::snprintf(line, sizeof(line), "GUEST VIEW C/C/R %u/%u/%u  EGL %u/%u  SWAP %u",
                      data.viewProbe.createdReturned ? 1U : 0U,
                      data.viewProbe.changedReturned ? 1U : 0U,
                      data.viewProbe.renderReturned ? 1U : 0U,
                      data.viewProbe.madeCurrent ? 1U : 0U,
                      data.viewProbe.failureCode,
                      data.viewProbe.swapped ? 1U : 0U);
        text(74, 406, line, 24,
             data.viewProbe.renderReturned && data.viewProbe.swapped
                 ? kGreen : kOrange,
             false, true);
        std::snprintf(line, sizeof(line), "VIEW JNI %u/%u/%u  GL %08X  EGL %08X",
                      data.viewProbe.createdJni.unhandledCalls,
                      data.viewProbe.changedJni.unhandledCalls,
                      data.viewProbe.renderJni.unhandledCalls,
                      data.viewProbe.glError, data.viewProbe.eglError);
        text(74, 454, line, 24, kPorcelain, false, true);
    }
    if (notice != nullptr && notice[0] != '\0') {
        rect(0, 658, kWidth, 62, kCarbon);
        text(38, 698, notice, 22, kPorcelain);
    }
    endFrame();
}

} // namespace rr3
