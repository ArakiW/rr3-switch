#pragma once

#include <GLES2/gl2.h>

// ---------------------------------------------------------------------------
// The text-draw probe.
//
// WHAT THIS IS FOR
// ----------------
// The guest draws a label as ONE textured quad. The port can see the texture
// it uploaded, but it could not see what the guest DID with it: which texture
// was bound when the label was drawn, what UV rectangle the quad carried, what
// the vertex transform did to its corners, what colour it carried -- and
// whether the draw left any pixels on the screen at all.
//
// That is what this probe answers, from the GL traffic itself.
//
// HOW IT WORKS
// ------------
// The port already wraps the guest's GL entry points (source/gfx/gfx_gles.cpp),
// so the probe is fed from four cheap places:
//
//   * gfxTextProbeVertexAttribPointer
//       remembers the CLIENT-SIDE vertex array the guest just described. The
//       pointer argument IS the address of the guest's own memory, so this is
//       the only place the quad's vertices can be read from: the guest does
//       not use a vertex buffer object for them.
//
//   * gfxTextProbeBindBuffer / BufferData / BufferSubData
//       keep a CPU shadow of the vertex buffers, for the draws that DO use a
//       buffer object. GLES2 has no glGetBufferSubData, so the only way to
//       read back what the guest is about to draw is to remember what it
//       uploaded.
//
//   * gfxTextProbeDraw
//       runs at the draw. It reads the bound texture, the current vertex
//       attribute setup, the blend/depth/scissor state and the MVP, decodes
//       the vertices, and works out where the quad lands on the screen.
//
//   * gfxTextProbeDrawFinished
//       runs AFTER the draw and reads the quad's own screen rectangle back
//       out of the framebuffer. A quad that drew nothing and a quad that drew
//       white pixels look identical from the outside; only the pixels say
//       which one it was.
//
// The probe only does the expensive work when the bound texture is one the
// glyph facade created (gfxTextProbeNoteTexture), so the cost on the other
// draws is a single glGetIntegerv.
namespace rr3 {

// The glyph facade calls this for every texture it creates, so the probe can
// tell the label texture apart from the game's own textures.
void gfxTextProbeNoteTexture(GLuint id, int width, int height);

// The CLIENT-SIDE vertex array the guest is describing. The pointer is the
// address of the guest's own memory: that is where the quad's vertices live.
void gfxTextProbeVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                     GLboolean normalized, GLsizei stride,
                                     const void* pointer);

// The program the guest has selected. The probe follows it so it can report
// EVERY draw the HUD's own 2D program makes, not only the ones that happen to
// have a glyph texture bound: a HUD widget that draws with the engine's own
// atlas is exactly the draw a glyph-texture gate would miss.
void gfxTextProbeUseProgram(GLuint program);

// The buffer shadow.
void gfxTextProbeBindBuffer(GLenum target, GLuint buffer);
void gfxTextProbeBufferData(GLenum target, GLsizeiptr size, const void* data);
void gfxTextProbeBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                const void* data);

// The draw hook, called immediately BEFORE the draw.
void gfxTextProbeDraw(GLenum mode, GLint first, GLsizei count);

// The same hook for an INDEXED draw. The guest issues millions of indexed
// draws, so an instrument that only watches glDrawArrays cannot claim that a
// quad was never drawn: it can only claim that no ARRAY draw carried it.
void gfxTextProbeDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void* indices);

// The draw hook, called immediately AFTER the draw. The pixel readback of the
// quad's screen rectangle happens here, because a readback taken before the
// draw would only show the background.
void gfxTextProbeDrawFinished();

}  // namespace rr3
