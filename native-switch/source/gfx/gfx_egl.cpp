// Null EGL: 28 linker --wrap shims that satisfy the guest's EGL surface
// without touching a device.
//
// Only three EGL symbols are statically imported by the guest (eglGetDisplay,
// eglGetProcAddress, eglQueryString); the other 25 are reached either by the
// host probe (guest_view_probe.cpp, graphics_probe.cpp) or through
// eglGetProcAddress. Wrapping all of them is what lets one runtime switch
// cover both the guest's calls and the probe's own calls -- the probe is the
// process that has to get past eglInitialize for the acceptance log to exist
// at all.
//
// Mesa path: every wrapper forwards to __real_* with the arguments untouched.

#include "rr3/gfx_backend.hpp"

#include <EGL/egl.h>
#include <cstring>

namespace rr3 {
namespace {

// Fake handles. Real objects, not magic integers, so a stray dereference reads
// zeroed memory instead of jumping into the middle of the NRO.
struct FakeObject {
    unsigned long long magic;
    unsigned long long payload[3];
};
constexpr unsigned long long kDisplayMagic = 0x45474C4449535031ULL; // "EGLDISP1"
constexpr unsigned long long kSurfaceMagic = 0x45474C5355464631ULL; // "EGLSUF1"
constexpr unsigned long long kContextMagic = 0x45474C4354585031ULL; // "EGLCTX1"
constexpr unsigned long long kConfigMagic = 0x45474C43464E5031ULL;  // "EGLCFNP1"

// The magic is a debugging aid, not a contract: nothing checks it, and the
// payload stays zeroed so a stray dereference reads zeros.
FakeObject s_display = {kDisplayMagic, {0, 0, 0}};
FakeObject s_surface = {kSurfaceMagic, {0, 0, 0}};
FakeObject s_pbuffer = {kSurfaceMagic, {0, 0, 0}};
FakeObject s_context = {kContextMagic, {0, 0, 0}};
FakeObject s_config = {kConfigMagic, {0, 0, 0}};

EGLDisplay s_currentDisplay = EGL_NO_DISPLAY;
EGLContext s_currentContext = EGL_NO_CONTEXT;
EGLSurface s_currentSurface = EGL_NO_SURFACE;
EGLint s_error = EGL_SUCCESS;

// The GL extensions the engine probes for are advertised here as well as from
// glGetString: engines do read EGL_EXTENSIONS looking for a renderer hint, and
// an unknown name in either list is simply ignored.
constexpr const char* kEglExtensions =
    "EGL_KHR_fbo_halffloat_simd "
    "EGL_EXT_platform_base "
    "EGL_OES_vertex_array_object "
    "GL_OES_vertex_array_object "
    "GL_EXT_texture_format_s3tc "
    "GL_EXT_texture_compression_s3tc "
    "GL_OES_compressed_ETC1_RGB8_texture "
    "GL_EXT_shader_framebuffer_fetch";

constexpr const char* kVendor = "RR3 null graphics backend";
constexpr const char* kVersion = "1.4 null";
constexpr const char* kClientApis = "OpenGL_ES OpenGL_ES2";

} // namespace
} // namespace rr3

// ---- Mesa passthrough declarations -----------------------------------------
// __real_X is the archive symbol; --wrap turns references to X into __wrap_X
// and references to __real_X back into X.
#define RR3_REAL_EGL_DECL(rettype, name, params) \
    extern "C" rettype __real_##name params;

