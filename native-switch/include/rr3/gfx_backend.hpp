#ifndef RR3_GFX_BACKEND_HPP
#define RR3_GFX_BACKEND_HPP

// Runtime-selectable graphics backend for the rehosted guest.
//
// The NRO ships with devkitPro's Mesa (nouveau/DRM) as the only real EGL/GLES2
// implementation, because that is the path real Switch hardware needs. On an
// emulator with no DRM device that path is a dead end: eglInitialize spins.
//
// This layer keeps both backends in the binary and picks one per process:
//
//   RR3_GFX unset (or anything but "null")  -> Mesa, byte-for-byte the old path
//   RR3_GFX=null                            -> the null/recording backend
//
// The selection is made once, on the first graphics call, and never changes.
// The conf file is only consulted when the environment variable is absent, so
// the default really is "Mesa" and the emulator path is strictly opt-in.

#include <cstddef>
#include <cstdint>

namespace rr3 {

enum class GfxBackend {
    kMesa,
    kNull,
};

struct GfxCounters {
    uint64_t frames = 0;             // eglSwapBuffers calls (null backend)
    uint64_t calls = 0;              // every null-backend entry point
    uint64_t drawArrays = 0;
    uint64_t drawElements = 0;
    uint64_t shaderSource = 0;       // glShaderSource calls
    uint64_t glslBytes = 0;          // total GLSL bytes handed to glShaderSource
    uint64_t shaderBinary = 0;       // glShaderBinary calls
    uint64_t binBytes = 0;           // total binary shader bytes
    uint32_t binFormat = 0;          // binaryFormat of the first blob
    uint64_t compileShader = 0;
    uint64_t linkProgram = 0;
    uint64_t programs = 0;           // glCreateProgram calls
    uint64_t shaders = 0;            // glCreateShader calls
    uint64_t textures = 0;           // glGenTextures
    uint64_t bufferBytes = 0;        // glBufferData + glBufferSubData bytes
    uint64_t texImage2D = 0;         // glTexImage2D calls
    uint64_t texImagePixels = 0;     // summed w*h of glTexImage2D
    uint32_t texFmt[4] = {0, 0, 0, 0};
    uint64_t texFmtCount[4] = {0, 0, 0, 0};
    uint64_t compressedTex = 0;      // glCompressedTexImage2D calls
    uint64_t compressedTexBytes = 0; // summed imageSize
    uint64_t compressedTexPixels = 0; // summed w*h
    uint32_t compFmt[6] = {0, 0, 0, 0, 0, 0};
    uint64_t compFmtCount[6] = {0, 0, 0, 0, 0, 0};
    uint64_t readPixels = 0;
    uint64_t guestShaderSource = 0;  // subset of shaderSource from guest code
    uint64_t glslDumps = 0;          // GLSL files actually written
    uint64_t binDumps = 0;           // binary blobs actually written
    char firstBinMagic[80] = {0};    // hex of the first 32 blob bytes
    char lastDump[64] = {0};         // most recent dump filename
};

// Resolved once per process. Cheap after the first call.
GfxBackend gfxBackend();
bool gfxNullActive();
const char* gfxBackendName();   // "mesa" / "null"
const char* gfxBackendSource(); // "default" / "env" / "file"

// "nofb=1" in rr3-gfx.conf: the console must not create a framebuffer on the
// default NWindow. Experiment knob for the Mesa eglInitialize wedge.
bool gfxSkipFramebufferUi();

// "nogo=1": never initialise the socket stack, so outbound connections fail at
// once instead of hanging. Honest failure, not a forged success -- see the
// implementation for the measurement that motivated it.
bool gfxOfflineNetwork();

// "menugate=1": report the host Cloudcell stub's byte 0x4C1 as 1, which is the
// main-menu loader gate at guest 0x12aefd8. Off by default -- the same byte also
// gates async requests at 0xab2c30, where 0 is what prevents a wedge.
bool gfxMenuGateOpen();

// "ccdone=1": invoke the Cloudcell continuation captured by the endpoint stub.
// That is the only path that clears the guest's `pending` flag. Experiment only
// -- the record it decodes carries an assert whose accepted shape is unproven.
bool gfxCloudcellCompleteRequests();

// "ccdisp=0|1|2": how far the JNI call slots in jni_bridge.cpp go toward the
// ManagedMethod table in jni_compat/classes/.
//   0 (default) -- no dispatch; the call slots log and return, which is what
//                  they have always done.
//   1           -- resolve class and method, log the match, DO NOT call.
//   2           -- call it.
//
// The middle rung exists because the managed stubs blr back into guest code and
// a 2026-10-04 run recorded a callback that entered and never drained. Level 1
// answers "would this fire, and does the name even match" for a whole run with
// zero execution.
int gfxCloudcellDispatch();

// "objid=1": jniNewObjectV hands out one real handle per call, each carrying its
// class and enough instance space for the facade to write into, instead of the
// single shared &g_fakeObjectObject.
//
// Two things follow from the shared sentinel, and both are live defects:
// jniGetObjectClass cannot name an object's class (so instance methods arrive
// with no class and only reach a facade through a name-only guess), and any
// facade that writes instance fields writes into one byte of globals --
// aam_constructor alone stores 16.
//
// This changes every object address the guest sees, and this file records the
// guest spinning on repeated identical returns, so it is measured rather than
// assumed. jniCallVoidMethodV refuses to run at ccdisp=2 while this is off.
bool gfxObjectIdentity();

// "mkdiridem=1": guest mkdir returns success when the directory already exists.
//
// The guest logs "[FileManager::CreateDir] Error creating directory:
// .../.depot/CC_Data" because the host staged that directory out of the game
// data packs. On real hardware the game creates it itself on first run and
// mkdir succeeds, so the shape mismatch is the host's.
//
// Idempotent, not silent: EEXIST is counted, and a mkdir failing for any other
// reason still returns its error.
bool gfxMkdirIdempotent();

// "ccloginok=1": the Cloudcell account manager answers LoginCompleteCallback
// with success instead of an honest failure.
//
// The honest failure stops the "Invalid DeviceId!" complaint -- the callback
// is reached and understood -- but the guest then spins on the sync queue
// ("Sub Sync Queued: 10765") and the run dies. It retries because the login
// failed, not because the callback was missing.
//
// The user authorised a simulated Cloudcell backend, and EA's service is gone,
// so a local success is the only answer the guest accepts. Off by default.
bool gfxCloudcellLoginOk();

// "ccmode=off|now|bg": WHICH context invokes that continuation, as opposed to
// whether it is invoked at all (ccdone). The only delivery site that existed is
// the frame loop, and the guest blocks inside frame 0 before reaching it, so:
//   off -- unchanged behaviour, the frame-loop site is still the only one
//   now -- invoke it inline inside the endpoint stub, right after the capture
//   bg  -- cloudcell_defer() it and let the existing alert-pump thread drain it
// Never returns anything but "off", "now" or "bg".
// "ccsynth=0|1": install the host's synthetic Cloudcell object (1) or let the
// guest build its own (0, the default). See gfxCloudcellSynthetic().
bool gfxCloudcellSynthetic();

// Which GL identity the guest is told it is running on. The guest classifies
// GL_RENDERER into a numeric id and the render loop waits for id 4, which is
// "renderer string contains PowerVR". See gfx_gles.cpp.
enum class GpuIdProfile { kPowerVR, kAdreno, kNative };
GpuIdProfile gfxGpuIdProfile();

// "glctxearly=1": bring a GL context up before the guest's early init, so the
// guest's one-shot glGetString(GL_VERSION) sample returns a real number. Default
// 1. See ensureEarlyGuestGlContext() for why this is not optional.
bool gfxEarlyGlContext();

const char* gfxCloudcellCompleteMode();

// "mlstub=1": force menu-loader vtable slot 21 to the host stub even when the
// guest's own vtable is valid. Experiment knob.
bool gfxForceMenuLoaderStub();

// Accounting. gfxNoteVoidCall() is on the hot path of every null-backend
// no-op, so it must stay a single increment.
void gfxNoteVoidCall();
void gfxTickFrame();            // called from the null eglSwapBuffers
uint64_t gfxFrameCount();
const GfxCounters& gfxCounters();
void gfxLogCounters(const char* reason); // force one summary line

// Object-name allocation. Always returns non-zero and never repeats, because
// the engine uses 0 as "no object" and compares names for identity.
uint32_t gfxNextObjectId();
uint32_t gfxNextLocation();

// Per-call accounting for the hand-written entry points. These stay separate
// instead of exposing the counter struct so the invariants live in one file.
void gfxNoteProgramCreated();
void gfxNoteTexturesCreated(uint64_t count);
void gfxNoteDrawArrays();
void gfxNoteDrawElements();
void gfxNoteCompile();
void gfxNoteLink();
void gfxNoteBufferBytes(uint64_t bytes);
void gfxNoteTexImage(uint64_t width, uint64_t height, uint32_t internalFormat);
void gfxNoteCompressedTex(uint32_t internalFormat, uint64_t width, uint64_t height,
                          uint64_t bytes);
void gfxNoteReadPixels();

// Shader capture. gfxRecordShaderSource glues the count/string array into one
// NUL-terminated buffer and writes it to sdmc:/switch/ under a stable name.
void gfxNoteShaderCreated(uint32_t shader, uint32_t type);
void gfxRecordShaderSource(uint32_t shader, const char* joined, size_t length,
                           uintptr_t caller);
void gfxRecordShaderBinary(uint32_t count, uint32_t format, const void* binary,
                           size_t length, uintptr_t caller);
const char* gfxShaderSource(uint32_t shader, size_t* length);

// Null-backend eglGetProcAddress. Never returns 0: the engine dereferences the
// 321 names it resolves without a NULL check (GL_SURFACE_REPORT.md 3.3).
void* gfxProcAddress(const char* name);

// nullptr unless this port itself wraps `name`. Use this to decide whether to
// hand a guest our wrapper (accounting included) or the driver's own pointer.
void* gfxProcAddressIfWrapped(const char* name);
// Records one of the string-only names the engine actually asked for, so the
// log answers "which of the 321 does it really use" instead of guessing.
void gfxNoteUnknownProc(const char* name);

} // namespace rr3

#endif // RR3_GFX_BACKEND_HPP
