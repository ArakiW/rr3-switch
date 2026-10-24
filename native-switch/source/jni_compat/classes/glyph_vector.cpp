// com.firemint.realracing.GlyphVector -- the guest's paragraph-text class.
//
// The contract (every signature, every field, and the guest address that
// proves it) is in glyph_vector.h. This file is the C++ that stands in for
// the Android runtime the guest would normally have:
//
//     android.graphics.Paint        -> the FreeType face and its metrics
//     android.text.StaticLayout     -> the word wrap, the alignment, the lines
//     android.graphics.Bitmap       -> the 8-bit coverage raster
//     android.opengl.GLUtils        -> the GL texture upload
//
// WHY THE LAYOUT IS DONE HERE AND NOT IN JAVA
// -----------------------------------------
// There is no JVM on this port, so the guest's Android build -- which runs
// GlyphVector as ART bytecode -- cannot run it. The guest's native side calls
// the class through JNI and reads the ten fields back, so the port has to
// answer those calls with the same values the Java would have produced.
//
// The numbers follow GlyphVector.java from the guest APK
// (reverse/java/sources/com/firemint/realracing/GlyphVector.java):
//   * init()           -> the ink box of the whole string, one line.
//   * initWithParagraph()
//                      -> a word-wrapped layout, ALIGN_NORMAL / ALIGN_OPPOSITE
//                         / ALIGN_CENTER, the line count, and the ink union.
//   * createTexture()  -> a power-of-two GL texture holding the raster.
//   * renderToTexture()-> the same text re-rendered into a texture the guest
//                         already owns.
//
// WHAT IS MEASURED AND WHAT IS APPROXIMATED
// -----------------------------------------
// MEASURED: the JNI signatures, the ten fields, the order the guest reads
// them in, the fact that the paragraph is a word-wrapped layout with three
// alignments, and the fact that createTexture() produces a POT GL texture and
// renderToTexture() produces an ALPHA_8 sub-image.
//
// APPROXIMATED: Android's exact line-box arithmetic. The port uses the
// FreeType face's ascender/descender, rounded up, as the ascent and descent,
// and a line box of ascent+descent. Android rounds the same values and adds
// the face's leading, which FreeType reports as (height - ascender +
// descender); the difference is at most one pixel per line. The wrap points
// are the spaces (Android also breaks after some punctuation); a word longer
// than the wrap width is broken between characters, as Android does.
//
// NOT IMPLEMENTED: the ellipsis variants. breakStyle 3/4/5 (truncate head /
// tail / middle) and 6 (word wrap + truncate) clip the text at the height
// limit instead of adding an ellipsis, because an ellipsis would need
// Android's exact glyph metrics to land in the right place.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <GLES2/gl2.h>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../../../include/rr3/guest_view_trace.hpp"
#include "../../../include/rr3/gfx_text_probe.hpp"
#include "../jni.h"
#include "../jni_internals.h"
#include "glyph_vector.h"

// The bridge's exported accessors (see rr3/glyph_vector.h) are declared in
// glyph_vector.h: glyphClassName, glyphStringText and glyphFontRequest.

// The Class the ManagedMethod table refers to. Declared before the table so
// the table can take its address; only the address matters, so the static
// initialisation order is not a problem.
extern Class ClassGlyphVector;

