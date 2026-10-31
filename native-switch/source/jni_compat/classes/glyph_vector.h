#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../jni.h"
#include "../jni_internals.h"

/*
 * com.firemint.realracing.GlyphVector -- the class the guest's PARAGRAPH text
 * goes through.
 *
 * WHERE THE CONTRACT COMES FROM
 * -----------------------------
 * The guest binary is the oracle. Every signature below was read out of
 * libRealRacing3.so, not guessed:
 *
 *   FindClass "com/firemint/realracing/GlyphVector"
 *       guest .rodata 0x39910d, materialised by the adrp/add pair at guest
 *       0x12a731c/0x12a7320 inside the FDE [0x12a730c, 0x12a7510) -- the
 *       guest's GlyphVector binding setup.
 *
 *   GetMethodID "<init>"  "()V"
 *       guest .rodata 0x3cd28a / 0x3fd87a, reached through the guest's
 *       find-class-plus-NewObject helper at guest 0x12b60cc (the GetMethodID
 *       is at 0x12b611c, the NewObjectV at 0x12b6150).
 *
 *   GetMethodID "init"  "(Lcom/firemint/realracing/Font;Ljava/lang/String;)V"
 *       guest .rodata 0x443042 / 0x48e838; the id lands in the guest's
 *       binding struct at +0x50. The call is guest 0x12a7510, which issues
 *       JNIEnv::CallVoidMethodV (table slot 62) with the Font jobject in x3
 *       and the String jstring in x4.
 *
 *   GetMethodID "initWithParagraph"
 *       "(Lcom/firemint/realracing/Font;Ljava/lang/String;FFII)V"
 *       guest .rodata 0x3e78fa / 0x418aeb; the id lands at +0x58. The call
 *       is guest 0x12a771c: x3 = Font jobject, x4 = String jstring,
 *       q0/q1 = the two floats, x5/x6 = the two ints.
 *
 *   GetMethodID "createTexture" "()Z"
 *       guest .rodata 0x3efe71 / 0x42f472; the id lands at +0x60. The call
 *       is guest 0x12a78e4, which issues CallBooleanMethodV (table slot 38)
 *       through the guest's own boolean helper at 0x10e8f2c.
 *
 *   GetMethodID "renderToTexture" "(IIIF)Z"
 *       guest .rodata 0x3e790c / 0x475590; the id lands at +0x68.
 *
 *   GetFieldID, ten of them, read back right after the calls above:
 *       texId(I) 0x43a9fc -> +0x70      texWidth(I) 0x3cd231 -> +0x78
 *       texHeight(I) 0x475598 -> +0x80   offsetX(F) 0x3efe7f -> +0x88
 *       offsetY(F) 0x421473 -> +0x90     boundsW(F) 0x43aa02 -> +0x98
 *       boundsH(F) 0x3efe87 -> +0xa0     pixelBoundsW(F) 0x429a73 -> +0xa8
 *       pixelBoundsH(F) 0x4322ee -> +0xb0 numLines(I) 0x3e791c -> +0xb8
 *
 * The Java side of the class is not invented either: it is
 * reverse/java/sources/com/firemint/realracing/GlyphVector.java in the guest
 * APK, and the field list above matches that file exactly (plus
 * pixelTopOffset, which the guest never reads).
 *
 * WHY THIS CLASS IS A FACADE AND NOT A PORT OF THE JAVA
 * ------------------------------------------------------
 * There is no JVM on this port. The guest's Android build runs GlyphVector as
 * ART bytecode; here the class body has to be C++ that measures the text,
 * lays it out, rasterises it and uploads a GL texture -- i.e. the port has to
 * stand in for android.text.StaticLayout, android.graphics.Paint and
 * android.opengl.GLUtils at once.
 *
 * The instance state therefore lives in the port's fake-object pool (the
 * bridge hands out one pool slot per NewObjectV), and the ten Java-visible
 * fields are stored verbatim so GetIntField / GetFloatField can answer them
 * from the same struct.
 */

/*
 * The facade, laid out to live in the port's fake-object pool slot.
 *
 * It is a POD on purpose: the pool MEMSETS a slot to zero when it hands it
 * out, so a non-trivial member would be zeroed rather than constructed. The
 * magic is stamped by the bridge the moment NewObjectV produces the object.
 */