RR3_REAL_EGL_DECL(EGLBoolean, eglBindAPI, (EGLenum api))
RR3_REAL_EGL_DECL(EGLBoolean, eglChooseConfig, (EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs, EGLint config_size, EGLint *num_config))
RR3_REAL_EGL_DECL(EGLContext, eglCreateContext, (EGLDisplay dpy, EGLConfig config, EGLContext share_context, const EGLint *attrib_list))
RR3_REAL_EGL_DECL(EGLSurface, eglCreatePbufferSurface, (EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list))
RR3_REAL_EGL_DECL(EGLSurface, eglCreateWindowSurface, (EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list))
RR3_REAL_EGL_DECL(EGLBoolean, eglDestroyContext, (EGLDisplay dpy, EGLContext ctx))
RR3_REAL_EGL_DECL(EGLBoolean, eglDestroySurface, (EGLDisplay surface))
RR3_REAL_EGL_DECL(EGLBoolean, eglGetConfigAttrib, (EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value))
RR3_REAL_EGL_DECL(EGLBoolean, eglGetConfigs, (EGLDisplay dpy, EGLConfig *configs, EGLint config_size, EGLint *num_config))
RR3_REAL_EGL_DECL(EGLContext, eglGetCurrentContext, (void))
RR3_REAL_EGL_DECL(EGLDisplay, eglGetCurrentDisplay, (void))
RR3_REAL_EGL_DECL(EGLSurface, eglGetCurrentSurface, (EGLint readdraw))
RR3_REAL_EGL_DECL(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType display_id))
RR3_REAL_EGL_DECL(EGLint, eglGetError, (void))
RR3_REAL_EGL_DECL(__eglMustCastToProperFunctionPointerType, eglGetProcAddress, (const char *procname))
RR3_REAL_EGL_DECL(const char *, eglQueryString, (EGLDisplay dpy, EGLint name))
RR3_REAL_EGL_DECL(EGLBoolean, eglInitialize, (EGLDisplay dpy, EGLint *major, EGLint *minor))
RR3_REAL_EGL_DECL(EGLBoolean, eglMakeCurrent, (EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx))
RR3_REAL_EGL_DECL(EGLBoolean, eglQueryAPI, (EGLenum api))
RR3_REAL_EGL_DECL(EGLBoolean, eglQueryContext, (EGLDisplay dpy, EGLContext ctx, EGLint attribute, EGLint *value))
RR3_REAL_EGL_DECL(EGLBoolean, eglQuerySurface, (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint *value))
RR3_REAL_EGL_DECL(EGLBoolean, eglReleaseThread, (void))
RR3_REAL_EGL_DECL(EGLBoolean, eglSurfaceAttrib, (EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value))
RR3_REAL_EGL_DECL(EGLBoolean, eglSwapBuffers, (EGLDisplay dpy, EGLSurface surface))
RR3_REAL_EGL_DECL(EGLBoolean, eglSwapInterval, (EGLDisplay dpy, EGLint interval))
RR3_REAL_EGL_DECL(EGLBoolean, eglTerminate, (EGLDisplay dpy))
RR3_REAL_EGL_DECL(EGLBoolean, eglWaitClient, (void))
RR3_REAL_EGL_DECL(EGLBoolean, eglWaitGL, (void))
RR3_REAL_EGL_DECL(EGLBoolean, eglWaitNative, (EGLint engine))

namespace rr3 {

// ---- null EGL --------------------------------------------------------------

extern "C" EGLDisplay __wrap_eglGetDisplay(EGLNativeDisplayType display_id) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return &s_display;
    }
    return __real_eglGetDisplay(display_id);
}

extern "C" EGLBoolean __wrap_eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (major != nullptr) *major = 1;
        if (minor != nullptr) *minor = 4;
        return EGL_TRUE;
    }
    return __real_eglInitialize(dpy, major, minor);
}

extern "C" EGLBoolean __wrap_eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                            EGLConfig *configs, EGLint config_size,
                                            EGLint *num_config) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (configs != nullptr && config_size > 0) {
            configs[0] = &s_config;
        }
        if (num_config != nullptr) {
            *num_config = configs != nullptr && config_size > 0 ? 1 : 0;
        }
        return EGL_TRUE;
    }
    return __real_eglChooseConfig(dpy, attrib_list, configs, config_size, num_config);
}