namespace glyphvector {
namespace {

// ---------------------------------------------------------------------------
// The typeface
// ---------------------------------------------------------------------------
//
// The guest's Font.init(String path, boolean bold, boolean italic, float size,
// float widthScale) names a file inside the game's own VFS. The port ships
// the game's UI faces inside the NRO's romfs (native-switch/romfs/ui/), so
// the basename of the path is enough to pick one.
//
// The faces are cached: FreeType faces are expensive to open and the guest
// creates a Font per (face, style, size) and then hands the same Font to
// every GlyphVector that uses it.

constexpr size_t kFaceSlots = 6;

struct FaceSlot {
    FT_Face face;
    char key[96];
};

FT_Library g_library = nullptr;
FaceSlot g_faces[kFaceSlots];
size_t g_faceCount = 0;
FT_Face g_cjkFace = nullptr;

// FreeType faces are NOT thread-safe and the guest drives JNI from several
// threads. One lock covers every face operation and the raster below.
Mutex g_glyphMutex;

// The faces the port can offer, in the order it prefers them.
struct FaceChoice {
    const char* file;   // the basename the guest asks for
    const char* romfs;  // the face inside the NRO
};

// THE GAME'S OWN FACES, BY EXACT NAME.
//
// The old table matched by SUBSTRING, which is what broke it: the needle
// "EurostileLTStd" matches "EurostileLTStd-Demi.otf" AND
// "EurostileLTStd.otf", and the table sent every one of them to
// EurostileLTStd-Bold.otf. So the game's REGULAR weight was drawn bold, its
// Demi was drawn bold, and its Ex2 was drawn bold. "ConduitITCPro" had the
// same problem the other way: ConduitITCPro-ExtraBold.otf was drawn as
// ConduitITCPro-Regular.otf.
//
// An exact name match cannot do that, and it is also what the guest asked
// for: the guest's Java measures the UI text with the face it names, so the
// port has to measure it with the same face or the laid-out paragraph is
// not the paragraph the guest asked for.
//
// The faces that are not staged in this build simply fail to open, and the
// caller falls back to the Conduit pair -- which is what happened before,
// except that the log now says which face was really used.
constexpr FaceChoice kGameFaces[] = {
    {"ConduitITCPro-Regular.otf", "romfs:/ui/ConduitITCPro-Regular.otf"},
    {"ConduitITCPro-Bold.otf", "romfs:/ui/ConduitITCPro-Bold.otf"},
    {"ConduitITCPro-Medium.otf", "romfs:/ui/ConduitITCPro-Medium.otf"},
    {"ConduitITCPro-ExtraBold.otf", "romfs:/ui/ConduitITCPro-ExtraBold.otf"},
    {"EurostileLTStd.otf", "romfs:/ui/EurostileLTStd.otf"},
    {"EurostileLTStd-Bold.otf", "romfs:/ui/EurostileLTStd-Bold.otf"},
    {"EurostileLTStd-BoldEx2.otf", "romfs:/ui/EurostileLTStd-BoldEx2.otf"},
    {"EurostileLTStd-BoldOblique.otf",
     "romfs:/ui/EurostileLTStd-BoldOblique.otf"},
    {"EurostileLTStd-Demi.otf", "romfs:/ui/EurostileLTStd-Demi.otf"},
    {"EurostileLTStd-DemiOblique.otf",
     "romfs:/ui/EurostileLTStd-DemiOblique.otf"},
    {"EurostileLTStd-Ex2.otf", "romfs:/ui/EurostileLTStd-Ex2.otf"},
    {"HelveticaNeueBold.ttf", "romfs:/ui/HelveticaNeueBold.ttf"},
    {"LCD.ttf", "romfs:/ui/LCD.ttf"},
    {"minion_pro_italic.otf", "romfs:/ui/minion_pro_italic.otf"},
    {"myriadp0.otf", "romfs:/ui/myriadp0.otf"},
};

// THE FACES THAT ARE NOT TEXT.
//
// r3_symbols.ttf and r3_decals.ttf are the game's SYMBOL fonts. Measured
// from the files: upem 2048, ascent 2.75 em, and the '0' of r3_symbols.ttf
// advances 2.38 em with an ink box of 2.29 x 2.28 em. Those are the big
// placeholder boxes the game draws its currency and badge art into, not
// letters, and drawing a paragraph with them produces solid boxes.
//
// So the port deliberately draws those with the text face instead. It is a
// DECISION in the source, not an accident of a missing needle, and the log
// says when it happened.
//
// LCD.ttf is NOT in this list, even though its upem is 2048 too: it is the
// game's display face for the HUD's digits, and its digit advances 0.71 em
// with an ink box 0.62 x 1.00 em. It is used as it is.
constexpr const char* kNonTextFaces[] = {
    "r3_symbols.ttf",
    "r3_decals.ttf",
};

const char* basenameOf(const char* path) {
    if (path == nullptr) {
        return "";
    }
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

// (nameContains used to live here. It matched by SUBSTRING, which is what
// sent "EurostileLTStd.otf" to the BOLD face: the exact-name table in
// kGameFaces replaced it, and a substring match is exactly the thing that
// must not decide which typeface a paragraph is measured with.)

// The face the port draws the text with.
//
// MEASURED from the game's own file (.build/r3_symbols.ttf, and the copy the
// guest names at sdmc:/switch/rr3-native-rehost/.depot/r3_symbols.ttf):
//
//   head.unitsPerEm = 2048
//   hhea.ascender  = 5628   = 2.748 em     (a real face is 0.75..1.0 em)
//   hhea.descender = 0
//   glyph '1'      = ink 4037 x 5700 units = 1.971 x 2.783 em, advance 2.055 em
//   glyph '0'      = ink 4680 x 4676 units = 2.285 x 2.283 em, advance 2.383 em
//
// So r3_symbols.ttf is the game's SYMBOL font: its "digits" are the big
// placeholder boxes the game draws its currency and badge art into, and its
// head table is scrambled (which is why the port's own Font comment calls the
// hhea scrambled). It is not a text face, and drawing the UI's paragraphs
// with it produces solid boxes.
//
// The port therefore draws text with the game's real Latin UI face, which
// ships in the NRO's romfs (ConduitITCPro is RR3's UI typeface), and falls
// back to the CJK face for the codepoints the Latin face does not have --
// which is every Chinese one, and the port's UI is Chinese.
//
// The romfs faces are also the port's established convention: native_ui.cpp
// has always used exactly these files.
FT_Face openFace(const char* romfsPath);

// Is this face one of the game's SYMBOL fonts, which the port deliberately
// replaces with a text face? (See kNonTextFaces.)
bool isNonTextFace(const char* base) {
    if (base == nullptr) {
        return false;
    }
    for (const char* name : kNonTextFaces) {
        if (strcmp(base, name) == 0) {
            return true;
        }
    }
    return false;
}

// The face the port draws with, for a face the guest asked for.
//
// THE ORDER MATTERS:
//   1. the game's own face, by EXACT name -- the one the guest measured
//      its layout with;
//   2. the Conduit pair, in the requested weight, for everything else.
//
// A face that is staged but not present in this build fails to open and
// falls through to (2), which is what the port did before this table
// existed.
FT_Face openFaceFor(const char* requestedPath, bool bold) {
    const char* base = basenameOf(requestedPath);
    for (const FaceChoice& choice : kGameFaces) {
        if (strcmp(base, choice.file) != 0) {
            continue;
        }
        FT_Face face = openFace(choice.romfs);
        if (face != nullptr) {
            return face;
        }
        // Not staged in this build: fall through to the family fallback.
        break;
    }
    return openFace(bold ? "romfs:/ui/ConduitITCPro-Bold.otf"
                          : "romfs:/ui/ConduitITCPro-Regular.otf");
}

FT_Face openFace(const char* romfsPath) {
    for (size_t i = 0; i < g_faceCount; ++i) {
        if (strcmp(g_faces[i].key, romfsPath) == 0) {
            return g_faces[i].face;
        }
    }
    if (g_faceCount >= kFaceSlots) {
        return nullptr;
    }
    FT_Face face = nullptr;
    if (FT_New_Face(g_library, romfsPath, 0, &face) != 0 || face == nullptr) {
        return nullptr;
    }
    FaceSlot& slot = g_faces[g_faceCount];
    slot.face = face;
    snprintf(slot.key, sizeof(slot.key), "%s", romfsPath);
    ++g_faceCount;
    return face;
}

// The romfs path of a face that is already open, or "" when it is not one
// of ours.
//
// THIS IS WHAT MAKES THE LOG HONEST. The old log line printed the path the
// GUEST ASKED FOR together with the metrics of the face the port ACTUALLY
// opened, and its openErr was a literal 0. So a line read
//
//     face=sdmc:/.../r3_symbols.ttf openErr=0 size=175.0 upem=1000
//
// and every reader -- including this project -- concluded that the symbol
// font was a 1000-upem text face with 653 glyphs. It was not: the port was
// drawing ConduitITCPro-Regular and calling it by the symbol font's name.
//
// The face's real identity is now reported next to the request.
const char* facePath(FT_Face face) {
    if (face == nullptr) {
        return "";
    }
    for (size_t i = 0; i < g_faceCount; ++i) {
        if (g_faces[i].face == face) {
            return g_faces[i].key;
        }
    }
    return "";
}

// The Chinese face, opened on demand. It is a big file, so it is only
// touched when the Latin face has no glyph for a codepoint.
constexpr const char* kCjkFacePath = "romfs:/ui/NotoSansCJKsc-RR3Bootstrap.otf";

FT_Face cjkFace() {
    if (g_cjkFace == nullptr) {
        g_cjkFace = openFace(kCjkFacePath);
    }
    return g_cjkFace;
}

// Sets the face's pixel size. FT_Set_Pixel_Sizes is refused by a face with
// no scaled strike, and FT_Set_Char_Size is the fallback for those.
FT_Error sizeFace(FT_Face face, float sizePx) {
    if (face == nullptr) {
        return 1;
    }
    const FT_UInt pixels = static_cast<FT_UInt>(sizePx + 0.5f);
    FT_Error error = FT_Set_Pixel_Sizes(face, 0, pixels);
    if (error != 0) {
        error = FT_Set_Char_Size(face, 0, pixels * 64, 72, 72);
    }
    return error;
}

// ---------------------------------------------------------------------------
// Font metrics and glyph measurement
// ---------------------------------------------------------------------------

struct Metrics {
    float ascent;   // pixels above the baseline
    float descent;  // pixels below the baseline
    float leading;  // extra leading the face asks for
};

Metrics metricsOf(FT_Face face) {
    Metrics m{0.f, 0.f, 0.f};
    if (face == nullptr || face->size == nullptr) {
        return m;
    }
    const FT_Size_Metrics& sm = face->size->metrics;
    const float ascent = static_cast<float>(-sm.ascender) / 64.0f;
    const float descent = static_cast<float>(-sm.descender) / 64.0f;
    const float height = static_cast<float>(sm.height) / 64.0f;
    m.ascent = ceilf(ascent);
    m.descent = ceilf(descent);
    m.leading = height - (ascent + descent);
    if (m.leading < 0.f) {
        m.leading = 0.f;
    }
    return m;
}

// The face that actually carries a codepoint, plus its glyph index.
FT_UInt glyphIndex(FT_Face face, FT_Face fallback, uint32_t codepoint,
                   FT_Face* used) {
    FT_UInt index = FT_Get_Char_Index(face, codepoint);
    *used = face;
    if (index == 0 && fallback != nullptr) {
        const FT_UInt alt = FT_Get_Char_Index(fallback, codepoint);
        if (alt != 0) {
            index = alt;
            *used = fallback;
        }
    }
    return index;
}

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

// Decodes one codepoint. Returns the number of bytes consumed, or 0 when the
// input is not valid UTF-8 (in which case the byte is consumed as U+FFFD).
size_t decodeUtf8(const char* text, size_t len, size_t at, uint32_t* out) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    size_t extra = 0;
    uint32_t value = 0;
    if (c < 0x80) {
        value = c;
    } else if ((c & 0xE0) == 0xC0) {
        extra = 1;
        value = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        value = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        value = c & 0x07u;
    } else {
        *out = 0xFFFD;
        return 1;
    }
    if (at + extra >= len) {
        *out = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i <= extra; ++i) {
        const unsigned char cc = static_cast<unsigned char>(text[at + i]);
        if ((cc & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (cc & 0x3Fu);
    }
    *out = value;
    return extra + 1;
}

// ---------------------------------------------------------------------------
// The layout
// ---------------------------------------------------------------------------

struct Item {
    uint32_t codepoint;
    size_t byteOffset;
    size_t byteLength;
    float advance;
    bool isSpace;
};

constexpr size_t kMaxItems = 4096;
constexpr size_t kMaxLines = 64;

struct Line {
    size_t start;   // byte offset of the first byte on the line
    size_t end;     // byte offset just past the last byte on the line
    float width;    // advance width, trailing spaces excluded
    float left;     // x of the line inside the paragraph
    float inkLeft, inkTop, inkRight, inkBottom;  // union of the line's ink,
                                                  // relative to the baseline
    bool hasInk;
};

struct Layout {
    Line lines[kMaxLines];
    size_t lineCount;
    Metrics metrics;
};

// The wrap width the guest asks for is a float; a non-positive or absurd one
// means "do not wrap", which is what StaticLayout does with width 0.
constexpr float kMaxWrapWidth = 4096.f;

// Defined further down, next to the rest of the instrument.
void noteFace(const char* romfsPath, FT_Face face, FT_Error error, float sizePx,
              FT_Error sizeError);
void noteGlyph(uint32_t codepoint, FT_Face face, FT_UInt index, int left, int top,
               int width, int rows, float advance);

void layoutParagraph(const char* text, size_t len, FT_Face face,
                     FT_Face fallback, float widthScale, float wrapWidth,
                     int alignment, Layout* out) {
    memset(out, 0, sizeof(*out));
    out->metrics = metricsOf(face);

    const bool wrap = wrapWidth > 0.f && wrapWidth < kMaxWrapWidth;

    // Pass 1: measure every codepoint once.
    static Item items[kMaxItems];
    size_t itemCount = 0;

    size_t at = 0;
    while (at < len && itemCount < kMaxItems) {
        uint32_t codepoint = 0;
        const size_t used = decodeUtf8(text, len, at, &codepoint);
        FT_Face glyphFace = face;
        const FT_UInt index =
            glyphIndex(face, fallback, codepoint, &glyphFace);
        float advance = 0.f;
        if (glyphFace != nullptr &&
            FT_Load_Glyph(glyphFace, index, FT_LOAD_DEFAULT) == 0) {
            advance = static_cast<float>(glyphFace->glyph->advance.x) / 64.0f;
        }
        Item& item = items[itemCount];
        item.codepoint = codepoint;
        item.byteOffset = at;
        item.byteLength = used;
        item.advance = advance * widthScale;
        item.isSpace = (codepoint == ' ' || codepoint == '\t' ||
                         codepoint == '\n' || codepoint == '\r');
        ++itemCount;
        at += used;
    }

    // Pass 2: break the items into lines.
    size_t lineStart = 0;
    size_t contentEnd = 0;
    float contentWidth = 0.f;
    float pendingSpace = 0.f;
    size_t i = 0;
    auto emitLine = [&](size_t end, float width) {
        if (out->lineCount >= kMaxLines) {
            return;
        }
        Line& line = out->lines[out->lineCount];
        line.start = lineStart;
        line.end = end;
        line.width = width;
        line.left = 0.f;
        line.hasInk = false;
        ++out->lineCount;
    };

    while (i < itemCount) {
        if (items[i].isSpace) {
            if (items[i].codepoint == '\n' || items[i].codepoint == '\r') {
                // A hard break ends the line where it is.
                emitLine(contentEnd, contentWidth);
                lineStart = items[i].byteOffset + items[i].byteLength;
                contentEnd = lineStart;
                contentWidth = 0.f;
                pendingSpace = 0.f;
                ++i;
                continue;
            }
            pendingSpace += items[i].advance;
            ++i;
            continue;
        }

        // One word: the run of codepoints up to the next space.
        const size_t wordStart = i;
        float wordWidth = 0.f;
        while (i < itemCount && !items[i].isSpace) {
            wordWidth += items[i].advance;
            ++i;
        }
        const size_t wordEnd = i;

        if (wrap && contentWidth > 0.f &&
            contentWidth + pendingSpace + wordWidth > wrapWidth) {
            emitLine(contentEnd, contentWidth);
            lineStart = items[wordStart].byteOffset;
            contentWidth = 0.f;
            pendingSpace = 0.f;
        }

        if (wrap && contentWidth == 0.f && wordWidth > wrapWidth) {
            // A single word wider than the paragraph: break it between
            // characters, the way StaticLayout does.
            float chunk = 0.f;
            size_t chunkStart = wordStart;
            for (size_t k = wordStart; k < wordEnd; ++k) {
                if (chunk > 0.f && chunk + items[k].advance > wrapWidth) {
                    lineStart = items[chunkStart].byteOffset;
                    emitLine(items[k].byteOffset, chunk);
                    chunkStart = k;
                    chunk = 0.f;
                }
                chunk += items[k].advance;
            }
            lineStart = items[chunkStart].byteOffset;
            contentEnd = items[wordEnd - 1].byteOffset + items[wordEnd - 1].byteLength;
            contentWidth = chunk;
            pendingSpace = 0.f;
            continue;
        }

        contentWidth += pendingSpace + wordWidth;
        contentEnd = items[wordEnd - 1].byteOffset + items[wordEnd - 1].byteLength;
        pendingSpace = 0.f;
    }
    if (contentEnd > lineStart || out->lineCount == 0) {
        emitLine(contentEnd, contentWidth);
    }

    // Pass 3: the ink box of every line, relative to that line's baseline.
    for (size_t l = 0; l < out->lineCount; ++l) {
        Line& line = out->lines[l];
        float pen = 0.f;
        for (size_t k = 0; k < itemCount; ++k) {
            const Item& item = items[k];
            if (item.byteOffset < line.start) {
                continue;
            }
            if (item.byteOffset >= line.end) {
                break;
            }
            if (!item.isSpace) {
                FT_Face glyphFace = face;
                const FT_UInt index =
                    glyphIndex(face, fallback, item.codepoint, &glyphFace);
                if (glyphFace != nullptr &&
            FT_Load_Glyph(glyphFace, index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
                    const FT_GlyphSlot slot = glyphFace->glyph;
                    noteGlyph(item.codepoint, glyphFace, index, slot->bitmap_left,
                              slot->bitmap_top,
                              slot->format == FT_GLYPH_FORMAT_BITMAP
                                  ? static_cast<int>(slot->bitmap.width)
                                  : 0,
                              slot->format == FT_GLYPH_FORMAT_BITMAP
                                  ? static_cast<int>(slot->bitmap.rows)
                                  : 0,
                              static_cast<float>(slot->advance.x) / 64.f);
                    if (slot->format == FT_GLYPH_FORMAT_BITMAP && slot->bitmap.width > 0 &&
                        slot->bitmap.rows > 0) {
                        const float left = pen + static_cast<float>(slot->bitmap_left) * widthScale;
                        const float right = left + static_cast<float>(slot->bitmap.width) * widthScale;
                        const float top = -static_cast<float>(slot->bitmap_top);
                        const float bottom = top + static_cast<float>(slot->bitmap.rows);
                        if (!line.hasInk) {
                            line.inkLeft = left;
                            line.inkRight = right;
                            line.inkTop = top;
                            line.inkBottom = bottom;
                            line.hasInk = true;
                        } else {
                            if (left < line.inkLeft) line.inkLeft = left;
                            if (right > line.inkRight) line.inkRight = right;
                            if (top < line.inkTop) line.inkTop = top;
                            if (bottom > line.inkBottom) line.inkBottom = bottom;
                        }
                    }
                }
            }
            pen += item.advance;
        }
    }

    // Pass 4: the horizontal alignment of every line, and the paragraph box.
    float minLeft = 0.f;
    float maxRight = 0.f;
    for (size_t l = 0; l < out->lineCount; ++l) {
        Line& line = out->lines[l];
        const float reference = wrapWidth > 0.f ? wrapWidth : line.width;
        float left = 0.f;
        if (alignment == 1) {  // ALIGN_OPPOSITE
            left = reference - line.width;
        } else if (alignment == 2) {  // ALIGN_CENTER
            left = (reference - line.width) * 0.5f;
        }
        line.left = left;
        const float right = left + line.width;
        if (l == 0) {
            minLeft = left;
            maxRight = right;
        } else {
            if (left < minLeft) minLeft = left;
            if (right > maxRight) maxRight = right;
        }
    }
    (void)minLeft;
    (void)maxRight;
}

// ---------------------------------------------------------------------------
// The render state
// ---------------------------------------------------------------------------
//
// The guest keeps one GlyphVector per text element and asks it to re-render
// later (renderToTexture), so the TEXT has to outlive the call that carried
// it. The port's fake-object pool recycles a slot every 512 objects, so the
// text cannot live in the facade; it lives here, keyed by the object's
// handle.

// What the raster needs to know about the layout.
struct RenderPlan {
    bool paragraph;
    float lineHeight;
    float ascent;
    float boundsLeft;
    float pixelTopOffset;
    float singleOriginX;  // the single-line (init) path
    float singleOriginY;
    int32_t textureWidth;
    int32_t textureHeight;
    size_t usedLines;
};

struct RenderState {
    bool inUse;
    uintptr_t object;
    char* text;
    size_t textLen;
    float sizePx;
    float widthScale;
    bool bold;
    char fontPath[96];
    // The layout the measurement produced, kept so createTexture() can
    // rasterise exactly what was measured. The Java's createTexture() reads
    // m_layout, m_bounds and m_text, so the port has to keep the same three
    // things alive between the init call and the createTexture call.
    Layout layout;
    RenderPlan plan;
    bool hasLayout;
};

constexpr size_t kStateCount = 24;
RenderState g_states[kStateCount];

RenderState* stateFor(uintptr_t object) {
    for (size_t i = 0; i < kStateCount; ++i) {
        if (g_states[i].inUse && g_states[i].object == object) {
            return &g_states[i];
        }
    }
    return nullptr;
}

RenderState* acquireState(uintptr_t object) {
    RenderState* existing = stateFor(object);
    if (existing != nullptr) {
        return existing;
    }
    RenderState* slot = nullptr;
    for (size_t i = 0; i < kStateCount; ++i) {
        if (!g_states[i].inUse) {
            slot = &g_states[i];
            break;
        }
    }
    if (slot == nullptr) {
        // Every slot is busy: reuse the first one. The guest draws each
        // paragraph inside its own frame, so the oldest slot is the one that
        // has been waiting longest.
        slot = &g_states[0];
        free(slot->text);
        slot->text = nullptr;
        slot->textLen = 0;
    }
    slot->inUse = true;
    slot->object = object;
    slot->text = nullptr;
    slot->textLen = 0;
    slot->sizePx = 48.f;
    slot->widthScale = 1.f;
    slot->bold = false;
    slot->fontPath[0] = '\0';
    return slot;
}

// The instrument.
//
// The face and the glyph are the two places a wrong number can hide, so both
// are reported once: which file was opened, whether the pixel size took, and
// what the first glyph actually looked like. A run that shows a sane em and a
// sane glyph box has a real raster; one that does not has a font problem, and
// the difference is not visible in a screenshot.
void noteFace(const char* romfsPath, FT_Face face, FT_Error error, float sizePx,
              FT_Error sizeError) {
    static char lastKey[96] = {0};
    char key[96];
    snprintf(key, sizeof(key), "%s|%.1f", romfsPath, sizePx);
    if (strcmp(lastKey, key) == 0) {
        return;
    }
    snprintf(lastKey, sizeof(lastKey), "%s", key);
    const float em = face != nullptr && face->size != nullptr
                         ? static_cast<float>(face->units_per_EM)
                         : 0.f;
    const float ascent = face != nullptr && face->size != nullptr
                             ? static_cast<float>(-face->size->metrics.ascender) / 64.f
                             : 0.f;
    const float descent = face != nullptr && face->size != nullptr
                              ? static_cast<float>(-face->size->metrics.descender) / 64.f
                              : 0.f;
    // THE FACE THAT WAS REALLY OPENED. `romfsPath` is what the guest asked
    // for; the face may be a different file entirely, and the difference
    // has to be visible in the log.
    const char* actual = facePath(face);
    char details[384];
    snprintf(details, sizeof(details),
             "req=%s face=%s substituted=%d symbol=%d openErr=%d sizeErr=%d "
             "size=%.1f upem=%.0f ascentPx=%.1f descentPx=%.1f scalable=%d "
             "glyphs=%ld",
             romfsPath, actual,
             (actual != nullptr && strcmp(actual, romfsPath) != 0) ? 1 : 0,
             isNonTextFace(basenameOf(romfsPath)) ? 1 : 0,
             (int)error, (int)sizeError, sizePx, em, ascent, descent,
             face != nullptr ? (face->face_flags & FT_FACE_FLAG_SCALABLE) ? 1 : 0 : 0,
             face != nullptr ? static_cast<long>(face->num_glyphs) : 0L);
    rr3::guestViewWideLog("glyph-face", 0, 0, details);
}

void noteGlyph(uint32_t codepoint, FT_Face face, FT_UInt index, int left, int top,
               int width, int rows, float advance) {
    static unsigned s_glyphLines = 0;
    if (s_glyphLines >= 16) {
        return;
    }
    ++s_glyphLines;
    char details[224];
    snprintf(details, sizeof(details),
             "cp=U+%04X idx=%u face=%s left=%d top=%d w=%d h=%d advance=%.1f",
             codepoint, index, face != nullptr ? "primary" : "none", left, top, width,
             rows, advance);
    rr3::guestViewWideLog("glyph-glyph", 0, 0, details);
}

// ---------------------------------------------------------------------------
// The raster
// ---------------------------------------------------------------------------

// Two buffers: the 8-bit coverage the glyphs are drawn into, and the RGBA
// texels that go to the GPU. One buffer cannot be both.
uint8_t* g_coverage = nullptr;
size_t g_coverageCapacity = 0;
uint8_t* g_texels = nullptr;
size_t g_texelCapacity = 0;

uint8_t* growBuffer(uint8_t*& buffer, size_t& capacity, size_t bytes) {
    if (bytes > capacity) {
        uint8_t* grown = static_cast<uint8_t*>(realloc(buffer, bytes));
        if (grown == nullptr) {
            return nullptr;
        }
        buffer = grown;
        capacity = bytes;
    }
    return buffer;
}

// Draws `text` as one line into an 8-bit coverage buffer.
//
// `originX`/`originY` place the baseline. The buffer is `width` x `height`.
void rasterizeLine(FT_Face face, FT_Face fallback, const char* text,
                   size_t len, float widthScale, float originX, float originY,
                   int width, int height, uint8_t* buffer, bool clear) {
    // The buffer is cleared ONCE per texture, not once per line: a paragraph
    // draws every line into the same bitmap, and clearing per line erased the
    // lines already drawn.
    if (clear) {
        memset(buffer, 0, static_cast<size_t>(width) * static_cast<size_t>(height));
    }
    if (face == nullptr || text == nullptr) {
        return;
    }
    const int baselineY = static_cast<int>(originY + 0.5f);
    float pen = originX;
    size_t at = 0;
    while (at < len) {
        uint32_t codepoint = 0;
        const size_t used = decodeUtf8(text, len, at, &codepoint);
        FT_Face glyphFace = face;
        const FT_UInt index =
            glyphIndex(face, fallback, codepoint, &glyphFace);
        if (glyphFace != nullptr &&
            FT_Load_Glyph(glyphFace, index, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
            const FT_GlyphSlot slot = glyphFace->glyph;
            if (slot->format == FT_GLYPH_FORMAT_BITMAP && slot->bitmap.width > 0 &&
                slot->bitmap.rows > 0) {
                const int left = static_cast<int>(pen + slot->bitmap_left + 0.5f);
                const int top = baselineY - slot->bitmap_top;
                const int rows = static_cast<int>(slot->bitmap.rows);
                const int cols = static_cast<int>(slot->bitmap.width);
                const int pitch = slot->bitmap.pitch;
                for (int row = 0; row < rows; ++row) {
                    const int y = top + row;
                    if (y < 0 || y >= height) {
                        continue;
                    }
                    // FreeType's pitch is positive for a normal render, but a
                    // negative pitch means the rows run backwards.
                    const unsigned char* src =
                        pitch >= 0
                            ? slot->bitmap.buffer + static_cast<size_t>(row) * pitch
                            : slot->bitmap.buffer +
                                  static_cast<size_t>(rows - 1 - row) *
                                      static_cast<size_t>(-pitch);
                    for (int col = 0; col < cols; ++col) {
                        const int x = left + col;
                        if (x < 0 || x >= width) {
                            continue;
                        }
                        uint8_t& dst = buffer[static_cast<size_t>(y) *
                                                 static_cast<size_t>(width) +
                                             static_cast<size_t>(x)];
                        const uint8_t value = src[col];
                        if (value > dst) {
                            dst = value;
                        }
                    }
                }
            }
            pen += static_cast<float>(slot->advance.x) / 64.0f * widthScale;
        }
        at += used;
    }
}

// ---------------------------------------------------------------------------
// The facade helpers
// ---------------------------------------------------------------------------

GlyphVectorFacade* facadeFor(uintptr_t object) {
    if (object == 0) {
        return nullptr;
    }
    GlyphVectorFacade* facade = reinterpret_cast<GlyphVectorFacade*>(object);
    if (facade->magic != kGlyphVectorMagic) {
        return nullptr;
    }
    return facade;
}

int nextPowerOfTwo(int value) {
    int result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

// ---------------------------------------------------------------------------
// The instrument
// ---------------------------------------------------------------------------
//
// One line per distinct (method, size) so a run can say what the port did,
// not only that the class resolved. The positive control is the Font line
// the same run prints, so a run with no GlyphVector line is a real absence
// and not a broken logger.

void noteGlyphVector(const char* what, const GlyphVectorFacade* facade,
                     size_t textLen, float sizePx, const char* text) {
    // The first few calls of each kind, NOT one line per distinct key: the
    // key-only version logged the first occurrence of each size and so
    // reported "textLen=0" for every one of them, which reads like "the text
    // never arrives" and cannot tell "never arrived" from "arrived after the
    // first sample".
    static char lastKey[64] = {0};
    char key[64];
    snprintf(key, sizeof(key), "%s|%.1f", what, sizePx);
    const bool sameKey = strcmp(lastKey, key) == 0;
    snprintf(lastKey, sizeof(lastKey), "%s", key);
    static unsigned s_lines = 0;
    if (sameKey && s_lines >= 24) {
        return;
    }
    ++s_lines;
    char preview[96];
    preview[0] = '\0';
    if (text != nullptr) {
        size_t n = 0;
        while (n < 60 && text[n] != '\0' && text[n] != '\n') {
            preview[n] = text[n];
            ++n;
        }
        preview[n] = '\0';
    }
    char details[384];
    snprintf(details, sizeof(details),
             "method=%s textLen=%u size=%.1f lines=%d offsetX=%.1f offsetY=%.1f "
             "boundsW=%.1f boundsH=%.1f pixelW=%.1f pixelH=%.1f texId=%d "
             "texW=%d texH=%d text=\"%s\"",
             what, static_cast<unsigned>(textLen), sizePx,
             facade != nullptr ? facade->numLines : 0,
             facade != nullptr ? facade->offsetX : 0.f,
             facade != nullptr ? facade->offsetY : 0.f,
             facade != nullptr ? facade->boundsW : 0.f,
             facade != nullptr ? facade->boundsH : 0.f,
             facade != nullptr ? facade->pixelBoundsW : 0.f,
             facade != nullptr ? facade->pixelBoundsH : 0.f,
             facade != nullptr ? facade->texId : -1,
             facade != nullptr ? facade->texWidth : 0,
             facade != nullptr ? facade->texHeight : 0,
             preview);
    rr3::guestViewWideLog("glyph-vector", 0, 0, details);
}

// ---------------------------------------------------------------------------
// The four methods
// ---------------------------------------------------------------------------

// Shared body of init() and initWithParagraph().
//
// `paragraph` selects the layout: a single ink box for init(), a word-wrapped
// layout for initWithParagraph().
void renderText(uintptr_t self, uintptr_t fontHandle, const char* text,
                size_t textLen, float wrapWidth, float maxHeight, int breakStyle,
                int alignment, bool paragraph) {
    GlyphVectorFacade* facade = facadeFor(self);
    if (facade == nullptr || text == nullptr) {
        return;
    }

    FontRequest request{};
    fontRequest(fontHandle, &request);

    mutexLock(&g_glyphMutex);
    if (g_library == nullptr && FT_Init_FreeType(&g_library) != 0) {
        g_library = nullptr;
    }
    FT_Face face = nullptr;
    FT_Face fallback = nullptr;
    if (g_library != nullptr) {
        face = openFaceFor(request.path, request.bold);
        if (face != nullptr) {
            const FT_Error sizeError = sizeFace(face, request.sizePx);
            noteFace(request.path, face, 0, request.sizePx, sizeError);
        }
        fallback = cjkFace();
        if (fallback != nullptr) {
            const FT_Error sizeError = sizeFace(fallback, request.sizePx);
            noteFace(kCjkFacePath, fallback, 0, request.sizePx, sizeError);
        }
    }

    Layout layout{};
    if (face != nullptr) {
        layoutParagraph(text, textLen, face, fallback, request.widthScale,
                        paragraph ? wrapWidth : 0.f, alignment, &layout);
    }
    const Metrics metrics = layout.metrics;
    float lineHeight = metrics.ascent + metrics.descent;
    if (lineHeight <= 0.f) {
        lineHeight = request.sizePx > 0.f ? request.sizePx : 1.f;
    }

    // The paragraph box: [minLeft, maxRight] x [0, numLines * lineHeight].
    float boundsLeft = 0.f;
    float boundsRight = 0.f;
    float inkLeft = 0.f;
    float inkTop = 0.f;
    float inkRight = 0.f;
    float inkBottom = 0.f;
    bool hasInk = false;
    float lastLineInkBottom = 0.f;
    size_t usedLines = layout.lineCount;

    // breakStyle 2 (Clip) and 6 (WordWrapTruncate) drop the lines that do
    // not fit the height the guest gave. breakStyle 3/4/5 truncate the text
    // instead, which the port approximates with the same clip. breakStyle 0
    // (WordWrap) and 1 (CharWrap, which the Java falls back to WordWrap for)
    // do not clip at all.
    if (paragraph && maxHeight > 0.f && breakStyle != 0 && breakStyle != 1) {
        const int maxLines = static_cast<int>(maxHeight / lineHeight);
        if (maxLines > 0 && static_cast<size_t>(maxLines) < usedLines) {
            usedLines = static_cast<size_t>(maxLines);
        }
    }

    for (size_t l = 0; l < usedLines; ++l) {
        const Line& line = layout.lines[l];
        const float baseline = static_cast<float>(l) * lineHeight + metrics.ascent;
        const float right = line.left + line.width;
        if (l == 0) {
            boundsLeft = line.left;
            boundsRight = right;
        } else {
            if (line.left < boundsLeft) boundsLeft = line.left;
            if (right > boundsRight) boundsRight = right;
        }
        if (line.hasInk) {
            const float lx = line.inkLeft + line.left;
            const float rx = line.inkRight + line.left;
            const float ty = line.inkTop + baseline;
            const float by = line.inkBottom + baseline;
            if (!hasInk) {
                inkLeft = lx;
                inkRight = rx;
                inkTop = ty;
                inkBottom = by;
                hasInk = true;
            } else {
                if (lx < inkLeft) inkLeft = lx;
                if (rx > inkRight) inkRight = rx;
                if (ty < inkTop) inkTop = ty;
                if (by > inkBottom) inkBottom = by;
            }
            lastLineInkBottom = line.inkBottom;
        }
    }

    if (paragraph) {
        facade->numLines = static_cast<int32_t>(usedLines);
        facade->offsetX = 0.f;
        facade->offsetY = lastLineInkBottom > 0.f ? lastLineInkBottom : 0.f;
        facade->boundsW = boundsRight - boundsLeft;
        facade->boundsH = lineHeight * static_cast<float>(usedLines);
        facade->pixelBoundsW = hasInk ? inkRight - inkLeft : 0.f;
        facade->pixelBoundsH = hasInk ? inkBottom - inkTop : 0.f;
    } else {
        // init(): the ink box of the whole string, grown by one pixel at the
        // top and one at the bottom, exactly as GlyphVector.java does.
        //
        // Android's Paint.getTextBounds() is BASELINE RELATIVE: bounds.top is
        // the distance from the baseline UP to the ink (negative) and
        // bounds.bottom the distance DOWN to the ink (positive). The loop
        // above works in paragraph-box coordinates, where the first line's
        // baseline sits at metrics.ascent, so the baseline has to be taken
        // back off here.
        //
        // The Java then does:
        //     rect.top--;  i = rect.bottom + 1;  rect.bottom = i;
        //     offsetX = -rect.left;  offsetY = -i;
        //     boundsW = rect.width();  boundsH = rect.height();
        // so offsetY is -(descent + 1), NOT -(baseline + descent + 1). The
        // port measured the paragraph-box bottom instead, which put offsetY
        // one whole ASCENT away from the Java's value -- a 124 px shift for a
        // 175 px font, which moves the label off the widget it belongs to.
        const float baseline = metrics.ascent;
        const float left = hasInk ? inkLeft : 0.f;
        const float top = hasInk ? inkTop - baseline : 0.f;
        const float bottom = hasInk ? inkBottom - baseline : 0.f;
        const float grownTop = top - 1.f;
        const float grownBottom = bottom + 1.f;
        facade->numLines = 1;
        facade->offsetX = -left;
        facade->offsetY = -grownBottom;
        facade->boundsW = hasInk ? inkRight - inkLeft : 0.f;
        facade->boundsH = grownBottom - grownTop;
        facade->pixelBoundsW = facade->boundsW;
        facade->pixelBoundsH = facade->boundsH;
    }

    // The raster. The texture is a power of two, as the Java's is, and the
    // text is drawn into its top-left corner.
    const int textureWidth =
        nextPowerOfTwo(static_cast<int>(facade->boundsW + 0.5f));
    const int textureHeight =
        nextPowerOfTwo(static_cast<int>(facade->boundsH + 0.5f));

    // Keep the text, the font parameters and the layout, so that
    // createTexture() can rasterise exactly what was measured. The Java does
    // the same: createTexture() reads m_text, m_paint, m_layout and m_bounds.
    RenderState* state = acquireState(self);
    if (state != nullptr) {
        char* copy = static_cast<char*>(malloc(textLen + 1));
        if (copy != nullptr) {
            memcpy(copy, text, textLen);
            copy[textLen] = '\0';
            free(state->text);
            state->text = copy;
            state->textLen = textLen;
        }
        state->sizePx = request.sizePx;
        state->widthScale = request.widthScale;
        state->bold = request.bold;
        snprintf(state->fontPath, sizeof(state->fontPath), "%s", request.path);
        state->layout = layout;
        state->hasLayout = true;
        state->plan.paragraph = paragraph;
        state->plan.lineHeight = lineHeight;
        state->plan.ascent = metrics.ascent;
        state->plan.boundsLeft = boundsLeft;
        state->plan.pixelTopOffset = hasInk ? inkTop : 0.f;
        state->plan.singleOriginX = facade->offsetX;
        // drawText(str, -rect.left, -rect.top, paint): the BASELINE goes at
        // -rect.top in the bitmap, and rect.top is the baseline-relative ink
        // top MINUS one (the Java grows the box by a pixel at the top).
        //
        // The baseline-relative ink top is inkTop - ascent, because inkTop
        // above is the paragraph-box coordinate. Using the paragraph-box
        // value here put the baseline 123 px ABOVE the texture for a 175 px
        // font, and every row of the glyph was clipped away: the texture was
        // uploaded with no ink in it at all.
        state->plan.singleOriginY = 1.f - (hasInk ? inkTop - metrics.ascent : 0.f);
        state->plan.textureWidth = textureWidth;
        state->plan.textureHeight = textureHeight;
        state->plan.usedLines = usedLines;
    }

    // The Java's init() does NOT create the texture: createTexture() does,
    // and until then texId is still the field's initial value. The guest
    // reads texId back right after init() (guest 0x12a761c), so answering a
    // real texture id here would be a visible difference from the game.
    facade->texId = -1;
    facade->texWidth = 0;
    facade->texHeight = 0;

    mutexUnlock(&g_glyphMutex);
    noteGlyphVector(paragraph ? "initWithParagraph" : "init", facade, textLen, request.sizePx, text);
}

// Rasterises the measured layout and uploads it as a GL texture.
//
// This is the Java's createTexture(): a power-of-two texture holding the
// text, with the coverage in the alpha channel. It reads the layout the
// init call measured, exactly as the Java reads m_layout and m_bounds.
//
// The caller holds g_glyphMutex: the FreeType face and the shared raster
// buffers are not thread-safe.
bool uploadTexture(uintptr_t self) {
    GlyphVectorFacade* facade = facadeFor(self);
    if (facade == nullptr) {
        return false;
    }
    RenderState* state = stateFor(self);
    if (state == nullptr || !state->hasLayout || state->text == nullptr) {
        return false;
    }
    if (g_library == nullptr) {
        return false;
    }
    FT_Face face = openFaceFor(state->fontPath, state->bold);
    if (face == nullptr) {
        return false;
    }
    {
        const FT_Error sizeError = sizeFace(face, state->sizePx);
        noteFace(state->fontPath, face, 0, state->sizePx, sizeError);
    }
    FT_Face fallback = cjkFace();
    if (fallback != nullptr) {
        const FT_Error sizeError = sizeFace(fallback, state->sizePx);
        noteFace(kCjkFacePath, fallback, 0, state->sizePx, sizeError);
    }

    const RenderPlan& plan = state->plan;
    const int textureWidth = plan.textureWidth;
    const int textureHeight = plan.textureHeight;
    if (textureWidth <= 0 || textureHeight <= 0) {
        return false;
    }
    uint8_t* coverage =
        growBuffer(g_coverage, g_coverageCapacity,
                   static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight));
    if (coverage == nullptr) {
        return false;
    }
    if (plan.paragraph) {
        // The layout is drawn with the paragraph origin at
        // (boundsLeft, pixelTopOffset); pixelTopOffset is the ink top of the
        // first line, so the ink starts at the top of the texture.
        for (size_t l = 0; l < plan.usedLines; ++l) {
            const Line& line = state->layout.lines[l];
            const float baseline = static_cast<float>(l) * plan.lineHeight +
                                   plan.ascent - plan.pixelTopOffset;
            rasterizeLine(face, fallback, state->text + line.start,
                          line.end - line.start, state->widthScale,
                          line.left - plan.boundsLeft, baseline, textureWidth,
                          textureHeight, coverage, l == 0);
        }
    } else {
        // drawText(str, -rect.left, -rect.top, paint): the baseline sits one
        // pixel below the top of the grown ink box, and the pen starts at
        // -rect.left.
        rasterizeLine(face, fallback, state->text, state->textLen,
                      state->widthScale, plan.singleOriginX, plan.singleOriginY,
                      textureWidth, textureHeight, coverage, true);
    }

    // White with the coverage in the alpha channel, which is what the ARGB_4444
    // bitmap the Java builds amounts to.
    GLuint texture = 0;
    ::glGenTextures(1, &texture);
    if (texture == 0) {
        facade->texId = -1;
        return false;
    }
    uint8_t* texels =
        growBuffer(g_texels, g_texelCapacity,
                   static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight) * 4);
    if (texels == nullptr) {
        ::glDeleteTextures(1, &texture);
        facade->texId = -1;
        return false;
    }
    const size_t pixels =
        static_cast<size_t>(textureWidth) * static_cast<size_t>(textureHeight);
    // The texture is BLACK where there is no ink, and white where there is.
    //
    // The Java's bitmap is created with eraseColor(argb(0,0,0,0)) and then
    // the text is drawn with a white paint, so the empty pixels are BLACK
    // with alpha 0 and the drawn pixels are WHITE with the coverage in the
    // alpha. The guest's text shader reads the RGB, so a texture that is
    // white everywhere draws a solid white block instead of the glyphs --
    // which is exactly what the first runs showed.
    size_t inked = 0;
    for (size_t p = 0; p < pixels; ++p) {
        const uint8_t alpha = coverage[p];
        const uint8_t white = alpha != 0 ? 255 : 0;
        texels[p * 4 + 0] = white;
        texels[p * 4 + 1] = white;
        texels[p * 4 + 2] = white;
        texels[p * 4 + 3] = alpha;
        if (alpha != 0) {
            ++inked;
        }
    }
    if (inked == 0) {
        rr3::guestViewWideLog("glyph-blank", 0, 0,
                               "texture has NO ink: the raster produced an empty "
                               "coverage map");
    } else if (inked == pixels) {
        rr3::guestViewWideLog("glyph-blank", 0, 0,
                               "texture is FULLY covered: the raster filled the "
                               "whole bitmap");
    }
    {
        static char lastTexKey[48] = {0};
        char texKey[48];
        snprintf(texKey, sizeof(texKey), "%dx%d", textureWidth, textureHeight);
        if (strcmp(lastTexKey, texKey) != 0) {
            snprintf(lastTexKey, sizeof(lastTexKey), "%s", texKey);
            char details[160];
            snprintf(details, sizeof(details),
                     "texture=%s inked=%zu of %zu (%.1f%%) texId=%d",
                     texKey, inked, pixels,
                     pixels != 0 ? 100.0 * static_cast<double>(inked) /
                                       static_cast<double>(pixels)
                                 : 0.0,
                     static_cast<int>(texture));
            rr3::guestViewWideLog("glyph-tex", 0, 0, details);
        }
    }
    ::glBindTexture(GL_TEXTURE_2D, texture);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureWidth, textureHeight, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, texels);
    facade->texId = static_cast<int32_t>(texture);
    facade->texWidth = textureWidth;
    facade->texHeight = textureHeight;
    // Tell the text-draw probe which texture is the label texture, so it can
    // tell the label's draw apart from the game's other 40,000 draws.
    rr3::gfxTextProbeNoteTexture(texture, textureWidth, textureHeight);
    return true;
}

// The ManagedMethod entry points.
//
// The argument types are the JNI ones, so the array-form dispatcher can read
// them out of the jvalue array the bridge builds from the guest's own
// variadic block.

void gv_init(JNIEnv*, jobject self, jobject font, jstring text) {
    const char* utf8 = rr3::glyphStringText(reinterpret_cast<uintptr_t>(text));
    renderText(reinterpret_cast<uintptr_t>(self),
               reinterpret_cast<uintptr_t>(font), utf8, strlen(utf8), 0.f, 0.f, 0,
               0, false);
}

void gv_initWithParagraph(JNIEnv*, jobject self, jobject font, jstring text,
                           jfloat wrapWidth, jfloat maxHeight, jint breakStyle,
                           jint alignment) {
    const char* utf8 = rr3::glyphStringText(reinterpret_cast<uintptr_t>(text));
    renderText(reinterpret_cast<uintptr_t>(self),
               reinterpret_cast<uintptr_t>(font), utf8, strlen(utf8), wrapWidth,
               maxHeight, breakStyle, alignment, true);
}

jboolean gv_createTexture(JNIEnv*, jobject self) {
    const uintptr_t object = reinterpret_cast<uintptr_t>(self);
    mutexLock(&g_glyphMutex);
    const bool ok = uploadTexture(object);
    GlyphVectorFacade* facade = facadeFor(object);
    mutexUnlock(&g_glyphMutex);
    if (ok && facade != nullptr) {
        noteGlyphVector("createTexture", facade, 0, 0.f, "");
    }
    return ok ? 1 : 0;
}

jboolean gv_renderToTexture(JNIEnv*, jobject self, jint texId, jint width,
                             jint height, jfloat scale) {
    GlyphVectorFacade* facade = facadeFor(reinterpret_cast<uintptr_t>(self));
    if (facade == nullptr || texId <= 0 || width <= 0 || height <= 0) {
        return 0;
    }
    mutexLock(&g_glyphMutex);
    RenderState* state = stateFor(reinterpret_cast<uintptr_t>(self));
    if (state == nullptr || state->text == nullptr) {
        mutexUnlock(&g_glyphMutex);
        return 0;
    }
    FT_Face face = nullptr;
    if (g_library != nullptr) {
        face = openFaceFor(state->fontPath, state->bold);
        if (face != nullptr) {
            const FT_Error sizeError = sizeFace(face, state->sizePx);
            noteFace(state->fontPath, face, 0, state->sizePx, sizeError);
        }
    }
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    uint8_t* coverage =
        growBuffer(g_coverage, g_coverageCapacity,
                   static_cast<size_t>(w) * static_cast<size_t>(h));
    if (coverage == nullptr || face == nullptr) {
        mutexUnlock(&g_glyphMutex);
        return 0;
    }
    // The Java centres the text and scales it; the canvas is flipped, so the
    // baseline of the single line sits at the vertical centre.
    const Metrics metrics = metricsOf(face);
    const float originX = (static_cast<float>(w) - facade->boundsW * scale) * 0.5f;
    const float originY =
        (static_cast<float>(h) + static_cast<float>(metrics.ascent) * scale) * 0.5f;
    rasterizeLine(face, nullptr, state->text, state->textLen, state->widthScale,
                  originX, originY, w, h, coverage, true);
    ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texId));
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_ALPHA, GL_UNSIGNED_BYTE,
                      coverage);
    mutexUnlock(&g_glyphMutex);
    return 1;
}

const ManagedMethod g_glyphVectorMethods[] = {
    ManagedMethod::Register<&gv_init>(ClassGlyphVector, "init",
                                       "(Lcom/firemint/realracing/Font;Ljava/lang/String;)V"),
    ManagedMethod::Register<&gv_initWithParagraph>(ClassGlyphVector,
                                                    "initWithParagraph",
                                                    "(Lcom/firemint/realracing/Font;Ljava/lang/String;FFII)V"),
    ManagedMethod::Register<&gv_createTexture>(ClassGlyphVector, "createTexture",
                                               "()Z"),
    ManagedMethod::Register<&gv_renderToTexture>(ClassGlyphVector,
                                                  "renderToTexture", "(IIIF)Z"),
    {NULL},
};

}  // namespace
}  // namespace glyphvector

