// The text-draw probe -- see include/rr3/gfx_text_probe.hpp.
//
// WHY IT EXISTS
// -------------
// The port can prove that it uploaded a glyph texture; it could not prove that
// the guest SAMPLES that texture where the ink is. The guest's label quad is
// four vertices in a buffer the guest owns, and the port had no way to see
// the UV rectangle those vertices carried.
//
// This file closes that gap by reading the GL traffic:
//
//   * a CPU shadow of the vertex buffers (GLES2 cannot read a buffer back),
//   * the vertex attribute setup at the moment of the draw,
//   * the bound texture, the program, and the MVP uniform.
//
// The measurement is what says WHY the label is invisible. Guessing does not.

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

// ---------------------------------------------------------------------------
// The glyph textures
// ---------------------------------------------------------------------------

constexpr int kGlyphTextureSlots = 64;

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

// ---------------------------------------------------------------------------
// The buffer shadow
// ---------------------------------------------------------------------------
//
// GLES2 has no glGetBufferSubData, so the only way to see what the guest is
// about to draw is to remember what it uploaded. Four slots is enough: the
// guest keeps one vertex buffer and one index buffer live at a time, and the
// shadow is a copy of the bytes, not of the GPU's storage.

constexpr int kBufferSlots = 4;
constexpr size_t kBufferCapacity = 512 * 1024;

// How much the shadow has copied. If this number is huge the shadow is the
// reason the guest is slow, and it has to be made cheaper.
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

// Reads one attribute component out of the shadow.
float readComponent(const uint8_t* base, size_t baseSize, GLintptr offset, GLenum type,
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
    const size_t at = static_cast<size_t>(offset) +
                      static_cast<size_t>(component) * elementSize;
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

struct AttribInfo {
    GLint index;
    GLint size;
    GLint type;
    GLint stride;
    GLint normalized;
    GLint bufferBinding;
    GLintptr pointer;
    bool present;
};

AttribInfo queryAttrib(GLuint program, GLint index) {
    AttribInfo info{};
    info.index = index;
    info.present = false;
    if (index < 0) {
        return info;
    }
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
    GLint pointer = 0;
    ::glGetVertexAttribiv(static_cast<GLuint>(index), GL_VERTEX_ATTRIB_ARRAY_POINTER,
                           &pointer);
    info.pointer = pointer;
    info.present = info.size > 0;
    (void)program;
    return info;
}

void formatAttrib(const char* label, const AttribInfo& info, const uint8_t* base,
                  size_t baseSize, int vertex, char* out, size_t capacity) {
    if (!info.present) {
        __builtin_snprintf(out, capacity, "%s=none", label);
        return;
    }
    const GLintptr offset = info.pointer + static_cast<GLintptr>(vertex) * info.stride;
    float values[4] = {0.f, 0.f, 0.f, 0.f};
    for (GLint component = 0; component < info.size && component < 4; ++component) {
        values[component] = readComponent(base, baseSize, offset,
                                          static_cast<GLenum>(info.type),
                                          info.normalized != 0, component);
    }
    __builtin_snprintf(out, capacity, "%s(size=%d type=0x%x stride=%d norm=%d buf=%d off=%ld)",
                       label, info.size, info.type, info.stride, info.normalized,
                       info.bufferBinding, (long)info.pointer);
    char numbers[96];
    numbers[0] = '\0';
    for (GLint component = 0; component < info.size && component < 4; ++component) {
        char part[32];
        __builtin_snprintf(part, sizeof(part), "%s%.4f", component == 0 ? "" : ",",
                           values[component]);
        const size_t used = strlen(numbers);
        if (used + strlen(part) + 1 < sizeof(numbers)) {
            memcpy(numbers + used, part, strlen(part) + 1);
        }
    }
    char combined[512];
    __builtin_snprintf(combined, sizeof(combined), "%s vals=[%s]", out, numbers);
    __builtin_snprintf(out, capacity, "%s", combined);
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

void gfxTextProbeDraw(GLenum mode, GLint first, GLsizei count) {
    static int s_logged = 0;
    if (count <= 0 || count > 8) {
        return;
    }
    GLint texture = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    if (!isGlyphTexture(static_cast<GLuint>(texture))) {
        return;
    }
    // The cap counts the LABEL draws only. Counting every draw would use the
    // whole budget in the first few frames and the label would never be
    // reported.
    if (s_logged >= 400) {
        return;
    }
    ++s_logged;

    GLint program = 0;
    ::glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    GLint unit = 0;
    ::glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    GLint viewport[4] = {0, 0, 0, 0};
    ::glGetIntegerv(GL_VIEWPORT, viewport);

    // Enumerate the program's attributes instead of guessing their names: the
    // port does not know which names the guest's shader uses.
    GLint attributeCount = 0;
    ::glGetProgramiv(static_cast<GLuint>(program), GL_ACTIVE_ATTRIBUTES,
                     &attributeCount);
    char attributeText[768];
    attributeText[0] = '\0';
    for (GLint index = 0; index < attributeCount && index < 8; ++index) {
        char name[64] = {0};
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        ::glGetActiveAttrib(static_cast<GLuint>(program), static_cast<GLuint>(index),
                            sizeof(name) - 1, &length, &size, &type, name);
        const GLint location =
            ::glGetAttribLocation(static_cast<GLuint>(program), name);
        const AttribInfo info = queryAttrib(static_cast<GLuint>(program), location);
        const BufferShadow* shadow = nullptr;
        if (info.bufferBinding != 0) {
            for (int slot = 0; slot < kBufferSlots; ++slot) {
                if (g_buffers[slot].id == static_cast<GLuint>(info.bufferBinding) &&
                    g_buffers[slot].data != nullptr) {
                    shadow = &g_buffers[slot];
                    break;
                }
            }
        }
        const uint8_t* base = shadow != nullptr ? shadow->data : nullptr;
        const size_t baseSize = shadow != nullptr ? shadow->size : 0;
        char one[512];
        formatAttrib(name, info, base, baseSize, first, one, sizeof(one));
        const size_t used = strlen(attributeText);
        if (used + strlen(one) + 2 < sizeof(attributeText)) {
            memcpy(attributeText + used, one, strlen(one) + 1);
            if (used > 0) {
                attributeText[used] = ' ';
                attributeText[used + 1] = '\0';
            }
        }
    }

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
                    __builtin_snprintf(mvpText, sizeof(mvpText), "%s", name);
                    break;
                }
            }
        }
    } else {
        __builtin_snprintf(mvpText, sizeof(mvpText), "u_mvp");
    }
    if (mvpLocation >= 0) {
        ::glGetUniformfv(static_cast<GLuint>(program), mvpLocation, mvp);
    }

    char details[1400];
    __builtin_snprintf(
        details, sizeof(details),
        "mode=0x%x first=%d count=%d tex=%d unit=%d prog=%d attribs=%d vp=%d,%d,%d,%d "
        "mvpName=%s mvp=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
        "%.3f %.3f %.3f %.3f] shadow=%lluB/%llu calls %s",
        mode, first, count, texture, unit - GL_TEXTURE0, program, attributeCount,
        viewport[0], viewport[1], viewport[2], viewport[3], mvpText, mvp[0], mvp[1],
        mvp[2], mvp[3], mvp[4], mvp[5], mvp[6], mvp[7], mvp[8], mvp[9], mvp[10],
        mvp[11], mvp[12], mvp[13], mvp[14], mvp[15], g_shadowBytes, g_shadowCalls,
        attributeText);
    guestViewWideLog("text-draw", static_cast<unsigned long long>(texture),
                     static_cast<unsigned long long>(count), details);
}

}  // namespace rr3
