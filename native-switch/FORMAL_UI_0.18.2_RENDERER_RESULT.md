# Formal UI 0.18.2 renderer result

The read-only pbuffer renderer returned safely but failed before drawing. The
loader and every non-graphics self-test remained OK.

The switch-mesa reference path uses an EGL window surface on
`nwindowGetDefault()`. The dashboard software console also owns that NWindow,
so 0.18.3 explicitly releases the console framebuffer before EGL, renders and
swaps one frame on the default window, destroys EGL, then recreates the console
dashboard. The UI now displays graphics stage, mode, EGL error and GL error.