extern "C" EGLBoolean __wrap_eglGetConfigs(EGLDisplay dpy, EGLConfig *configs,
                                           EGLint config_size, EGLint *num_config) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (configs != nullptr && config_size > 0) {
            configs[0] = &s_config;
        }
        if (num_config != nullptr) {
            *num_config = configs != nullptr && config_size > 0 ? 1 : 0;
        }
        return EGL_TRUE;
    }
    return __real_eglGetConfigs(dpy, configs, config_size, num_config);
}

extern "C" EGLBoolean __wrap_eglGetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                                EGLint attribute, EGLint *value) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (value != nullptr) {
            switch (attribute) {
            case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE:
            case EGL_ALPHA_SIZE: case EGL_STENCIL_SIZE:
                *value = 8;
                break;
            case EGL_DEPTH_SIZE:
                *value = 24;
                break;
            case EGL_SAMPLE_BUFFERS: case EGL_SAMPLES:
                *value = 0;
                break;
            case EGL_CONFIG_ID:
                *value = 1;
                break;
            case EGL_SURFACE_TYPE:
                *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
                break;
            case EGL_RENDERABLE_TYPE:
                *value = EGL_OPENGL_ES2_BIT;
                break;
            case EGL_CONFORMANT:
                *value = EGL_OPENGL_ES2_BIT;
                break;
            default:
                *value = 0;
                break;
            }
        }
        return EGL_TRUE;
    }
    return __real_eglGetConfigAttrib(dpy, config, attribute, value);
}

extern "C" EGLSurface __wrap_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                                    EGLNativeWindowType win,
                                                    const EGLint *attrib_list) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return &s_surface;
    }
    return __real_eglCreateWindowSurface(dpy, config, win, attrib_list);
}

extern "C" EGLSurface __wrap_eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                                     const EGLint *attrib_list) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return &s_pbuffer;
    }
    return __real_eglCreatePbufferSurface(dpy, config, attrib_list);
}

extern "C" EGLContext __wrap_eglCreateContext(EGLDisplay dpy, EGLConfig config,
                                              EGLContext share_context,
                                              const EGLint *attrib_list) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return &s_context;
    }
    return __real_eglCreateContext(dpy, config, share_context, attrib_list);
}

extern "C" EGLBoolean __wrap_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                            EGLSurface read, EGLContext ctx) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        s_currentDisplay = dpy;
        s_currentContext = ctx;
        s_currentSurface = draw;
        return EGL_TRUE;
    }
    return __real_eglMakeCurrent(dpy, draw, read, ctx);
}

extern "C" EGLBoolean __wrap_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (gfxNullActive()) {
        gfxTickFrame();
        return EGL_TRUE;
    }
    // Tick on the Mesa path too. It used to be null-only, which made frames,
    // calls, draws and the whole gfx-stats line STRUCTURALLY UNAVAILABLE the
    // moment backend=mesa became viable -- so "no counters" on a real GPU
    // backend was indistinguishable from "the guest stopped drawing". That is
    // exactly the ambiguity that sent earlier rounds hunting for a draw-count
    // bug that was never there. Counting the frames we actually swap is the
    // one observation the whole next round depends on.
    const EGLBoolean rc = __real_eglSwapBuffers(dpy, surface);
    gfxTickFrame();
    return rc;
}

extern "C" EGLint __wrap_eglGetError(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return s_error;
    }
    return __real_eglGetError();
}

extern "C" const char* __wrap_eglQueryString(EGLDisplay dpy, EGLint name) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        switch (name) {
        case EGL_VENDOR:
            return kVendor;
        case EGL_VERSION:
            return kVersion;
        case EGL_EXTENSIONS:
            return kEglExtensions;
        case EGL_CLIENT_APIS:
            return kClientApis;
        default:
            return kClientApis;
        }
    }
    return __real_eglQueryString(dpy, name);
}