struct GlyphVectorFacade {
    uint32_t magic;      // 'GLVP'
    int32_t stateIndex;  // index into the render-state table, -1 = none
    // The ten fields the guest reads back, in the order it reads them.
    float offsetX;
    float offsetY;
    float boundsW;
    float boundsH;
    float pixelBoundsW;
    float pixelBoundsH;
    int32_t numLines;
    int32_t texId;
    int32_t texWidth;
    int32_t texHeight;
};

namespace glyphvector {

constexpr uint32_t kGlyphVectorMagic = 0x50564C47;  // GLVP

/*
 * Called by the bridge right after NewObjectV handed out an object. Returns
 * false for every class that is not GlyphVector, so the bridge can call it
 * unconditionally.
 */
bool onNewObject(uintptr_t classHandle, uintptr_t object);

/*
 * The ten field reads. The bridge asks with the field name the guest used;
 * each returns false when the object is not a GlyphVector or the name is not
 * one of the ten, so the caller can fall through to its other answers.
 */
bool readIntField(uintptr_t object, const char* name, int32_t* out);
bool readFloatField(uintptr_t object, const char* name, float* out);

/*
 * True when the class handle names GlyphVector. The bridge uses it to keep
 * the class out of the shared-sentinel path.
 */
bool isGlyphVectorClass(uintptr_t classHandle);

/*
 * The Font's real parameters.
 *
 * The guest's Font.init(String path, boolean bold, boolean italic, float
 * size, float widthScale) is the only place the text size comes from, and
 * the port's Font used to answer a synthetic 48 px for every Font object.
 * The bridge captures the real arguments and hands them over here.
 */
struct FontRequest {
    float sizePx;
    float widthScale;
    bool bold;
    char path[96];
};

void fontRequest(uintptr_t fontHandle, FontRequest* out);

/*
 * The metrics of a REAL face at a REAL size, in the convention the guest's
 * Font class uses.
 *
 * WHY THE BRIDGE NEEDS THIS
 * -------------------------
 * com.firemint.realracing.Font.init() builds a Paint with the typeface at the
 * requested size and then reads Paint.getFontMetrics() (reverse/java/sources/
 * com/firemint/realracing/Font.java:80-88).  The guest's own layout reads
 * those fields back, so the port has to answer them with the numbers the Java
 * would have produced -- for the face the guest NAMED and the size it asked
 * for, not with a constant.
 *
 * THE SIGNS, which are the whole point
 * ------------------------------------
 * Android's Paint.FontMetrics is documented as: ascent and top NEGATIVE,
 * descent and bottom POSITIVE, leading POSITIVE.  Font.java then stores
 *
 *     this.ascent  = -fontMetrics.ascent;   -> POSITIVE, px above the baseline
 *     this.descent =  fontMetrics.descent;   -> POSITIVE, px below the baseline
 *     this.height  =  this.ascent + this.descent;
 *     this.top     = -fontMetrics.top;      -> POSITIVE
 *     this.bottom  =  fontMetrics.bottom;   -> POSITIVE
 *     this.leading =  fontMetrics.leading;   -> POSITIVE
 *
 * The port used to answer a synthetic 48 px set in which DESCENT WAS NEGATIVE
 * (-9), so height came out as 39 + (-9) = 30 instead of 48, and the guest
 * sized its labels against a font that was 38% shorter than the one the
 * paragraph was actually laid out with.
 */
struct FontMetrics {
    float ascent;   // px above the baseline (positive)
    float descent;  // px below the baseline (positive)
    float height;   // ascent + descent -- the game's Font.height
    float top;      // px above the baseline to the font box top
    float bottom;   // px below the baseline to the font box bottom
    float leading;  // the extra leading the face asks for (positive)
};

/*
 * The metrics of the face the guest named, at the size it asked for. Returns
 * false when the face cannot be opened, in which case the caller keeps its
 * own fallback.
 */
bool fontMetricsFor(const char* path, bool bold, float sizePx, FontMetrics* out);

}  // namespace glyphvector

/*
 * Provided by jni_bridge.cpp: the Font object's captured Font.init arguments.
 * The Font class is a Java class in the guest APK, so its typeface, size and
 * width scale only exist there -- and GlyphVector reads them from the Font
 * object it is handed.
 */
namespace rr3 {
void glyphFontRequest(uintptr_t fontHandle, float* sizePx, float* widthScale,
                      bool* bold, char* path, size_t pathCapacity);

// The name behind a class handle, and the text behind a jstring. The port
// has no ART runtime, so the jstring table and the class table are the only
// copies of either.
const char* glyphClassName(uintptr_t handle);
const char* glyphStringText(uintptr_t string);
}  // namespace rr3
