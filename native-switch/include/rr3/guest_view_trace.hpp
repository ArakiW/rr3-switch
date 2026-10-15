#pragma once

#include <GLES2/gl2.h>
#include <cstddef>
#include <cstdint>

namespace rr3 {

// Installs a narrowly-scoped trace for guest-resolved glGetString calls. The
// wrapper is transparent unless enabled by the Guest View probe.
void enableGuestViewGlTrace(const char* progressPath);
void disableGuestViewGlTrace();

// Host-symbol target used by the guest resolver for glGetString.
const GLubyte* guestViewTracedGlGetString(GLenum name);

// Host-symbol target used by the guest resolver for malloc. It is transparent
// unless the Guest View callback trace is enabled, then records enter/return
// pairs for selected post-GL allocation sizes: 0x2800, 0x3e38, 0x478, the
// first 0x240 vector-growth call, 0x130, and the first two 0x40
// vector-storage growth calls.
void* guestViewTracedMalloc(size_t size);

// Host-symbol target used by the guest resolver for memcpy. It is transparent
// until the trace has reached stage 30, then records only the first
// post-stage-30 memcpy enter/return pair.
//
// AS OF 2026-10-06 this symbol is an ASSEMBLY STUB, not a C function. See
// source/guest_frame_probe.s for why, and rr3/guest_frame_probe.hpp for what it
// is for. The short version: x29 is callee-saved, so the guest's own frame
// pointer is live in x29 at every call site inside the container-teardown
// function, and a C prologue destroys it before any C statement can read it.
// The stub captures the incoming x29 and the incoming x30 and hands them to the
// C body below, which is the same logic this function always had plus the two
// extra parameters.
//
// The symbol is extern "C" so the stub can define it from an assembly file
// without having to spell out a C++ mangled name.
extern "C" void* guestViewTracedMemcpy(void* destination, const void* source,
                                       size_t length);

// The C body the assembly stub calls. Not a resolver target: nothing binds the
// guest's "memcpy" import to this, the stub is.
//
// guestFp is the caller's ARM64 frame pointer and guestLr is the caller's
// return address, both as they were on entry to the stub. For a guest call
// through the PLT, guestLr is a guest ELF address and guestFp is that guest
// function's own frame pointer.
//
// Also extern "C", for the same reason as the stub: the stub reaches it with a
// plain `bl`, and a C++ mangled name would have to be spelled out in the
// assembly and would then break silently the first time a parameter changed.
extern "C" void* guestViewTracedMemcpyBody(void* destination, const void* source,
                                           size_t length, uintptr_t guestFp,
                                           uintptr_t guestLr);

// Host-symbol target used by the guest resolver for free. It is transparent
// until the traced post-stage-30 memcpy has returned, then records only the
// first subsequent free enter/return pair.
void guestViewTracedFree(void* allocation);

// Host-symbol target used by the guest resolver for memset. It is transparent
// unless the Guest View callback trace is enabled, then records the distinctive
// initialization clears used by the first post-GL helper.
void* guestViewTracedMemset(void* destination, int value, size_t length);

// Last traced operation in the current probe. Zero means that no traced
// operation has completed or started.
unsigned int guestViewTraceLastStage();
bool guestViewTraceReached(unsigned int stage);

// Host-symbol target used by the stack-protector shim. When the Guest View
// trace is enabled, it records the first stack-canary failure without
// suppressing the subsequent abort.
GLuint guestViewTracedGlCreateShader(GLenum type);
void guestViewTracedGlShaderSource(GLuint shader, GLsizei count,
                                   const GLchar* const* strings, const GLint* lengths);
void guestViewTracedGlCompileShader(GLuint shader);
void guestViewTracedGlLinkProgram(GLuint program);
void guestViewTracedGlShaderBinary(GLsizei count, const GLuint* shaders,
                                   GLenum binaryformat, const void* binary,
                                   GLsizei length);

void guestViewTraceStackCheckFailure(uintptr_t caller);

// Records that a named guest synchronization primitive is still blocked during
// the Render interval. Safe to call from any thread.
void guestViewTraceSyncBlocked(const char* primitive, unsigned int stage);

// Frame-loop draw traffic. Counters advance only while the view trace is
// enabled so host/renderer probes do not pollute guest attribution.
struct GuestViewGlStats {
    uint32_t clears{};
    uint32_t drawArrays{};
    uint32_t drawElements{};
    uint32_t texImage2D{};
    uint32_t compressedTexImage2D{};
    uint32_t bindFramebuffer{};
    uint32_t genFramebuffers{};
    uint32_t framebufferTexture2D{};
    uint32_t useProgram{};
    uint32_t viewports{};
    uint32_t scissors{};
    uint32_t fboChecks{};
    uint32_t fboIncomplete{};
    uint32_t bindBuffer{};
    uint32_t bindTexture{};
    uint32_t bindTextureZero{};
    uint32_t bindBufferZero{};
    uint32_t genTextures{};
    uint32_t genBuffers{};
    uint32_t lastGenTexture{};
    uint32_t lastGenBuffer{};
    uint32_t hostTexInject{};
    uint32_t vertexAttribPointer{};
    uint32_t lastBoundTexture{};
    uint32_t lastBoundBuffer{};
    uint32_t lastDrawElementsCount{};
    uint32_t lastDrawMode{};
    uint32_t lastViewport[4]{};
    // Set by guestViewTracedGlScissor. Added 2026-10-11 for the black-screen
    // question: the run reports sc=1, i.e. glScissor is called exactly ONCE in
    // the whole session, while lastViewport is the full 1280x720. If the game
    // enables GL_SCISSOR_TEST and never resizes the box, every later fragment
    // is discarded and the buffer stays exactly as glClear left it -- which is
    // what the full-screen readback shows (lit=0 of 921600, EARLY and LATE).
    // Until the rectangle's actual values were logged, "one scissor call" was
    // a count and not a measurement.
    uint32_t lastScissor[4]{};
    uint32_t scissorEnabled{};
    uint32_t lastBoundFramebuffer{};
    uint32_t lastFboStatus{};
};
GuestViewGlStats guestViewGlStats();
void resetGuestViewGlStats();

void guestViewTracedGlClear(GLbitfield mask);
void guestViewTracedGlDrawArrays(GLenum mode, GLint first, GLsizei count);
void guestViewTracedGlDrawElements(GLenum mode, GLsizei count, GLenum type,
                                   const void* indices);
void guestViewTracedGlTexImage2D(GLenum target, GLint level, GLint internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLenum format, GLenum type, const void* pixels);
void guestViewTracedGlCompressedTexImage2D(GLenum target, GLint level,
                                           GLenum internalformat, GLsizei width,
                                           GLsizei height, GLint border,
                                           GLsizei imageSize, const void* data);
void guestViewTracedGlBindFramebuffer(GLenum target, GLuint framebuffer);
void guestViewTracedGlGenFramebuffers(GLsizei n, GLuint* framebuffers);
void guestViewTracedGlFramebufferTexture2D(GLenum target, GLenum attachment,
                                           GLenum textarget, GLuint texture,
                                           GLint level);
void guestViewTracedGlUseProgram(GLuint program);
void guestViewTracedGlViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void guestViewTracedGlScissor(GLint x, GLint y, GLsizei width, GLsizei height);
GLenum guestViewTracedGlCheckFramebufferStatus(GLenum target);
void guestViewTracedGlBindBuffer(GLenum target, GLuint buffer);
void guestViewTracedGlBindTexture(GLenum target, GLuint texture);
void guestViewTracedGlGenTextures(GLsizei n, GLuint* textures);
void guestViewTracedGlGenBuffers(GLsizei n, GLuint* buffers);
void guestViewTracedGlVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                          GLboolean normalized, GLsizei stride,
                                          const void* pointer);

// Persist guest android_log lines that mention view/splash/orientation so a
// device crash still reveals whether VIEW CREATED/CHANGED actually ran.
void noteContentRootGate(const char* site, uintptr_t renderTarget);
void guestViewNoteAndroidLog(const char* text);

// Wide-net first-N event log for the next device run (one build, many signals).
void guestViewWideLog(const char* phase, unsigned long long a,
                      unsigned long long b, const char* details = nullptr);

// After a guest Render that issued no indexed draws, paint a host fullscreen
// magenta/black quad so the device screen proves the GL pipeline.
void hostDrawMagentaQuad();

} // namespace rr3
