// The text-draw probe -- see include/rr3/gfx_text_probe.hpp.
//
// WHY IT EXISTS
// -------------
// The port can prove that it uploaded a glyph texture; it could not prove that
// the guest SAMPLES that texture where the ink is. The guest's label quad is
// four vertices in memory the guest owns, and the port had no way to see the
// UV rectangle those vertices carried.
//
// This file closes that gap by reading the GL traffic:
//
//   * the CLIENT-SIDE vertex arrays the guest hands to glVertexAttribPointer,
//   * the vertex attribute setup at the moment of the draw,
//   * the bound texture, the program, the MVP, the blend/depth/scissor state,
//   * and the pixels the draw actually left in the framebuffer.
//
// The measurement is what says WHY the label is invisible. Guessing does not.
//
// WHAT IS MEASURED, AND WHY EACH PIECE IS HERE
// ---------------------------------------------
//   a_pos / a_uv / a_colour
//       The quad. A label that is drawn off-screen, degenerate, or with a
//       zero alpha is invisible BY CONSTRUCTION, and each of those has a
//       different signature in the vertex data.
//
//   the bound texture id
//       If it is not the texture the glyph facade created, the guest is
//       drawing something else and the glyph texture is never sampled.
//
//   the viewport / scissor / FBO
//       A quad outside the viewport, or clipped away by a scissor, or drawn
//       into a framebuffer nobody presents, does not reach the screen.
//
//   the blend state
//       SRC_ALPHA with a zero alpha source, or a blend equation that
//       discards the source, hides a perfectly good quad.
//
//   the pixel readback
//       The only measurement that cannot lie about the question "did this
//       draw reach the screen".
//
// TWO INDEPENDENT ANSWERS TO "WHERE IS THE TEXT?"
// ------------------------------------------------
// The quad is reported from the CLIENT MEMORY the guest handed to
// glVertexAttribPointer, and the pixels are reported from the framebuffer
// AFTER the draw. If the two agree, the answer is measured, not inferred.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GLES2/gl2.h>

#include "rr3/gfx_text_probe.hpp"
#include "rr3/guest_view_trace.hpp"

