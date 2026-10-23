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
// the vertex transform did to its corners, and what colour it carried.
//
// That is what this probe answers, from the GL traffic itself.
//
// HOW IT WORKS
// ------------
// The port already wraps the guest's GL entry points (source/gfx/gfx_gles.cpp),
// so the probe is fed from three cheap places:
//
//   * gfxTextProbeBindBuffer / BufferData / BufferSubData
//       keep a CPU shadow of the vertex buffers. GLES2 has no
//       glGetBufferSubData, so the only way to read back what the guest is
//       about to draw is to remember what it uploaded.
//
//   * gfxTextProbeDraw
//       runs at the draw. It reads the bound texture and the current vertex
//       attribute setup, and decodes the first few vertices out of the shadow.
//
// The probe only does the expensive work when the bound texture is one the
// glyph facade created (gfxTextProbeNoteTexture), so the cost on the 46,630
// other draws is a single glGetIntegerv.
namespace rr3 {

// The glyph facade calls this for every texture it creates, so the probe can
// tell the label texture apart from the game's own textures.
void gfxTextProbeNoteTexture(GLuint id, int width, int height);

// The buffer shadow.
void gfxTextProbeBindBuffer(GLenum target, GLuint buffer);
void gfxTextProbeBufferData(GLenum target, GLsizeiptr size, const void* data);
void gfxTextProbeBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                const void* data);

// The draw hook.
void gfxTextProbeDraw(GLenum mode, GLint first, GLsizei count);

}  // namespace rr3
