#include "rr3/graphics_probe.hpp"

#include "rr3/guest_filesystem.hpp"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <switch.h>
#include <zlib.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <dirent.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace rr3 {

GraphicsProbe runWindowGraphicsProbe(NWindow* window) {
    GraphicsProbe probe{};
    probe.attempted = true;
    probe.surfaceMode = 1;
    if (window == nullptr) {
        probe.eglError = EGL_BAD_NATIVE_WINDOW;
        return probe;
    }

    probe.stage = 1;
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        probe.eglError = eglGetError();
        return probe;
    }
    probe.displayCreated = true;
    probe.stage = 2;
    if (eglInitialize(display, &probe.major, &probe.minor) != EGL_TRUE) {
        probe.eglError = eglGetError();
        eglTerminate(display);
        return probe;
    }
    probe.initialized = true;

    probe.stage = 3;
    constexpr EGLint configAttributes[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config{};
    EGLint configCount = 0;
    if (eglChooseConfig(display, configAttributes, &config, 1, &configCount) != EGL_TRUE ||
        configCount == 0) {
        probe.eglError = eglGetError();
        eglTerminate(display);
        return probe;
    }
    probe.configChosen = true;
    probe.stage = 4;

    EGLSurface surface = eglCreateWindowSurface(display, config, window, nullptr);
    if (surface == EGL_NO_SURFACE) {
        probe.eglError = eglGetError();
        eglTerminate(display);
        return probe;
    }
    probe.surfaceCreated = true;
    probe.stage = 5;

    constexpr EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(
        display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        probe.eglError = eglGetError();
        eglDestroySurface(display, surface);
        eglTerminate(display);
        return probe;
    }
    probe.contextCreated = true;
    probe.stage = 6;

    if (eglMakeCurrent(display, surface, surface, context) == EGL_TRUE) {
        probe.madeCurrent = true;
        const GLubyte* version = glGetString(GL_VERSION);
        std::snprintf(probe.version, sizeof(probe.version), "%s",
                      version != nullptr ? reinterpret_cast<const char*>(version) : "unknown");
        glViewport(0, 0, 1280, 720);
        glClearColor(0.03f, 0.08f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        probe.glError = glGetError();
        const bool swapped = eglSwapBuffers(display, surface) == EGL_TRUE;
        if (!swapped) {
            probe.eglError = eglGetError();
        }
        probe.drawCompleted = probe.glError == GL_NO_ERROR && swapped;
        probe.stage = probe.drawCompleted ? 7 : 6;

        // Minimal ES2 shader-compile probe: proves the Switch GLES driver
        // accepts GLSL source compilation before RR3's real shaders are tried.
        // Uses no RR3 assets; a self-contained vertex+fragment pair.
        {
            static const char* kVertexSource =
                "attribute vec4 aPos;\n"
                "void main() { gl_Position = aPos; }\n";
            static const char* kFragmentSource =
                "precision mediump float;\n"
                "void main() { gl_FragColor = vec4(1.0, 0.5, 0.0, 1.0); }\n";

            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &kVertexSource, nullptr);
            glCompileShader(vs);
            GLint vsStatus = GL_FALSE;
            glGetShaderiv(vs, GL_COMPILE_STATUS, &vsStatus);
            probe.vertexShaderCompiled = vsStatus == GL_TRUE;
            if (!probe.vertexShaderCompiled) {
                glGetShaderInfoLog(vs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
            }

            GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fs, 1, &kFragmentSource, nullptr);
            glCompileShader(fs);
            GLint fsStatus = GL_FALSE;
            glGetShaderiv(fs, GL_COMPILE_STATUS, &fsStatus);
            probe.fragmentShaderCompiled = fsStatus == GL_TRUE;
            if (!probe.fragmentShaderCompiled && probe.shaderLog[0] == '\0') {
                glGetShaderInfoLog(fs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
            }

            if (probe.vertexShaderCompiled && probe.fragmentShaderCompiled) {
                GLuint program = glCreateProgram();
                glAttachShader(program, vs);
                glAttachShader(program, fs);
                glLinkProgram(program);
                GLint linkStatus = GL_FALSE;
                glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
                probe.programLinked = linkStatus == GL_TRUE;
                if (!probe.programLinked && probe.shaderLog[0] == '\0') {
                    glGetProgramInfoLog(program, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
                }
                glDeleteProgram(program);
            }

            glDeleteShader(vs);
            glDeleteShader(fs);
            if (probe.stage == 7 && probe.programLinked) {
                probe.stage = 8;
            }
        }

        // Realistic-usage probe: a textured, transformed ES2 pipeline closer to
        // what RR3 actually issues (mat4 uniform, varyings, sampler2D/texture2D),
        // plus a real texture upload and a textured draw. Still uses no RR3
        // assets — a 2x2 RGBA texture generated in-place. Gated on the toy
        // program linking so we don't chase secondary failures.
        if (probe.programLinked) {
            static const char* kRealVertex =
                "attribute vec4 aPos;\n"
                "attribute vec2 aUV;\n"
                "uniform mat4 uMVP;\n"
                "varying vec2 vUV;\n"
                "void main() { vUV = aUV; gl_Position = uMVP * aPos; }\n";
            static const char* kRealFragment =
                "precision mediump float;\n"
                "varying vec2 vUV;\n"
                "uniform sampler2D uTex;\n"
                "uniform vec4 uTint;\n"
                "void main() { gl_FragColor = texture2D(uTex, vUV) * uTint; }\n";

            GLuint rvs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(rvs, 1, &kRealVertex, nullptr);
            glCompileShader(rvs);
            GLint rvsStatus = GL_FALSE;
            glGetShaderiv(rvs, GL_COMPILE_STATUS, &rvsStatus);

            GLuint rfs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(rfs, 1, &kRealFragment, nullptr);
            glCompileShader(rfs);
            GLint rfsStatus = GL_FALSE;
            glGetShaderiv(rfs, GL_COMPILE_STATUS, &rfsStatus);

            probe.realisticShaderCompiled = rvsStatus == GL_TRUE && rfsStatus == GL_TRUE;
            if (!probe.realisticShaderCompiled && probe.shaderLog[0] == '\0') {
                if (rvsStatus != GL_TRUE) {
                    glGetShaderInfoLog(rvs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
                } else {
                    glGetShaderInfoLog(rfs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
                }
            }

            GLuint realProgram = 0;
            if (probe.realisticShaderCompiled) {
                realProgram = glCreateProgram();
                glAttachShader(realProgram, rvs);
                glAttachShader(realProgram, rfs);
                glBindAttribLocation(realProgram, 0, "aPos");
                glBindAttribLocation(realProgram, 1, "aUV");
                glLinkProgram(realProgram);
                GLint rLink = GL_FALSE;
                glGetProgramiv(realProgram, GL_LINK_STATUS, &rLink);
                probe.realisticProgramLinked = rLink == GL_TRUE;
                if (!probe.realisticProgramLinked && probe.shaderLog[0] == '\0') {
                    glGetProgramInfoLog(realProgram, sizeof(probe.shaderLog), nullptr, probe.shaderLog);
                }
            }

            GLuint tex = 0;
            if (probe.realisticProgramLinked) {
                const unsigned char pixels[] = {
                    255, 0,   0,   255,   0,   255, 0,   255,
                    0,   0,   255, 255,   255, 255, 0,   255,
                };
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pixels);
                probe.textureUploaded = glGetError() == GL_NO_ERROR;
            }

            if (probe.textureUploaded) {
                static const float kIdentity[16] = {
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f,
                };
                static const float kVerts[] = {
                    -0.5f, -0.5f, 0.0f, 0.0f,
                     0.5f, -0.5f, 1.0f, 0.0f,
                    -0.5f,  0.5f, 0.0f, 1.0f,
                     0.5f,  0.5f, 1.0f, 1.0f,
                };
                glUseProgram(realProgram);
                GLint mvpLoc = glGetUniformLocation(realProgram, "uMVP");
                GLint texLoc = glGetUniformLocation(realProgram, "uTex");
                GLint tintLoc = glGetUniformLocation(realProgram, "uTint");
                glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, kIdentity);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, tex);
                glUniform1i(texLoc, 0);
                glUniform4f(tintLoc, 1.0f, 1.0f, 1.0f, 1.0f);
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                      4 * sizeof(float), kVerts);
                glEnableVertexAttribArray(1);
                glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                      4 * sizeof(float), kVerts + 2);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                probe.texturedDrawCompleted = glGetError() == GL_NO_ERROR;
                glDisableVertexAttribArray(0);
                glDisableVertexAttribArray(1);
                glUseProgram(0);
            }

            if (tex != 0) {
                glDeleteTextures(1, &tex);
            }
            if (probe.stage == 8 && probe.texturedDrawCompleted) {
                probe.stage = 9;
            }

            // ---- Stage 8: real RR3 asset load probe (reuses realProgram; still
            // current). Walk the gui-sd volume for the first *.dds.z sprite,
            // strip the [u32 LE inflated-size][zlib stream] container header,
            // inflate, parse the DDS, upload the DXT block data with
            // glCompressedTexImage2D, then draw one frame with realProgram.
            // The 960x640 volume root holds only subdirectories, so this
            // descends one level to find a directory that directly holds a
            // sprite. Gated on stage 9 and a live realProgram to avoid chasing
            // secondary failures.
            if (probe.stage == 9 && realProgram != 0) {
                char assetPath[0x300] = {0};
                const char* kAssetRoot = ".depot/gui_assets/960x640";

                // Preferred known-good sprites, tried first for a stable probe.
                static const char* kPreferred[] = {
                    ".depot/gui_assets/960x640/upgrades/sprites_0.dxt.dds.z",
                    ".depot/gui_assets/960x640/car_sales/sprites_0.dxt.dds.z",
                    ".depot/gui_assets/960x640/whats_new/sprites_0.dxt.dds.z",
                };
                for (const char* candidate : kPreferred) {
                    if (std::FILE* probeFile = rr3::guestFopen(candidate, "rb")) {
                        std::fclose(probeFile);
                        std::snprintf(assetPath, sizeof(assetPath), "%s", candidate);
                        break;
                    }
                }

                // Fallback: descend into the first subdirectory that directly
                // holds a *.dds.z file.
                if (assetPath[0] == '\0') {
                    if (DIR* root = rr3::guestOpendir(kAssetRoot)) {
                        while (struct dirent* top = readdir(root)) {
                            if (top->d_name[0] == '.') {
                                continue;
                            }
                            char childDir[0x280];
                            std::snprintf(childDir, sizeof(childDir), "%s/%s",
                                          kAssetRoot, top->d_name);
                            if (DIR* sub = rr3::guestOpendir(childDir)) {
                                while (struct dirent* ent = readdir(sub)) {
                                    const char* n = ent->d_name;
                                    size_t nlen = std::strlen(n);
                                    if (nlen > 6 &&
                                        std::strcmp(n + nlen - 6, ".dds.z") == 0) {
                                        std::snprintf(assetPath, sizeof(assetPath),
                                                      "%s/%s", childDir, n);
                                        break;
                                    }
                                }
                                closedir(sub);
                            }
                            if (assetPath[0] != '\0') {
                                break;
                            }
                        }
                        closedir(root);
                    }
                }

                std::FILE* af =
                    assetPath[0] != '\0' ? rr3::guestFopen(assetPath, "rb") : nullptr;
                if (af != nullptr) {
                    probe.assetOpened = true;
                    std::fseek(af, 0, SEEK_END);
                    long asize = std::ftell(af);
                    std::fseek(af, 0, SEEK_SET);
                    std::vector<unsigned char> rawa(
                        asize > 0 ? static_cast<size_t>(asize) : 0);
                    size_t got =
                        rawa.empty() ? 0 : std::fread(rawa.data(), 1, rawa.size(), af);
                    std::fclose(af);

                    std::vector<unsigned char> dds;
                    if (got == rawa.size() && rawa.size() > 6 && rawa[4] == 0x78) {
                        uint32_t declared = 0;
                        std::memcpy(&declared, rawa.data(), 4);
                        dds.resize(declared);
                        uLongf destLen = declared;
                        if (uncompress(dds.data(), &destLen, rawa.data() + 4,
                                       static_cast<uLong>(rawa.size() - 4)) == Z_OK &&
                            destLen == declared) {
                            probe.assetInflated = true;
                        }
                    }

                    GLenum glFormat = 0;
                    GLsizei blockBytes = 0;
                    if (probe.assetInflated && dds.size() >= 128 &&
                        std::memcmp(dds.data(), "DDS ", 4) == 0) {
                        std::memcpy(&probe.assetHeight, dds.data() + 12, 4);
                        std::memcpy(&probe.assetWidth, dds.data() + 16, 4);
                        std::memcpy(probe.assetFourcc, dds.data() + 84, 4);
                        if (std::memcmp(probe.assetFourcc, "DXT1", 4) == 0) {
                            glFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
                            blockBytes = 8;
                        } else if (std::memcmp(probe.assetFourcc, "DXT5", 4) == 0) {
                            glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                            blockBytes = 16;
                        }
                        probe.assetDdsParsed = glFormat != 0;
                    }

                    const char* exts =
                        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
                    probe.s3tcSupported =
                        exts != nullptr &&
                        std::strstr(exts, "texture_compression_s3tc") != nullptr;

                    GLuint assetTex = 0;
                    if (probe.assetDdsParsed && probe.s3tcSupported) {
                        GLsizei imgSize =
                            ((static_cast<GLsizei>(probe.assetWidth) + 3) / 4) *
                            ((static_cast<GLsizei>(probe.assetHeight) + 3) / 4) *
                            blockBytes;
                        if (128 + static_cast<size_t>(imgSize) <= dds.size()) {
                            glGenTextures(1, &assetTex);
                            glBindTexture(GL_TEXTURE_2D, assetTex);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                            glCompressedTexImage2D(
                                GL_TEXTURE_2D, 0, glFormat,
                                static_cast<GLsizei>(probe.assetWidth),
                                static_cast<GLsizei>(probe.assetHeight), 0,
                                imgSize, dds.data() + 128);
                            probe.realAssetUploaded = glGetError() == GL_NO_ERROR;
                        }
                    }

                    if (probe.realAssetUploaded) {
                        static const float kIdentity2[16] = {
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        };
                        static const float kAssetVerts[] = {
                            -0.5f, -0.5f, 0.0f, 0.0f,  0.5f, -0.5f, 1.0f, 0.0f,
                            -0.5f,  0.5f, 0.0f, 1.0f,  0.5f,  0.5f, 1.0f, 1.0f,
                        };
                        glUseProgram(realProgram);
                        glUniformMatrix4fv(
                            glGetUniformLocation(realProgram, "uMVP"), 1, GL_FALSE,
                            kIdentity2);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, assetTex);
                        glUniform1i(glGetUniformLocation(realProgram, "uTex"), 0);
                        glUniform4f(glGetUniformLocation(realProgram, "uTint"),
                                    1.0f, 1.0f, 1.0f, 1.0f);
                        glEnableVertexAttribArray(0);
                        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                              4 * sizeof(float), kAssetVerts);
                        glEnableVertexAttribArray(1);
                        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                              4 * sizeof(float), kAssetVerts + 2);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        probe.realAssetDrawCompleted = glGetError() == GL_NO_ERROR;
                        glDisableVertexAttribArray(0);
                        glDisableVertexAttribArray(1);
                        glUseProgram(0);
                    }

                    if (assetTex != 0) {
                        glDeleteTextures(1, &assetTex);
                    }
                    if (probe.realAssetDrawCompleted) {
                        probe.stage = 10;
                    }
                }
            }
            // ---- End stage 8 ----

            if (realProgram != 0) {
                glDeleteProgram(realProgram);
            }
            glDeleteShader(rvs);
            glDeleteShader(rfs);
        }

        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    else {
        probe.eglError = eglGetError();
    }

    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    return probe;
}

} // namespace rr3