namespace rr3 {
namespace {

// GLES2/gl2.h does not name GL_HALF_FLOAT, but the guest uses it: the vertex
// buffer holds the label quad's corners as half floats.
constexpr GLenum kGlHalfFloat = 0x140B;

// The devkitPro GLES2 header does not declare the level query, and GLES2 asks
// the size of a texture through it rather than through glGetTexParameteriv.
constexpr GLenum kGlTextureWidth = 0x1000;
constexpr GLenum kGlTextureHeight = 0x1001;
extern "C" void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname,
                                         GLint* params);

// ---------------------------------------------------------------------------
// The glyph textures
// ---------------------------------------------------------------------------

constexpr int kGlyphTextureSlots = 4096;

struct GlyphTexture {
    GLuint id;
    int width;
    int height;
};

GlyphTexture g_glyphTextures[kGlyphTextureSlots];
int g_glyphTextureCount = 0;

bool isGlyphTexture(GLuint id) {
    if (id == 0) {
        return false;
    }
    for (int index = 0; index < g_glyphTextureCount; ++index) {
        if (g_glyphTextures[index].id == id) {
            return true;
        }
    }
    return false;
}

const GlyphTexture* glyphTexture(GLuint id) {
    for (int index = 0; index < g_glyphTextureCount; ++index) {
        if (g_glyphTextures[index].id == id) {
            return &g_glyphTextures[index];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The client-side vertex arrays
// ---------------------------------------------------------------------------
//
// The guest hands glVertexAttribPointer a POINTER into its own memory, and
// that memory is where the quad's vertices live. This ring keeps the last few
// descriptions, newest last, so a draw can be matched with the array setup
// that belongs to it.
//
// The match is not blind: the setup is only used when its size/type/stride
// agree with what the driver reports for the same attribute index at draw
// time. A disagreement is reported rather than hidden, because "the guest
// changed the array between the setup and the draw" is itself a finding.

constexpr int kSetupSlots = 32;

struct AttribSetup {
    unsigned long long seq;
    GLuint index;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    uintptr_t pointer;
    GLuint buffer;
};

AttribSetup g_setups[kSetupSlots];
int g_setupNext = 0;
unsigned long long g_setupSeq = 0;

// ---------------------------------------------------------------------------
// The buffer shadow
// ---------------------------------------------------------------------------
//
// GLES2 has no glGetBufferSubData, so the only way to see what the guest is
// about to draw from a vertex buffer object is to remember what it uploaded.
// Four slots is enough: the guest keeps one vertex buffer and one index
// buffer live at a time, and the shadow is a copy of the bytes, not of the
// GPU's storage.

constexpr int kBufferSlots = 4;
constexpr size_t kBufferCapacity = 512 * 1024;

unsigned long long g_shadowBytes = 0;
unsigned long long g_shadowCalls = 0;

struct BufferShadow {
    GLuint id;
    size_t size;
    uint8_t* data;
};

BufferShadow g_buffers[kBufferSlots];
GLuint g_boundArrayBuffer = 0;

BufferShadow* shadowFor(GLuint id) {
    for (int index = 0; index < kBufferSlots; ++index) {
        if (g_buffers[index].id == id && g_buffers[index].data != nullptr) {
            return &g_buffers[index];
        }
    }
    for (int index = 0; index < kBufferSlots; ++index) {
        if (g_buffers[index].data == nullptr) {
            g_buffers[index].id = id;
            g_buffers[index].size = 0;
            return &g_buffers[index];
        }
    }
    // Every slot is taken: reuse the first.
    g_buffers[0].id = id;
    g_buffers[0].size = 0;
    return &g_buffers[0];
}

const BufferShadow* shadowById(GLuint id) {
    if (id == 0) {
        return nullptr;
    }
    for (int index = 0; index < kBufferSlots; ++index) {
        if (g_buffers[index].id == id && g_buffers[index].data != nullptr) {
            return &g_buffers[index];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Decoding a vertex attribute
// ---------------------------------------------------------------------------

float halfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1F;
    uint32_t mantissa = half & 0x03FF;
    uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: renormalise.
            exponent = 1;
            while ((mantissa & 0x0400) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FF;
            bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float value = 0.f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

// Reads one attribute component out of a byte range.
float readComponent(const uint8_t* base, size_t baseSize, size_t offset, GLenum type,
                    bool normalized, int component) {
    if (base == nullptr) {
        return 0.f;
    }
    size_t elementSize = 0;
    switch (type) {
        case GL_FLOAT:
            elementSize = 4;
            break;
        case kGlHalfFloat:
            elementSize = 2;
            break;
        case GL_UNSIGNED_BYTE:
        case GL_BYTE:
            elementSize = 1;
            break;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
            elementSize = 2;
            break;
        default:
            return 0.f;
    }
    const size_t at = offset + static_cast<size_t>(component) * elementSize;
    if (at + elementSize > baseSize) {
        return 0.f;
    }
    float value = 0.f;
    switch (type) {
        case GL_FLOAT: {
            uint32_t bits = 0;
            memcpy(&bits, base + at, 4);
            memcpy(&value, &bits, 4);
            return value;
        }
        case kGlHalfFloat: {
            uint16_t half = 0;
            memcpy(&half, base + at, 2);
            return halfToFloat(half);
        }
        case GL_UNSIGNED_BYTE: {
            const uint8_t raw = base[at];
            return normalized ? raw / 255.f : static_cast<float>(raw);
        }
        case GL_BYTE: {
            const int8_t raw = static_cast<int8_t>(base[at]);
            return normalized ? static_cast<float>(raw) / 127.f : static_cast<float>(raw);
        }
        case GL_UNSIGNED_SHORT: {
            uint16_t raw = 0;
            memcpy(&raw, base + at, 2);
            return normalized ? raw / 65535.f : static_cast<float>(raw);
        }
        case GL_SHORT: {
            int16_t raw = 0;
            memcpy(&raw, base + at, 2);
            return normalized ? static_cast<float>(raw) / 32767.f : static_cast<float>(raw);
        }
        default:
            return 0.f;
    }
}

// ---------------------------------------------------------------------------
// The attribute state at the draw
// ---------------------------------------------------------------------------

struct AttribInfo {
    GLint index;
    GLint size;
    GLint type;
    GLint stride;
    GLint normalized;
    GLint bufferBinding;
    GLint pointer32;
    uintptr_t pointer64;
    bool present;
};

AttribInfo queryAttrib(GLuint index) {
    AttribInfo info{};
    info.index = static_cast<GLint>(index);
    info.present = false;
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_SIZE,
                           &info.size);
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_TYPE,
                           &info.type);
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_STRIDE,
                           &info.stride);
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                           &info.normalized);
    ::glGetVertexAttribiv(static_cast<GLuint>(index),
                           GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &info.bufferBinding);
    // The 32-bit query truncates the pointer on a 64-bit target, so the
    // 64-bit one is asked as well: both answers are reported, and the
    // difference between them is a finding in itself.
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_POINTER,
                           &info.pointer32);
    void* pointer = nullptr;
    ::glGetVertexAttribPointerv(static_cast<GLuint>(index),
                                 GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    info.pointer64 = reinterpret_cast<uintptr_t>(pointer);
    info.present = info.size > 0;
    return info;
}

// The newest setup for this attribute index that agrees with the driver's own
// view of the array; the newest one for the index if none agrees.
const AttribSetup* matchSetup(GLuint index, const AttribInfo& info) {
    const AttribSetup* newest = nullptr;
    const AttribSetup* agreeing = nullptr;
    for (int slot = 0; slot < kSetupSlots; ++slot) {
        const AttribSetup& setup = g_setups[slot];
        if (setup.seq == 0 || setup.index != index) {
            continue;
        }
        if (newest == nullptr || setup.seq > newest->seq) {
            newest = &setup;
        }
        if (setup.size == info.size && setup.type == static_cast<GLenum>(info.type) &&
            setup.stride == info.stride) {
            if (agreeing == nullptr || setup.seq > agreeing->seq) {
                agreeing = &setup;
            }
        }
    }
    return agreeing != nullptr ? agreeing : newest;
}

// Where the vertices of one attribute live right now.
//
// Client-side array: the guest's own memory at the pointer it handed to
// glVertexAttribPointer. Buffer object: the port's shadow of the upload.
struct VertexSource {
    const uint8_t* base = nullptr;
    size_t size = 0;
    uintptr_t address = 0;
    bool clientSide = false;
};

// A user-space address that the probe is willing to read. Anything outside it
// is reported as unreadable instead of being dereferenced.
bool readableAddress(uintptr_t address) {
    return address != 0 && address < 0x0000800000000000ull;
}

// The rectangle of the framebuffer the quad covers, clipped to the viewport.
// The clip is what makes a readback legal: a rectangle outside the
// framebuffer is a GL error and an undefined read.
bool readQuadRect(const GLint* rect, const GLint* viewport, int* width, int* height) {
    int w = rect[2];
    int h = rect[3];
    int x = rect[0];
    int y = rect[1];
    if (w > 256) w = 256;
    if (h > 64) h = 64;
    if (x < viewport[0]) {
        w += x - viewport[0];
        x = viewport[0];
    }
    if (y < viewport[1]) {
        h += y - viewport[1];
        y = viewport[1];
    }
    if (x + w > viewport[0] + viewport[2]) {
        w = viewport[0] + viewport[2] - x;
    }
    if (y + h > viewport[1] + viewport[3]) {
        h = viewport[1] + viewport[3] - y;
    }
    if (w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

// How many different colours are in a patch. A text patch has many, a flat
// background has one.
int countUniqueColours(const uint8_t* pixels, int count) {
    uint32_t colours[64];
    int unique = 0;
    for (int pixel = 0; pixel < count; ++pixel) {
        const uint32_t key = (static_cast<uint32_t>(pixels[pixel * 4 + 0]) << 16) |
                             (static_cast<uint32_t>(pixels[pixel * 4 + 1]) << 8) |
                             static_cast<uint32_t>(pixels[pixel * 4 + 2]);
        bool known = false;
        for (int seen = 0; seen < unique; ++seen) {
            if (colours[seen] == key) {
                known = true;
                break;
            }
        }
        if (!known && unique < 64) {
            colours[unique++] = key;
        }
    }
    return unique;
}

VertexSource sourceFor(const AttribSetup* setup) {
    VertexSource source;
    if (setup == nullptr) {
        return source;
    }
    if (setup->buffer != 0) {
        const BufferShadow* shadow = shadowById(setup->buffer);
        if (shadow == nullptr || shadow->data == nullptr) {
            return source;
        }
        const size_t offset = static_cast<size_t>(setup->pointer);
        if (offset >= shadow->size) {
            return source;
        }
        source.base = shadow->data + offset;
        source.size = shadow->size - offset;
        source.address = setup->pointer;
        return source;
    }
    // A client-side array. The pointer must look like a user-space address:
    // the guest's own heap lives there, and reading outside it would fault.
    if (!readableAddress(setup->pointer)) {
        return source;
    }
    source.base = reinterpret_cast<const uint8_t*>(setup->pointer);
    // The largest possible span for this attribute: count vertices of the
    // largest stride the guest could have set.
    source.size = 64 * 1024;
    source.address = setup->pointer;
    source.clientSide = true;
    return source;
}

// ---------------------------------------------------------------------------
// The pending report
// ---------------------------------------------------------------------------

constexpr int kMaxAttribs = 8;
constexpr int kMaxVertices = 8;

struct AttrReport {
    char name[64];
    GLint location;
    AttribInfo info;
    const AttribSetup* setup;
    VertexSource source;
    float values[kMaxVertices][4];
    int valueCount;
    int componentCount;
};

struct PendingReport {
    bool active;
    unsigned long long ordinal;
    GLuint texture;
    GLuint program;
    GLint viewport[4];
    GLint rect[4];
    bool onScreen;
    float ndc[kMaxVertices][2];
    int vertexCount;
    AttrReport attrs[kMaxAttribs];
    int attrCount;
    // The pixels of the quad's own rectangle, read BEFORE the draw.
    int beforeWhite;
    int beforeUnique;
    int beforeMaxR;
    int readWidth;
    int readHeight;
};

PendingReport g_pending;
unsigned long long g_drawOrdinal = 0;

// ---------------------------------------------------------------------------
// Reporting budgets
// ---------------------------------------------------------------------------
//
// The cap counts the LABEL draws only. Counting every draw would use the
// whole budget in the first few frames and the label would never be reported.
//
// The budget is spent by DISTINCT glyph textures, not by draws: the guest
// creates a new texture for every string instance (the measured run created
// ~1200 of them), and the first draw of a texture is the draw that puts that
// text on the screen. Reporting every draw of every texture instead would
// spend the whole budget in the front end, and the race HUD -- the thing the
// probe is for -- would never be reported at all.

constexpr int kTextDrawBudget = 3000;
constexpr int kFirstDrawBudget = 96;
constexpr int kProgramDrawBudget = 1200;
int g_textDraws = 0;
int g_firstDraws = 0;
int g_programDraws = 0;

// The set of glyph textures already reported. Open addressing, power of two.
constexpr size_t kSeenSlots = 8192;
GLuint g_seenTextures[kSeenSlots];
bool g_seenUsed[kSeenSlots];

bool firstDrawOfTexture(GLuint id) {
    if (id == 0) {
        return false;
    }
    const size_t start = (static_cast<size_t>(id) * 2654435761u) & (kSeenSlots - 1);
    for (size_t probe = 0; probe < kSeenSlots; ++probe) {
        const size_t at = (start + probe) & (kSeenSlots - 1);
        if (!g_seenUsed[at]) {
            g_seenUsed[at] = true;
            g_seenTextures[at] = id;
            return true;
        }
        if (g_seenTextures[at] == id) {
            return false;
        }
    }
    return false;
}

// The programs whose source has already been dumped. The shader source is
// what says what the program DOES with the quad, and it never changes, so it
// is written once per program.
constexpr size_t kProgramSlots = 256;
GLuint g_seenPrograms[kProgramSlots];
bool g_seenProgramUsed[kProgramSlots];

bool firstDrawOfProgram(GLuint id) {
    if (id == 0) {
        return false;
    }
    const size_t start = (static_cast<size_t>(id) * 2654435761u) & (kProgramSlots - 1);
    for (size_t probe = 0; probe < kProgramSlots; ++probe) {
        const size_t at = (start + probe) & (kProgramSlots - 1);
        if (!g_seenProgramUsed[at]) {
            g_seenProgramUsed[at] = true;
            g_seenPrograms[at] = id;
            return true;
        }
        if (g_seenPrograms[at] == id) {
            return false;
        }
    }
    return false;
}

// The program the guest has selected. It is tracked from the guest's own
// glUseProgram call, so the probe does not have to ask the driver once per
// draw: a quarter of a million draws per run cannot afford another query.
GLuint g_currentProgram = 0;

// The programs that a GLYPH TEXTURE was drawn with. Those are the engine's
// 2D programs, and every draw they make is a candidate for "this is the HUD
// drawing something".
constexpr int kTextProgramSlots = 16;
GLuint g_textPrograms[kTextProgramSlots];
int g_textProgramCount = 0;

bool isTextProgram(GLuint program) {
    for (int index = 0; index < g_textProgramCount; ++index) {
        if (g_textPrograms[index] == program) {
            return true;
        }
    }
    return false;
}

void rememberTextProgram(GLuint program) {
    if (program == 0 || isTextProgram(program)) {
        return;
    }
    if (g_textProgramCount < kTextProgramSlots) {
        g_textPrograms[g_textProgramCount++] = program;
    }
}

// The (program, texture) pairs already reported. The HUD draws the same few
// textures every frame; the first draw of each pair is the one that says
// what the HUD is putting on the screen.
constexpr size_t kPairSlots = 4096;
uint64_t g_seenPairs[kPairSlots];
bool g_seenPairUsed[kPairSlots];

bool firstDrawOfPair(GLuint program, GLuint texture) {
    const uint64_t key = (static_cast<uint64_t>(program) << 32) | texture;
    const size_t start = static_cast<size_t>((key * 2654435761u) & (kPairSlots - 1));
    for (size_t probe = 0; probe < kPairSlots; ++probe) {
        const size_t at = (start + probe) & (kPairSlots - 1);
        if (!g_seenPairUsed[at]) {
            g_seenPairUsed[at] = true;
            g_seenPairs[at] = key;
            return true;
        }
        if (g_seenPairs[at] == key) {
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void appendText(char* buffer, size_t capacity, const char* text) {
    const size_t used = strlen(buffer);
    if (used + 1 >= capacity) {
        return;
    }
    snprintf(buffer + used, capacity - used, "%s", text);
}

}  // namespace

void gfxTextProbeNoteTexture(GLuint id, int width, int height) {
    if (id == 0) {
        return;
    }
    for (int index = 0; index < g_glyphTextureCount; ++index) {
        if (g_glyphTextures[index].id == id) {
            g_glyphTextures[index].width = width;
            g_glyphTextures[index].height = height;
            return;
        }
    }
    if (g_glyphTextureCount >= kGlyphTextureSlots) {
        return;
    }
    g_glyphTextures[g_glyphTextureCount].id = id;
    g_glyphTextures[g_glyphTextureCount].width = width;
    g_glyphTextures[g_glyphTextureCount].height = height;
    ++g_glyphTextureCount;
}

void gfxTextProbeUseProgram(GLuint program) {
    g_currentProgram = program;
}

void gfxTextProbeVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                     GLboolean normalized, GLsizei stride,
                                     const void* pointer) {
    AttribSetup& setup = g_setups[g_setupNext];
    setup.seq = ++g_setupSeq;
    setup.index = index;
    setup.size = size;
    setup.type = type;
    setup.normalized = normalized;
    setup.stride = stride;
    setup.pointer = reinterpret_cast<uintptr_t>(pointer);
    // A pointer into a buffer object is an OFFSET, not an address, so the
    // buffer that was bound at setup time is part of the record.
    setup.buffer = g_boundArrayBuffer;
    g_setupNext = (g_setupNext + 1) % kSetupSlots;
}

void gfxTextProbeBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) {
        g_boundArrayBuffer = buffer;
    }
}

void gfxTextProbeBufferData(GLenum target, GLsizeiptr size, const void* data) {
    if (target != GL_ARRAY_BUFFER) {
        return;
    }
    BufferShadow* shadow = shadowFor(g_boundArrayBuffer);
    if (shadow == nullptr) {
        return;
    }
    if (shadow->data == nullptr) {
        shadow->data = static_cast<uint8_t*>(malloc(kBufferCapacity));
        if (shadow->data == nullptr) {
            return;
        }
    }
    const size_t wanted = size > 0 ? static_cast<size_t>(size) : 0;
    shadow->size = wanted > kBufferCapacity ? kBufferCapacity : wanted;
    if (data != nullptr && shadow->size > 0) {
        memcpy(shadow->data, data, shadow->size);
        g_shadowBytes += shadow->size;
        ++g_shadowCalls;
    }
}

void gfxTextProbeBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                const void* data) {
    if (target != GL_ARRAY_BUFFER || data == nullptr || size <= 0) {
        return;
    }
    BufferShadow* shadow = shadowFor(g_boundArrayBuffer);
    if (shadow == nullptr || shadow->data == nullptr) {
        return;
    }
    const size_t from = static_cast<size_t>(offset);
    if (from >= kBufferCapacity) {
        return;
    }
    size_t copy = static_cast<size_t>(size);
    if (from + copy > kBufferCapacity) {
        copy = kBufferCapacity - from;
    }
    memcpy(shadow->data + from, data, copy);
    g_shadowBytes += copy;
    ++g_shadowCalls;
    const size_t end = from + copy;
    if (end > shadow->size) {
        shadow->size = end;
    }
}

// The vertices a draw touches, in the order the draw touches them. For an
// array draw they are first, first+1, ...; for an indexed draw they are the
// values in the index buffer.
void probeDrawImpl(GLenum mode, GLsizei count, const int* vertexIndices, bool indexed);

void gfxTextProbeDraw(GLenum mode, GLint first, GLsizei count) {
    g_pending = PendingReport{};
    g_pending.active = false;
    if (count <= 0 || count > kMaxVertices) {
        return;
    }
    int vertices[kMaxVertices];
    for (int index = 0; index < count; ++index) {
        vertices[index] = first + index;
    }
    probeDrawImpl(mode, count, vertices, false);
}

void gfxTextProbeDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void* indices) {
    g_pending = PendingReport{};
    g_pending.active = false;
    if (count <= 0 || count > kMaxVertices) {
        return;
    }
    // The index buffer is the guest's own memory (or an offset into one).
    const uintptr_t address = reinterpret_cast<uintptr_t>(indices);
    if (!readableAddress(address)) {
        return;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(address);
    int vertices[kMaxVertices];
    for (int index = 0; index < count; ++index) {
        switch (type) {
            case GL_UNSIGNED_BYTE:
                vertices[index] = bytes[index];
                break;
            case GL_UNSIGNED_SHORT: {
                uint16_t value = 0;
                memcpy(&value, bytes + index * 2, 2);
                vertices[index] = value;
                break;
            }
            case GL_UNSIGNED_INT: {
                uint32_t value = 0;
                memcpy(&value, bytes + index * 4, 4);
                vertices[index] = static_cast<int>(value);
                break;
            }
            default:
                vertices[index] = index;
                break;
        }
    }
    probeDrawImpl(mode, count, vertices, true);
}

void probeDrawImpl(GLenum mode, GLsizei count, const int* vertexIndices, bool indexed) {
    const int first = vertexIndices[0];
    ++g_drawOrdinal;

    GLint texture = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    const bool glyph = isGlyphTexture(static_cast<GLuint>(texture));
    const bool firstDraw = g_drawOrdinal <= static_cast<unsigned long long>(kFirstDrawBudget);
    const char* phaseName = "text-draw";
    if (!glyph && !firstDraw) {
        // Not a glyph texture: it is still worth reporting if it is a draw
        // with one of the HUD's own 2D programs, because that is where the
        // HUD's widgets -- and the telemetry quad, if it is drawn at all --
        // come from. A glyph-texture gate alone would miss a HUD widget that
        // draws with the engine's own atlas.
        if (!isTextProgram(g_currentProgram)) {
            return;
        }
        if (g_programDraws >= kProgramDrawBudget) {
            return;
        }
        if (!firstDrawOfPair(g_currentProgram, static_cast<GLuint>(texture))) {
            return;
        }
        ++g_programDraws;
        phaseName = "hud-draw";
    } else if (glyph) {
        if (g_textDraws >= kTextDrawBudget) {
            return;
        }
        // One report per glyph texture: the first draw of a texture is the
        // draw that put that text on the screen.
        if (!firstDrawOfTexture(static_cast<GLuint>(texture))) {
            return;
        }
        ++g_textDraws;
    } else {
        if (g_firstDraws >= kFirstDrawBudget) {
            return;
        }
        ++g_firstDraws;
        phaseName = "draw-first";
    }

    GLint program = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (glyph) {
        // A glyph texture was drawn with this program: this is one of the
        // engine's 2D programs, and every draw it makes is worth a look.
        rememberTextProgram(static_cast<GLuint>(program));
    }
    GLint unit = 0;
    ::glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    GLint viewport[4] = {0, 0, 0, 0};
    ::glGetIntegerv(GL_VIEWPORT, viewport);
    GLint scissor[4] = {0, 0, 0, 0};
    ::glGetIntegerv(GL_SCISSOR_BOX, scissor);
    GLint framebuffer = 0;
    ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    GLint arrayBuffer = 0;
    ::glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);

    const GLboolean blend = ::glIsEnabled(GL_BLEND);
    const GLboolean depthTest = ::glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull = ::glIsEnabled(GL_CULL_FACE);
    const GLboolean scissorTest = ::glIsEnabled(GL_SCISSOR_TEST);
    GLint srcRgb = 0;
    GLint dstRgb = 0;
    GLint srcAlpha = 0;
    GLint dstAlpha = 0;
    GLint equation = 0;
    ::glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
    ::glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
    ::glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
    ::glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
    ::glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    ::glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);

    // The MVP, by uniform name if the guest uses that name, and otherwise by
    // taking the program's only mat4.
    float mvp[16] = {0.f};
    char mvpText[64] = "none";
    GLint mvpLocation =
        ::glGetUniformLocation(static_cast<GLuint>(program), "u_mvp");
    if (mvpLocation < 0) {
        GLint uniformCount = 0;
        ::glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_UNIFORMS,
                         &uniformCount);
        for (GLint index = 0; index < uniformCount && index < 24; ++index) {
            char name[64] = {0};
            GLint size = 0;
            GLenum type = 0;
            GLsizei length = 0;
            ::glGetActiveUniform(static_cast<GLuint>(program),
                                 static_cast<GLuint>(index), sizeof(name) - 1,
                                 &length, &size, &type, name);
            if (type == GL_FLOAT_MAT4) {
                mvpLocation = ::glGetUniformLocation(static_cast<GLuint>(program), name);
                if (mvpLocation >= 0) {
                    snprintf(mvpText, sizeof(mvpText), "%s", name);
                    break;
                }
            }
        }
    } else {
        snprintf(mvpText, sizeof(mvpText), "u_mvp");
    }
    if (mvpLocation >= 0) {
        ::glGetUniformfv(static_cast<GLuint>(program), mvpLocation, mvp);
    }

    // The program's uniforms. The colour of the quad is NOT necessarily an
    // attribute: the engine's 2D shader paints u_CurrentColour, so a zero
    // alpha there hides a perfectly good quad just as effectively as a zero
    // alpha attribute would.
    GLint uniformCount = 0;
    ::glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_UNIFORMS, &uniformCount);
    char uniformText[1024];
    uniformText[0] = '\0';
    for (GLint index = 0; index < uniformCount && index < 16; ++index) {
        char name[64] = {0};
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        ::glGetActiveUniform(static_cast<GLuint>(program), static_cast<GLuint>(index),
                             sizeof(name) - 1, &length, &size, &type, name);
        const GLint location = ::glGetUniformLocation(static_cast<GLuint>(program), name);
        char value[128] = "n/a";
        if (location >= 0) {
            if (type == GL_FLOAT_VEC4 || type == GL_FLOAT_VEC3 ||
                type == GL_FLOAT_VEC2) {
                float v[4] = {0.f, 0.f, 0.f, 0.f};
                ::glGetUniformfv(static_cast<GLuint>(program), location, v);
                snprintf(value, sizeof(value), "[%.3f %.3f %.3f %.3f]", v[0], v[1],
                         v[2], v[3]);
            } else if (type == GL_FLOAT) {
                float v = 0.f;
                ::glGetUniformfv(static_cast<GLuint>(program), location, &v);
                snprintf(value, sizeof(value), "[%.3f]", v);
            } else if (type == GL_SAMPLER_2D || type == 0x8DCA /* GL_INT_SAMPLER_2D */) {
                int v = -1;
                ::glGetUniformiv(static_cast<GLuint>(program), location, &v);
                snprintf(value, sizeof(value), "[%d]", v);
            } else if (type == GL_FLOAT_MAT3) {
                // The texture matrix: it is what maps the raw texture
                // coordinates into the texture, so it is part of "where do
                // the UVs point".
                float v[9] = {0.f};
                ::glGetUniformfv(static_cast<GLuint>(program), location, v);
                snprintf(value, sizeof(value),
                         "[%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f]",
                         v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
            } else if (type == GL_FLOAT_MAT4) {
                float v[16] = {0.f};
                ::glGetUniformfv(static_cast<GLuint>(program), location, v);
                snprintf(value, sizeof(value),
                         "[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                         "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]",
                         v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
                         v[10], v[11], v[12], v[13], v[14], v[15]);
            }
        }
        char one[192];
        snprintf(one, sizeof(one), " %s(type=0x%x %s)", name, type, value);
        const size_t used = strlen(uniformText);
        if (used + strlen(one) + 1 < sizeof(uniformText)) {
            memcpy(uniformText + used, one, strlen(one) + 1);
        }
    }