// The Class the ManagedMethod table refers to. Only its address is taken by
// the table, so the static initialisation order does not matter.
Class ClassGlyphVector = {
    .classpath = "com/firemint/realracing/GlyphVector",
    .classname = "GlyphVector",
    .managed_methods = glyphvector::g_glyphVectorMethods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(GlyphVectorFacade),
};

namespace glyphvector {
namespace {

const int g_glyphVectorRegistered =
    ClassRegistry::register_class(ClassGlyphVector);

}  // namespace

bool onNewObject(uintptr_t classHandle, uintptr_t object) {
    if (classHandle == 0 || object == 0) {
        return false;
    }
    if (!isGlyphVectorClass(classHandle)) {
        return false;
    }
    GlyphVectorFacade* facade = reinterpret_cast<GlyphVectorFacade*>(object);
    memset(facade, 0, sizeof(*facade));
    facade->magic = kGlyphVectorMagic;
    facade->stateIndex = -1;
    facade->texId = -1;
    return true;
}

bool readIntField(uintptr_t object, const char* name, int32_t* out) {
    const GlyphVectorFacade* facade = facadeFor(object);
    if (facade == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    if (strcmp(name, "texId") == 0) {
        *out = facade->texId;
        return true;
    }
    if (strcmp(name, "texWidth") == 0) {
        *out = facade->texWidth;
        return true;
    }
    if (strcmp(name, "texHeight") == 0) {
        *out = facade->texHeight;
        return true;
    }
    if (strcmp(name, "numLines") == 0) {
        *out = facade->numLines;
        return true;
    }
    return false;
}

bool readFloatField(uintptr_t object, const char* name, float* out) {
    const GlyphVectorFacade* facade = facadeFor(object);
    if (facade == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    if (strcmp(name, "offsetX") == 0) {
        *out = facade->offsetX;
        return true;
    }
    if (strcmp(name, "offsetY") == 0) {
        *out = facade->offsetY;
        return true;
    }
    if (strcmp(name, "boundsW") == 0) {
        *out = facade->boundsW;
        return true;
    }
    if (strcmp(name, "boundsH") == 0) {
        *out = facade->boundsH;
        return true;
    }
    if (strcmp(name, "pixelBoundsW") == 0) {
        *out = facade->pixelBoundsW;
        return true;
    }
    if (strcmp(name, "pixelBoundsH") == 0) {
        *out = facade->pixelBoundsH;
        return true;
    }
    return false;
}

bool isGlyphVectorClass(uintptr_t classHandle) {
    if (classHandle == 0) {
        return false;
    }
    const char* name = rr3::glyphClassName(classHandle);
    if (name == nullptr) {
        return false;
    }
    const char* leaf = name;
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '/') {
            leaf = p + 1;
        }
    }
    return strcmp(leaf, "GlyphVector") == 0;
}

void fontRequest(uintptr_t fontHandle, FontRequest* out) {
    if (out == nullptr) {
        return;
    }
    out->sizePx = 48.f;
    out->widthScale = 1.f;
    out->bold = false;
    out->path[0] = '\0';
    if (fontHandle == 0) {
        return;
    }
    // The bridge captured the real Font.init arguments; see the Font table
    // in jni_bridge.cpp.
    rr3::glyphFontRequest(fontHandle, &out->sizePx, &out->widthScale, &out->bold,
                     out->path, sizeof(out->path));
}

}  // namespace glyphvector