extern "C" __eglMustCastToProperFunctionPointerType
__wrap_eglGetProcAddress(const char *procname) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(
            gfxProcAddress(procname));
    }
    // Mesa path: hand back OUR wrapper whenever we have one, so the guest's
    // resolved entry points land on __wrap_gl* and the counters, shader dumps
    // and draw accounting keep working. The wrappers forward to __real_gl* on
    // this backend, so behaviour is unchanged -- only the accounting comes back.
    //
    // This used to return __real_eglGetProcAddress unconditionally, which handed
    // the guest Mesa's own dispatch for every name it resolves. That left every
    // counter structurally blind on a working GPU backend.
    if (void* ours = gfxProcAddressIfWrapped(procname)) {
        return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(ours);
    }
    return __real_eglGetProcAddress(procname);
}

extern "C" EGLBoolean __wrap_eglBindAPI(EGLenum api) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglBindAPI(api);
}

extern "C" EGLBoolean __wrap_eglQueryAPI(EGLenum api) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE;
    }
    return __real_eglQueryAPI(api);
}

extern "C" EGLBoolean __wrap_eglTerminate(EGLDisplay dpy) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        s_currentDisplay = EGL_NO_DISPLAY;
        s_currentContext = EGL_NO_CONTEXT;
        s_currentSurface = EGL_NO_SURFACE;
        return EGL_TRUE;
    }
    return __real_eglTerminate(dpy);
}

extern "C" EGLBoolean __wrap_eglReleaseThread(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglReleaseThread();
}

extern "C" EGLBoolean __wrap_eglDestroyContext(EGLDisplay dpy, EGLContext ctx) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglDestroyContext(dpy, ctx);
}

extern "C" EGLBoolean __wrap_eglDestroySurface(EGLDisplay surface) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglDestroySurface(surface);
}

extern "C" EGLContext __wrap_eglGetCurrentContext(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return s_currentContext;
    }
    return __real_eglGetCurrentContext();
}

extern "C" EGLDisplay __wrap_eglGetCurrentDisplay(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return s_currentDisplay;
    }
    return __real_eglGetCurrentDisplay();
}

extern "C" EGLSurface __wrap_eglGetCurrentSurface(EGLint readdraw) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return s_currentSurface;
    }
    return __real_eglGetCurrentSurface(readdraw);
}

extern "C" EGLBoolean __wrap_eglQuerySurface(EGLDisplay dpy, EGLSurface surface,
                                            EGLint attribute, EGLint *value) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (value != nullptr) {
            // The surface the guest renders into is the VIEW size the port
            // told it (onViewChangedJNI).  rr3-screen.txt's "view=WxH" moves
            // it; absent that key this is the 1280x720 the port has always
            // answered.  See rr3/gfx_backend.hpp.
            const int viewWidth =
                gfxViewWidthOverride() > 0 ? gfxViewWidthOverride() : 1280;
            const int viewHeight =
                gfxViewHeightOverride() > 0 ? gfxViewHeightOverride() : 720;
            *value = attribute == EGL_WIDTH ? viewWidth : viewHeight;
        }
        return EGL_TRUE;
    }
    return __real_eglQuerySurface(dpy, surface, attribute, value);
}

extern "C" EGLBoolean __wrap_eglQueryContext(EGLDisplay dpy, EGLContext ctx,
                                            EGLint attribute, EGLint *value) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        if (value != nullptr) {
            *value = 0;
        }
        return EGL_TRUE;
    }
    return __real_eglQueryContext(dpy, ctx, attribute, value);
}

extern "C" EGLBoolean __wrap_eglSurfaceAttrib(EGLDisplay dpy, EGLSurface surface,
                                              EGLint attribute, EGLint value) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglSurfaceAttrib(dpy, surface, attribute, value);
}

extern "C" EGLBoolean __wrap_eglSwapInterval(EGLDisplay dpy, EGLint interval) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglSwapInterval(dpy, interval);
}

extern "C" EGLBoolean __wrap_eglWaitGL(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglWaitGL();
}

extern "C" EGLBoolean __wrap_eglWaitNative(EGLint engine) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglWaitNative(engine);
}

extern "C" EGLBoolean __wrap_eglWaitClient(void) {
    if (gfxNullActive()) {
        gfxNoteVoidCall();
        return EGL_TRUE;
    }
    return __real_eglWaitClient();
}

} // namespace rr3