    // The program's attributes. The names come from the driver, so the probe
    // does not have to guess what the guest's shader calls them.
    GLint attributeCount = 0;
    ::glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_ATTRIBUTES,
                     &attributeCount);
    int attrCount = 0;
    for (GLint index = 0;
         index < attributeCount && attrCount < kMaxAttribs; ++index) {
        char name[64] = {0};
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        ::glGetActiveAttrib(static_cast<GLuint>(program), static_cast<GLuint>(index),
                            sizeof(name) - 1, &length, &size, &type, name);
        const GLint location =
            ::glGetAttribLocation(static_cast<GLuint>(program), name);
        AttrReport& report = g_pending.attrs[attrCount];
        snprintf(report.name, sizeof(report.name), "%s", name);
        report.location = location;
        report.info = queryAttrib(static_cast<GLuint>(location));
        report.setup = matchSetup(static_cast<GLuint>(location), report.info);
        report.source = sourceFor(report.setup);
        // What the vertices are decoded with. The driver's own report is
        // authoritative; when it says nothing, the description the guest
        // handed to glVertexAttribPointer is the fallback.
        GLint decodeSize = report.info.size;
        GLenum decodeType = static_cast<GLenum>(report.info.type);
        GLint decodeStride = report.info.stride;
        if (decodeSize <= 0 && report.setup != nullptr) {
            decodeSize = report.setup->size;
            decodeType = report.setup->type;
            decodeStride = report.setup->stride;
        }
        report.componentCount = decodeSize > 4 ? 4 : (decodeSize > 0 ? decodeSize : 0);
        report.valueCount = 0;
        const int stride = decodeStride > 0 ? decodeStride : 0;
        const int vertexCount = count < kMaxVertices ? count : kMaxVertices;
        for (int vertex = 0; vertex < vertexCount; ++vertex) {
            float* values = report.values[vertex];
            for (int component = 0; component < 4; ++component) {
                values[component] = 0.f;
            }
            if (report.source.base == nullptr) {
                continue;
            }
            const size_t elementStride =
                stride > 0 ? static_cast<size_t>(stride) : 0;
            // The vertex this draw element refers to: first+vertex for an
            // array draw, the index value for an indexed draw.
            const size_t offset =
                static_cast<size_t>(vertexIndices[vertex]) * elementStride;
            for (int component = 0; component < report.componentCount; ++component) {
                values[component] = readComponent(
                    report.source.base, report.source.size, offset,
                    decodeType, report.info.normalized != 0, component);
            }
        }
        report.valueCount = vertexCount;
        ++attrCount;
    }
    g_pending.attrCount = attrCount;
    g_pending.vertexCount = count < kMaxVertices ? count : kMaxVertices;

    // Where the quad lands on the screen, from the MVP and the position
    // attribute. The position attribute is the one named a_pos / a_Position if
    // there is one, otherwise the first attribute with two or more components.
    const AttrReport* position = nullptr;
    for (int index = 0; index < attrCount; ++index) {
        const AttrReport& report = g_pending.attrs[index];
        if (strcmp(report.name, "a_pos") == 0 || strcmp(report.name, "a_Position") == 0) {
            position = &report;
            break;
        }
    }
    if (position == nullptr) {
        for (int index = 0; index < attrCount; ++index) {
            const AttrReport& report = g_pending.attrs[index];
            if (report.componentCount >= 2) {
                position = &report;
                break;
            }
        }
    }

    float minX = 0.f;
    float minY = 0.f;
    float maxX = 0.f;
    float maxY = 0.f;
    bool haveRect = false;
    if (position != nullptr && position->source.base != nullptr) {
        for (int vertex = 0; vertex < position->valueCount; ++vertex) {
            const float x = position->values[vertex][0];
            const float y = position->values[vertex][1];
            const float z = position->values[vertex][2];
            const float clipX = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
            const float clipY = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
            const float clipW = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
            const float ndcX = clipW != 0.f ? clipX / clipW : clipX;
            const float ndcY = clipW != 0.f ? clipY / clipW : clipY;
            g_pending.ndc[vertex][0] = ndcX;
            g_pending.ndc[vertex][1] = ndcY;
            if (!haveRect) {
                minX = maxX = ndcX;
                minY = maxY = ndcY;
                haveRect = true;
            } else {
                if (ndcX < minX) minX = ndcX;
                if (ndcX > maxX) maxX = ndcX;
                if (ndcY < minY) minY = ndcY;
                if (ndcY > maxY) maxY = ndcY;
            }
        }
    }

    g_pending.active = true;
    g_pending.ordinal = g_drawOrdinal;
    g_pending.texture = static_cast<GLuint>(texture);
    g_pending.program = static_cast<GLuint>(program);
    for (int index = 0; index < 4; ++index) {
        g_pending.viewport[index] = viewport[index];
    }
    if (haveRect) {
        const float left = viewport[0] + (minX * 0.5f + 0.5f) * viewport[2];
        const float right = viewport[0] + (maxX * 0.5f + 0.5f) * viewport[2];
        const float bottom = viewport[1] + (minY * 0.5f + 0.5f) * viewport[3];
        const float top = viewport[1] + (maxY * 0.5f + 0.5f) * viewport[3];
        g_pending.rect[0] = static_cast<GLint>(left);
        g_pending.rect[1] = static_cast<GLint>(bottom);
        g_pending.rect[2] = static_cast<GLint>(right - left);
        g_pending.rect[3] = static_cast<GLint>(top - bottom);
        g_pending.onScreen = maxX >= -1.f && minX <= 1.f && maxY >= -1.f && minY <= 1.f;
    } else {
        g_pending.rect[0] = 0;
        g_pending.rect[1] = 0;
        g_pending.rect[2] = 0;
        g_pending.rect[3] = 0;
        g_pending.onScreen = false;
    }

    // The pixels of the quad's own rectangle, BEFORE the draw. The same
    // rectangle is read again after the draw, and the difference is the
    // only measurement that cannot lie about "did this draw paint
    // anything": a quad that painted white text over a blue sky and a quad
    // that painted nothing look identical in a single readback.
    g_pending.beforeWhite = 0;
    g_pending.beforeUnique = 0;
    g_pending.beforeMaxR = 0;
    g_pending.readWidth = 0;
    g_pending.readHeight = 0;
    {
        uint8_t before[256 * 64 * 4];
        int width = 0;
        int height = 0;
        if (g_pending.onScreen && readQuadRect(g_pending.rect, g_pending.viewport, &width,
                                               &height)) {
            memset(before, 0, sizeof(before));
            ::glReadPixels(g_pending.rect[0], g_pending.rect[1], width, height, GL_RGBA,
                           GL_UNSIGNED_BYTE, before);
            g_pending.readWidth = width;
            g_pending.readHeight = height;
            const int count = width * height;
            for (int pixel = 0; pixel < count; ++pixel) {
                const int r = before[pixel * 4 + 0];
                const int g = before[pixel * 4 + 1];
                const int b = before[pixel * 4 + 2];
                if (r > 200 && g > 200 && b > 200) {
                    ++g_pending.beforeWhite;
                }
                if (r > g_pending.beforeMaxR) {
                    g_pending.beforeMaxR = r;
                }
            }
            g_pending.beforeUnique = countUniqueColours(before, count);
        }
    }

    // The state line.
    const GlyphTexture* glyphInfo = glyphTexture(static_cast<GLuint>(texture));
    int textureWidth = glyphInfo != nullptr ? glyphInfo->width : 0;
    int textureHeight = glyphInfo != nullptr ? glyphInfo->height : 0;
    if (glyphInfo == nullptr && texture != 0) {
        // GLES2 asks the size of a texture through the level query.
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kGlTextureWidth, &textureWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, kGlTextureHeight, &textureHeight);
    }
    char details[2400];
    details[0] = '\0';
    char part[256];
    snprintf(part, sizeof(part),
             "mode=0x%x first=%d count=%d indexed=%d tex=%d texWH=%dx%d glyph=%d "
             "unit=%d prog=%d trackedProg=%d attribs=%d vp=%d,%d,%d,%d "
             "scissor=%d,%d,%d,%d scissorTest=%d fbo=%d arrBuf=%d blend=%d srcRGB=%d "
             "dstRGB=%d srcA=%d dstA=%d eq=%d depth=%d cull=%d mask=%d,%d,%d,%d",
             mode, first, count, indexed ? 1 : 0, texture, textureWidth, textureHeight,
             glyphInfo != nullptr ? 1 : 0, unit - GL_TEXTURE0, program,
             static_cast<int>(g_currentProgram), attributeCount,
             viewport[0], viewport[1], viewport[2], viewport[3], scissor[0],
             scissor[1], scissor[2], scissor[3], scissorTest ? 1 : 0, framebuffer,
             arrayBuffer, blend ? 1 : 0, srcRgb, dstRgb, srcAlpha, dstAlpha, equation,
             depthTest ? 1 : 0, cull ? 1 : 0, colorMask[0] ? 1 : 0,
             colorMask[1] ? 1 : 0, colorMask[2] ? 1 : 0, colorMask[3] ? 1 : 0);
    appendText(details, sizeof(details), part);

    snprintf(part, sizeof(part), " mvpName=%s mvp=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
             "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]",
             mvpText, mvp[0], mvp[1], mvp[2], mvp[3], mvp[4], mvp[5], mvp[6], mvp[7],
             mvp[8], mvp[9], mvp[10], mvp[11], mvp[12], mvp[13], mvp[14], mvp[15]);
    appendText(details, sizeof(details), part);
    appendText(details, sizeof(details), " uniforms=");
    appendText(details, sizeof(details), uniformText);

    for (int index = 0; index < attrCount; ++index) {
        const AttrReport& report = g_pending.attrs[index];
        snprintf(part, sizeof(part),
                 " attr%d=%s(loc=%d size=%d type=0x%x stride=%d norm=%d buf=%d "
                 "mesaPtr32=0x%x mesaPtr64=0x%llx setupPtr=0x%llx setupBuf=%u "
                 "setupSeq=%llu setupAge=%llu src=%s)",
                 index, report.name, report.location, report.info.size,
                 report.info.type, report.info.stride, report.info.normalized,
                 report.info.bufferBinding, report.info.pointer32,
                 static_cast<unsigned long long>(report.info.pointer64),
                 report.setup != nullptr
                     ? static_cast<unsigned long long>(report.setup->pointer)
                     : 0ull,
                 report.setup != nullptr ? report.setup->buffer : 0u,
                 report.setup != nullptr ? report.setup->seq : 0ull,
                 report.setup != nullptr ? g_setupSeq - report.setup->seq : 0ull,
                 report.source.base != nullptr
                     ? (report.source.clientSide ? "client" : "vbo")
                     : "none");
        appendText(details, sizeof(details), part);
    }

    guestViewWideLog(phaseName, static_cast<unsigned long long>(texture),
                     static_cast<unsigned long long>(count), details);

    // The program's shader source, once per program. It is what says what the
    // program DOES with the quad: which attribute is the position, which one
    // is the texture coordinate, and what the fragment shader does with the
    // colour. A newline in the source would break the log's line format, so
    // the line feeds are turned into '|'.
    if (firstDrawOfProgram(static_cast<GLuint>(program))) {
        GLuint shaders[4] = {0, 0, 0, 0};
        GLsizei attached = 0;
        ::glGetAttachedShaders(static_cast<GLuint>(program), 4, &attached, shaders);
        for (GLsizei index = 0; index < attached && index < 4; ++index) {
            char source[1400];
            GLsizei length = 0;
            source[0] = '\0';
            ::glGetShaderSource(shaders[index], sizeof(source) - 1, &length, source);
            source[sizeof(source) - 1] = '\0';
            char flat[1400];
            size_t out = 0;
            for (size_t at = 0; source[at] != '\0' && out + 1 < sizeof(flat); ++at) {
                const char c = source[at];
                flat[out++] = (c == '\n' || c == '\r') ? '|' : c;
            }
            flat[out] = '\0';
            char programDetails[1500];
            snprintf(programDetails, sizeof(programDetails),
                     "shader=%u type=0x%x bytes=%d src=%s", shaders[index],
                     0u, static_cast<int>(length), flat);
            guestViewWideLog("text-prog", static_cast<unsigned long long>(program),
                             static_cast<unsigned long long>(shaders[index]),
                             programDetails);
        }
    }
}

void gfxTextProbeDrawFinished() {
    if (!g_pending.active) {
        return;
    }
    g_pending.active = false;

    char details[1600];
    details[0] = '\0';
    char part[256];

    // The vertices, attribute by attribute.
    for (int index = 0; index < g_pending.attrCount; ++index) {
        const AttrReport& report = g_pending.attrs[index];
        snprintf(part, sizeof(part), " %s:", report.name);
        appendText(details, sizeof(details), part);
        for (int vertex = 0; vertex < report.valueCount; ++vertex) {
            char numbers[96];
            numbers[0] = '\0';
            for (int component = 0; component < report.componentCount; ++component) {
                char one[24];
                snprintf(one, sizeof(one), "%s%.4f", component == 0 ? "" : ",",
                         report.values[vertex][component]);
                const size_t used = strlen(numbers);
                if (used + strlen(one) + 1 < sizeof(numbers)) {
                    memcpy(numbers + used, one, strlen(one) + 1);
                }
            }
            snprintf(part, sizeof(part), " v%d=(%s)", vertex, numbers);
            appendText(details, sizeof(details), part);
        }
    }

    // Where the quad lands.
    snprintf(part, sizeof(part), " ndc=");
    appendText(details, sizeof(details), part);
    for (int vertex = 0; vertex < g_pending.vertexCount; ++vertex) {
        snprintf(part, sizeof(part), " v%d=(%.4f,%.4f)", vertex,
                 g_pending.ndc[vertex][0], g_pending.ndc[vertex][1]);
        appendText(details, sizeof(details), part);
    }
    snprintf(part, sizeof(part), " rect=%d,%d,%d,%d onScreen=%d",
             g_pending.rect[0], g_pending.rect[1], g_pending.rect[2],
             g_pending.rect[3], g_pending.onScreen ? 1 : 0);
    appendText(details, sizeof(details), part);

    // The pixels, AFTER the draw. The same rectangle was read before the
    // draw, so the difference between the two reads is what the draw did.
    uint8_t pixels[256 * 64 * 4];
    memset(pixels, 0, sizeof(pixels));
    const int readWidth = g_pending.readWidth;
    const int readHeight = g_pending.readHeight;
    bool read = readWidth > 0 && readHeight > 0;
    if (read) {
        ::glReadPixels(g_pending.rect[0], g_pending.rect[1], readWidth, readHeight,
                       GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    if (read) {
        const int count = readWidth * readHeight;
        int white = 0;
        int maxR = 0;
        for (int pixel = 0; pixel < count; ++pixel) {
            const int r = pixels[pixel * 4 + 0];
            const int g = pixels[pixel * 4 + 1];
            const int b = pixels[pixel * 4 + 2];
            if (r > 200 && g > 200 && b > 200) {
                ++white;
            }
            if (r > maxR) {
                maxR = r;
            }
        }
        snprintf(part, sizeof(part),
                 " readback=%dx%d at=%d,%d whiteBefore=%d whiteAfter=%d "
                 "maxRBefore=%d maxRAfter=%d uniqAfter=%d first=%d,%d,%d,%d",
                 readWidth, readHeight, g_pending.rect[0], g_pending.rect[1],
                 g_pending.beforeWhite, white, g_pending.beforeMaxR, maxR,
                 countUniqueColours(pixels, count), pixels[0], pixels[1], pixels[2],
                 pixels[3]);
        appendText(details, sizeof(details), part);
    } else {
        appendText(details, sizeof(details), " readback=none");
    }

    guestViewWideLog("text-quad", static_cast<unsigned long long>(g_pending.texture),
                     static_cast<unsigned long long>(g_pending.ordinal), details);
}

}  // namespace rr3
