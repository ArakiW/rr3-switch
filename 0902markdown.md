

\_整理时间:2026-09-02\_



| 阶段 | 里程碑 | 状态 | 说明 |

|----|--------|------|------|

| 1 | 4 核心 DSO 加载 + 15.8 万重定位 + 权限/RELRO | ✅ 真机 | 0.21.0 |

| 2 | 7 卷 romfs 外部数据挂载 + 路径 VFS | ✅ 真机 | 7/7 已挂载 |

| 3 | 依赖构造器 init(libc++→Nimble→FMOD) | ✅ 真机 | TLS 崩溃已根治 |

| 4 | 181 个 RR3 主构造器 | ✅ 真机 | 全部跑完 |

| 5 | 首帧独立渲染(EGL/GLES 8 阶段 + glClear + swap) | ✅ 真机 | “独立渲染阶段已通过” |

| 6 | 合成图形管线(纹理 shader + 上传 + 带纹理绘制) | ✅ 真机 | 截图底部“全部成功” |

| 7 | UI 字体完整显示(无框框) | ✅ 真机 | 已确认无框 |

| 8 | 从 romfs 读\*\*真实游戏资源\*\*并上传/绘制 | 🟡 代码就绪待编译 | \*\*当前阶段\*\* |

| 9 | 进主循环 / 游戏自身场景第一帧 | ⬜ | |

| 10 | 输入(手柄)+ 音频(FMOD 出声) | ⬜ | |

| 11 | 单场比赛可玩(选车→进赛道→跑一圈) | ⬜ | “可玩”最低门槛 |

| 12 | 菜单/存档/多模式稳定 | ⬜ | |



\### 1. UI 字体:换成阿里巴巴普惠体大子集(阶段7,已真机确认)

\- 源:`AlibabaPuHuiTi-3-55-Regular.ttf`(静态 TTF,免费商用,非可变字体)。

\- 子集:在用汉字 + GB2312 全区 + ASCII + 中文标点 = \*\*7540 字\*\*,写回

&#x20; `romfs/ui/NotoSansCJKsc-RR3Bootstrap.otf`(\*\*保留原文件名,免改加载代码\*\*)。

\- \*\*关键陷阱\*\*:NRO 嵌的是预建镜像 `rr3.romfs`,不是 `romfs/` 目录。

&#x20; 之前只改 `romfs/` 目录、`make` 显示成功但 NRO 里字体没变——空转了好几轮。

&#x20; 已加自动重建规则:

&#x20; ```make

&#x20; ROMFS\_FILES := $(shell find $(ROMFS) -type f)

&#x20; $(ROMFS\_IMAGE): $(ROMFS\_FILES)

&#x20; 	@build\_romfs $(ROMFS) $(ROMFS\_IMAGE)

&#x20; $(BUILD): $(ROMFS\_IMAGE)

&#x20; ```

&#x20; NRO 内 `"Alibaba PuHuiTi".encode("utf-16-be")` = True,旧 Noto = False。

\- ⚠️ 二次陷阱:`cp` 还原镜像会把 mtime 设为“现在”,导致 make 跳过重建;需 `touch`

&#x20; romfs 下某文件或删镜像来强制重建。



&#x20; `\[4 字节小端 = 解压后大小]\[标准 zlib 流(0x78 …)]`。

&#x20; → 解法:`uncompress(data + 4, ...)`(\*\*跳过前 4 字节\*\*,否则 zlib 报错)。

\- 解压后是标准 \*\*DDS\*\*,`sprites\_0` = \*\*1024×1024,DXT1,1 mip\*\*。

\- DXT1/S3TC 是 \*\*Switch/Tegra 原生支持\*\*,可直接 `glCompressedTexImage2D` 上传,

&#x20; 无需 CPU 解块。



核对依据:`graphics\_probe.cpp`(282行)、`guest\_filesystem.cpp`(592行)、`Makefile`(88行)。

\- \*\*不新建文件、不改 Makefile\*\*:`-lz` 已在 LIBS(第32行);`\*.cpp` 通配自动纳入。

\- \*\*直接内联进 `graphics\_probe.cpp`\*\* 已验证的渲染探测末尾,\*\*复用现成 `realProgram`\*\*,不重开 EGL。

\- \*\*VFS 读取用 `rr3::guestFopen` / `rr3::guestOpendir`\*\*;传相对路径

&#x20; `.depot/gui\_assets/960x640/...` 经 `resolvePath` 的 `.depot/` 分支

&#x20; (guest\_filesystem.cpp:244)+ `contentDevice`(:92)正确路由到 `rr3gui1`(gui-sd 卷)。

\- 新增探测标志位:`assetOpened / assetInflated / assetDdsParsed / s3tcSupported /

&#x20; realAssetUploaded / realAssetDrawCompleted` + `assetWidth/Height/Fourcc`。

\- 探测链路:VFS 打开 → 剥4字节头 + zlib 解压(校验长度)→ 校验 DDS 魔数/fourCC

&#x20; → 查 `texture\_compression\_s3tc` 扩展 → `glCompressedTexImage2D` 上传 DXT1

&#x20; → 复用 realProgram 画一帧 → 全通则 `stage = 10`。



1\. \*\*未在工程内编译过\*\*:阶段8 代码照真实源码写、集成点已对准,但沙箱访问不到磁盘,

&#x20;  无法编译验证。以本地 `make` 结果为准。\*\*特别留意 stage 递进顺序\*\*(见下方代码注释)。

2\. \*\*运行时前提\*\*:渲染子进程(`--rr3-renderer`)在探测前\*\*必须已调

&#x20;  `mountExternalData()`\*\*(guest\_filesystem.cpp:308),否则 `rr3gui1:` 卷没挂,

&#x20;  `assetOpened` 直接 false。需确认子进程启动流程。

3\. \*\*UI 遥测未接线\*\*:新增 6 个 bool + `assetFourcc/Width/Height` 尚未拼进

&#x20;  `ui\_dashboard`/遥测字符串,真机屏幕暂时看不到结果。

4\. \*\*无法在本工作站关设备门\*\*:Eden 0.2.1 仅回归用,卡在 Applet SelfController;

&#x20;  阶段8 真机验证需拷回 SD 卡实测。



1\. `graphics\_probe.hpp` 加 9 个字段(见第五节)。

2\. `graphics\_probe.cpp` 用第六节整份覆盖。

3\. 确认渲染子进程已调 `mountExternalData()`。

4\. 把新标志位接进 UI 遥测显示。

5\. `make`(docker),按 name 表招牌确认无回归。

6\. 拷回真机,看 stage 是否走到 \*\*10\*\*、`realAssetDrawCompleted` 是否为真。

&#x20;  → 通过即阶段8 达成:真实资源管线(VFS 读→解压→DDS→压缩纹理→绘制)整条打通。



```cpp

bool assetOpened = false;            // 阶段8-1: VFS 读到真实资源

bool assetInflated = false;          // 阶段8-2: 剥4字节头+zlib 解压且长度吻合

bool assetDdsParsed = false;         // 阶段8-3: DDS 魔数/DXT fourCC 合法

bool s3tcSupported = false;          // 阶段8-4: 驱动 expose s3tc

bool realAssetUploaded = false;      // 阶段8-5: glCompressedTexImage2D 无错

bool realAssetDrawCompleted = false; // 阶段8-6: 真实纹理绘制无错

uint32\_t assetWidth = 0;

uint32\_t assetHeight = 0;

char assetFourcc\[8] = {0};

```



\## 六、graphics\_probe.cpp 全文(整份覆盖版,含阶段8)



```cpp

\#include "rr3/graphics\_probe.hpp"



\#include "rr3/guest\_filesystem.hpp"



\#ifndef GL\_COMPRESSED\_RGBA\_S3TC\_DXT1\_EXT

\#define GL\_COMPRESSED\_RGBA\_S3TC\_DXT1\_EXT 0x83F1

\#ifndef GL\_COMPRESSED\_RGBA\_S3TC\_DXT5\_EXT

\#define GL\_COMPRESSED\_RGBA\_S3TC\_DXT5\_EXT 0x83F3



namespace rr3 {



GraphicsProbe runWindowGraphicsProbe(NWindow\* window) {

&#x20;   GraphicsProbe probe{};

&#x20;   probe.attempted = true;

&#x20;   probe.surfaceMode = 1;

&#x20;   if (window == nullptr) {

&#x20;       probe.eglError = EGL\_BAD\_NATIVE\_WINDOW;

&#x20;       return probe;

&#x20;   }



&#x20;   probe.stage = 1;

&#x20;   EGLDisplay display = eglGetDisplay(EGL\_DEFAULT\_DISPLAY);

&#x20;   if (display == EGL\_NO\_DISPLAY) {

&#x20;       probe.eglError = eglGetError();

&#x20;       return probe;

&#x20;   }

&#x20;   probe.displayCreated = true;

&#x20;   probe.stage = 2;

&#x20;   if (eglInitialize(display, \&probe.major, \&probe.minor) != EGL\_TRUE) {

&#x20;       probe.eglError = eglGetError();

&#x20;       eglTerminate(display);

&#x20;       return probe;

&#x20;   }

&#x20;   probe.initialized = true;



&#x20;   probe.stage = 3;

&#x20;   constexpr EGLint configAttributes\[] = {

&#x20;       EGL\_RED\_SIZE, 8,

&#x20;       EGL\_GREEN\_SIZE, 8,

&#x20;       EGL\_BLUE\_SIZE, 8,

&#x20;       EGL\_ALPHA\_SIZE, 8,

&#x20;       EGL\_DEPTH\_SIZE, 24,

&#x20;       EGL\_STENCIL\_SIZE, 8,

&#x20;       EGL\_NONE,

&#x20;   };

&#x20;   EGLConfig config{};

&#x20;   EGLint configCount = 0;

&#x20;   if (eglChooseConfig(display, configAttributes, \&config, 1, \&configCount) != EGL\_TRUE ||

&#x20;       configCount == 0) {

&#x20;       probe.eglError = eglGetError();

&#x20;       eglTerminate(display);

&#x20;       return probe;

&#x20;   }

&#x20;   probe.configChosen = true;

&#x20;   probe.stage = 4;



&#x20;   EGLSurface surface = eglCreateWindowSurface(display, config, window, nullptr);

&#x20;   if (surface == EGL\_NO\_SURFACE) {

&#x20;       probe.eglError = eglGetError();

&#x20;       eglTerminate(display);

&#x20;       return probe;

&#x20;   }

&#x20;   probe.surfaceCreated = true;

&#x20;   probe.stage = 5;



&#x20;   constexpr EGLint contextAttributes\[] = {

&#x20;       EGL\_CONTEXT\_CLIENT\_VERSION, 2,

&#x20;       EGL\_NONE,

&#x20;   };

&#x20;   EGLContext context = eglCreateContext(

&#x20;       display, config, EGL\_NO\_CONTEXT, contextAttributes);

&#x20;   if (context == EGL\_NO\_CONTEXT) {

&#x20;       probe.eglError = eglGetError();

&#x20;       eglDestroySurface(display, surface);

&#x20;       eglTerminate(display);

&#x20;       return probe;

&#x20;   }

&#x20;   probe.contextCreated = true;

&#x20;   probe.stage = 6;



&#x20;   if (eglMakeCurrent(display, surface, surface, context) == EGL\_TRUE) {

&#x20;       probe.madeCurrent = true;

&#x20;       const GLubyte\* version = glGetString(GL\_VERSION);

&#x20;       std::snprintf(probe.version, sizeof(probe.version), "%s",

&#x20;           version != nullptr ? reinterpret\_cast<const char\*>(version) : "unknown");

&#x20;       glViewport(0, 0, 1280, 720);

&#x20;       glClearColor(0.03f, 0.08f, 0.12f, 1.0f);

&#x20;       glClear(GL\_COLOR\_BUFFER\_BIT);

&#x20;       glFinish();

&#x20;       probe.glError = glGetError();

&#x20;       const bool swapped = eglSwapBuffers(display, surface) == EGL\_TRUE;

&#x20;       if (!swapped) {

&#x20;           probe.eglError = eglGetError();

&#x20;       }

&#x20;       probe.drawCompleted = probe.glError == GL\_NO\_ERROR \&\& swapped;

&#x20;       probe.stage = probe.drawCompleted ? 7 : 6;



&#x20;       // Minimal ES2 shader-compile probe: proves the Switch GLES driver

&#x20;       // accepts GLSL source compilation before RR3's real shaders are tried.

&#x20;       // Uses no RR3 assets; a self-contained vertex+fragment pair.

&#x20;       {

&#x20;           static const char\* kVertexSource =

&#x20;               "attribute vec4 aPos;\\n"

&#x20;               "void main() { gl\_Position = aPos; }\\n";

&#x20;           static const char\* kFragmentSource =

&#x20;               "precision mediump float;\\n"

&#x20;               "void main() { gl\_FragColor = vec4(1.0, 0.5, 0.0, 1.0); }\\n";



&#x20;           GLuint vs = glCreateShader(GL\_VERTEX\_SHADER);

&#x20;           glShaderSource(vs, 1, \&kVertexSource, nullptr);

&#x20;           glCompileShader(vs);

&#x20;           GLint vsStatus = GL\_FALSE;

&#x20;           glGetShaderiv(vs, GL\_COMPILE\_STATUS, \&vsStatus);

&#x20;           probe.vertexShaderCompiled = vsStatus == GL\_TRUE;

&#x20;           if (!probe.vertexShaderCompiled) {

&#x20;               glGetShaderInfoLog(vs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;           }



&#x20;           GLuint fs = glCreateShader(GL\_FRAGMENT\_SHADER);

&#x20;           glShaderSource(fs, 1, \&kFragmentSource, nullptr);

&#x20;           glCompileShader(fs);

&#x20;           GLint fsStatus = GL\_FALSE;

&#x20;           glGetShaderiv(fs, GL\_COMPILE\_STATUS, \&fsStatus);

&#x20;           probe.fragmentShaderCompiled = fsStatus == GL\_TRUE;

&#x20;           if (!probe.fragmentShaderCompiled \&\& probe.shaderLog\[0] == '\\0') {

&#x20;               glGetShaderInfoLog(fs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;           }



&#x20;           if (probe.vertexShaderCompiled \&\& probe.fragmentShaderCompiled) {

&#x20;               GLuint program = glCreateProgram();

&#x20;               glAttachShader(program, vs);

&#x20;               glAttachShader(program, fs);

&#x20;               glLinkProgram(program);

&#x20;               GLint linkStatus = GL\_FALSE;

&#x20;               glGetProgramiv(program, GL\_LINK\_STATUS, \&linkStatus);

&#x20;               probe.programLinked = linkStatus == GL\_TRUE;

&#x20;               if (!probe.programLinked \&\& probe.shaderLog\[0] == '\\0') {

&#x20;                   glGetProgramInfoLog(program, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;               }

&#x20;               glDeleteProgram(program);

&#x20;           }



&#x20;           glDeleteShader(vs);

&#x20;           glDeleteShader(fs);

&#x20;           if (probe.stage == 7 \&\& probe.programLinked) {

&#x20;               probe.stage = 8;

&#x20;           }

&#x20;       }



&#x20;       // Realistic-usage probe: a textured, transformed ES2 pipeline closer to

&#x20;       // what RR3 actually issues (mat4 uniform, varyings, sampler2D/texture2D),

&#x20;       // plus a real texture upload and a textured draw. Still uses no RR3

&#x20;       // assets — a 2x2 RGBA texture generated in-place. Gated on the toy

&#x20;       // program linking so we don't chase secondary failures.

&#x20;       if (probe.programLinked) {

&#x20;           static const char\* kRealVertex =

&#x20;               "attribute vec4 aPos;\\n"

&#x20;               "attribute vec2 aUV;\\n"

&#x20;               "uniform mat4 uMVP;\\n"

&#x20;               "varying vec2 vUV;\\n"

&#x20;               "void main() { vUV = aUV; gl\_Position = uMVP \* aPos; }\\n";

&#x20;           static const char\* kRealFragment =

&#x20;               "precision mediump float;\\n"

&#x20;               "varying vec2 vUV;\\n"

&#x20;               "uniform sampler2D uTex;\\n"

&#x20;               "uniform vec4 uTint;\\n"

&#x20;               "void main() { gl\_FragColor = texture2D(uTex, vUV) \* uTint; }\\n";



&#x20;           GLuint rvs = glCreateShader(GL\_VERTEX\_SHADER);

&#x20;           glShaderSource(rvs, 1, \&kRealVertex, nullptr);

&#x20;           glCompileShader(rvs);

&#x20;           GLint rvsStatus = GL\_FALSE;

&#x20;           glGetShaderiv(rvs, GL\_COMPILE\_STATUS, \&rvsStatus);



&#x20;           GLuint rfs = glCreateShader(GL\_FRAGMENT\_SHADER);

&#x20;           glShaderSource(rfs, 1, \&kRealFragment, nullptr);

&#x20;           glCompileShader(rfs);

&#x20;           GLint rfsStatus = GL\_FALSE;

&#x20;           glGetShaderiv(rfs, GL\_COMPILE\_STATUS, \&rfsStatus);



&#x20;           probe.realisticShaderCompiled = rvsStatus == GL\_TRUE \&\& rfsStatus == GL\_TRUE;

&#x20;           if (!probe.realisticShaderCompiled \&\& probe.shaderLog\[0] == '\\0') {

&#x20;               if (rvsStatus != GL\_TRUE) {

&#x20;                   glGetShaderInfoLog(rvs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;               } else {

&#x20;                   glGetShaderInfoLog(rfs, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;               }

&#x20;           }



&#x20;           GLuint realProgram = 0;

&#x20;           if (probe.realisticShaderCompiled) {

&#x20;               realProgram = glCreateProgram();

&#x20;               glAttachShader(realProgram, rvs);

&#x20;               glAttachShader(realProgram, rfs);

&#x20;               glBindAttribLocation(realProgram, 0, "aPos");

&#x20;               glBindAttribLocation(realProgram, 1, "aUV");

&#x20;               glLinkProgram(realProgram);

&#x20;               GLint rLink = GL\_FALSE;

&#x20;               glGetProgramiv(realProgram, GL\_LINK\_STATUS, \&rLink);

&#x20;               probe.realisticProgramLinked = rLink == GL\_TRUE;

&#x20;               if (!probe.realisticProgramLinked \&\& probe.shaderLog\[0] == '\\0') {

&#x20;                   glGetProgramInfoLog(realProgram, sizeof(probe.shaderLog), nullptr, probe.shaderLog);

&#x20;               }

&#x20;           }



&#x20;           GLuint tex = 0;

&#x20;           if (probe.realisticProgramLinked) {

&#x20;               const unsigned char pixels\[] = {

&#x20;                   255, 0, 0, 255, 0, 255, 0, 255,

&#x20;                   0, 0, 255, 255, 255, 255, 0, 255,

&#x20;               };

&#x20;               glGenTextures(1, \&tex);

&#x20;               glBindTexture(GL\_TEXTURE\_2D, tex);

&#x20;               glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_MIN\_FILTER, GL\_NEAREST);

&#x20;               glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_MAG\_FILTER, GL\_NEAREST);

&#x20;               glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_WRAP\_S, GL\_CLAMP\_TO\_EDGE);

&#x20;               glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_WRAP\_T, GL\_CLAMP\_TO\_EDGE);

&#x20;               glTexImage2D(GL\_TEXTURE\_2D, 0, GL\_RGBA, 2, 2, 0, GL\_RGBA,

&#x20;                   GL\_UNSIGNED\_BYTE, pixels);

&#x20;               probe.textureUploaded = glGetError() == GL\_NO\_ERROR;

&#x20;           }



&#x20;           if (probe.textureUploaded) {

&#x20;               static const float kIdentity\[16] = {

&#x20;                   1.0f, 0.0f, 0.0f, 0.0f,

&#x20;                   0.0f, 1.0f, 0.0f, 0.0f,

&#x20;                   0.0f, 0.0f, 1.0f, 0.0f,

&#x20;                   0.0f, 0.0f, 0.0f, 1.0f,

&#x20;               };

&#x20;               static const float kVerts\[] = {

&#x20;                   0.5f, -0.5f, 1.0f, 0.0f,

&#x20;                   0.5f, 0.5f, 1.0f, 1.0f,

&#x20;               };

&#x20;               glUseProgram(realProgram);

&#x20;               GLint mvpLoc = glGetUniformLocation(realProgram, "uMVP");

&#x20;               GLint texLoc = glGetUniformLocation(realProgram, "uTex");

&#x20;               GLint tintLoc = glGetUniformLocation(realProgram, "uTint");

&#x20;               glUniformMatrix4fv(mvpLoc, 1, GL\_FALSE, kIdentity);

&#x20;               glActiveTexture(GL\_TEXTURE0);

&#x20;               glBindTexture(GL\_TEXTURE\_2D, tex);

&#x20;               glUniform1i(texLoc, 0);

&#x20;               glUniform4f(tintLoc, 1.0f, 1.0f, 1.0f, 1.0f);

&#x20;               glEnableVertexAttribArray(0);

&#x20;               glVertexAttribPointer(0, 2, GL\_FLOAT, GL\_FALSE,

&#x20;                   4 \* sizeof(float), kVerts);

&#x20;               glEnableVertexAttribArray(1);

&#x20;               glVertexAttribPointer(1, 2, GL\_FLOAT, GL\_FALSE,

&#x20;                   4 \* sizeof(float), kVerts + 2);

&#x20;               glDrawArrays(GL\_TRIANGLE\_STRIP, 0, 4);

&#x20;               probe.texturedDrawCompleted = glGetError() == GL\_NO\_ERROR;

&#x20;               glDisableVertexAttribArray(0);

&#x20;               glDisableVertexAttribArray(1);

&#x20;               glUseProgram(0);

&#x20;           }



&#x20;           // stage 8 -> 9：合成带纹理绘制通过

&#x20;           if (probe.stage == 8 \&\& probe.texturedDrawCompleted) {

&#x20;               probe.stage = 9;

&#x20;           }



&#x20;           // ---- 阶段8：真实 RR3 资源加载探测（复用 realProgram，仍处于 current 上下文）----

&#x20;           // 运行时扫 gui-sd 卷目录取第一个 .dds.z，剥 \[u32 解压后大小]\[zlib流] 头，

&#x20;           // zlib 解压 -> 解析 DDS -> glCompressedTexImage2D 上传 DXT1 -> 复用 realProgram 画一帧。

&#x20;           // 门控在 stage==9(合成带纹理绘制成功)之上，避免追逐次生失败。

&#x20;           if (probe.stage == 9 \&\& realProgram != 0) {

&#x20;               char assetPath\[0x200] = {0};

&#x20;               const char\* kAssetDir = ".depot/gui\_assets/960x640";

&#x20;               if (DIR\* dir = rr3::guestOpendir(kAssetDir)) {

&#x20;                   while (struct dirent\* ent = readdir(dir)) {

&#x20;                       const char\* n = ent->d\_name;

&#x20;                       size\_t nlen = std::strlen(n);

&#x20;                       if (nlen > 6 \&\& std::strcmp(n + nlen - 6, ".dds.z") == 0) {

&#x20;                           std::snprintf(assetPath, sizeof(assetPath), "%s/%s", kAssetDir, n);

&#x20;                           break;

&#x20;                       }

&#x20;                   }

&#x20;                   closedir(dir);

&#x20;               }



&#x20;               std::FILE\* af = assetPath\[0] != '\\0' ? rr3::guestFopen(assetPath, "rb") : nullptr;

&#x20;               if (af != nullptr) {

&#x20;                   probe.assetOpened = true;

&#x20;                   std::fseek(af, 0, SEEK\_END);

&#x20;                   long asize = std::ftell(af);

&#x20;                   std::fseek(af, 0, SEEK\_SET);

&#x20;                   std::vector<unsigned char> rawa(asize > 0 ? static\_cast<size\_t>(asize) : 0);

&#x20;                   size\_t got = rawa.empty() ? 0 : std::fread(rawa.data(), 1, rawa.size(), af);

&#x20;                   std::fclose(af);



&#x20;                   std::vector<unsigned char> dds;

&#x20;                   if (got == rawa.size() \&\& rawa.size() > 6 \&\& rawa\[4] == 0x78) {

&#x20;                       uint32\_t declared = 0;

&#x20;                       std::memcpy(\&declared, rawa.data(), 4);

&#x20;                       dds.resize(declared);

&#x20;                       uLongf destLen = declared;

&#x20;                       if (uncompress(dds.data(), \&destLen, rawa.data() + 4,

&#x20;                                      static\_cast<uLong>(rawa.size() - 4)) == Z\_OK \&\&

&#x20;                           destLen == declared) {

&#x20;                           probe.assetInflated = true;

&#x20;                       }

&#x20;                   }



&#x20;                   GLenum glFormat = 0;

&#x20;                   GLsizei blockBytes = 0;

&#x20;                   if (probe.assetInflated \&\& dds.size() >= 128 \&\&

&#x20;                       std::memcmp(dds.data(), "DDS ", 4) == 0) {

&#x20;                       std::memcpy(\&probe.assetHeight, dds.data() + 12, 4);

&#x20;                       std::memcpy(\&probe.assetWidth, dds.data() + 16, 4);

&#x20;                       std::memcpy(probe.assetFourcc, dds.data() + 84, 4);

&#x20;                       if (std::memcmp(probe.assetFourcc, "DXT1", 4) == 0) {

&#x20;                           glFormat = GL\_COMPRESSED\_RGBA\_S3TC\_DXT1\_EXT; blockBytes = 8;

&#x20;                       } else if (std::memcmp(probe.assetFourcc, "DXT5", 4) == 0) {

&#x20;                           glFormat = GL\_COMPRESSED\_RGBA\_S3TC\_DXT5\_EXT; blockBytes = 16;

&#x20;                       }

&#x20;                       probe.assetDdsParsed = glFormat != 0;

&#x20;                   }



&#x20;                   const char\* exts = reinterpret\_cast<const char\*>(glGetString(GL\_EXTENSIONS));

&#x20;                   probe.s3tcSupported = exts != nullptr \&\&

&#x20;                       std::strstr(exts, "texture\_compression\_s3tc") != nullptr;



&#x20;                   GLuint assetTex = 0;

&#x20;                   if (probe.assetDdsParsed \&\& probe.s3tcSupported) {

&#x20;                       GLsizei imgSize =

&#x20;                           ((static\_cast<GLsizei>(probe.assetWidth) + 3) / 4) \*

&#x20;                           ((static\_cast<GLsizei>(probe.assetHeight) + 3) / 4) \* blockBytes;

&#x20;                       if (128 + static\_cast<size\_t>(imgSize) <= dds.size()) {

&#x20;                           glGenTextures(1, \&assetTex);

&#x20;                           glBindTexture(GL\_TEXTURE\_2D, assetTex);

&#x20;                           glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_MIN\_FILTER, GL\_LINEAR);

&#x20;                           glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_MAG\_FILTER, GL\_LINEAR);

&#x20;                           glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_WRAP\_S, GL\_CLAMP\_TO\_EDGE);

&#x20;                           glTexParameteri(GL\_TEXTURE\_2D, GL\_TEXTURE\_WRAP\_T, GL\_CLAMP\_TO\_EDGE);

&#x20;                           glCompressedTexImage2D(GL\_TEXTURE\_2D, 0, glFormat,

&#x20;                               static\_cast<GLsizei>(probe.assetWidth),

&#x20;                               static\_cast<GLsizei>(probe.assetHeight), 0,

&#x20;                               imgSize, dds.data() + 128);

&#x20;                           probe.realAssetUploaded = glGetError() == GL\_NO\_ERROR;

&#x20;                       }

&#x20;                   }



&#x20;                   if (probe.realAssetUploaded) {

&#x20;                       static const float kIdentity2\[16] = {

&#x20;                           1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,

&#x20;                           0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f,

&#x20;                       };

&#x20;                       static const float kAssetVerts\[] = {

&#x20;                           -0.5f, -0.5f, 0.0f, 0.0f,   0.5f, -0.5f, 1.0f, 0.0f,

&#x20;                           -0.5f,  0.5f, 0.0f, 1.0f,   0.5f,  0.5f, 1.0f, 1.0f,

&#x20;                       };

&#x20;                       glUseProgram(realProgram);

&#x20;                       glUniformMatrix4fv(glGetUniformLocation(realProgram, "uMVP"), 1, GL\_FALSE, kIdentity2);

&#x20;                       glActiveTexture(GL\_TEXTURE0);

&#x20;                       glBindTexture(GL\_TEXTURE\_2D, assetTex);

&#x20;                       glUniform1i(glGetUniformLocation(realProgram, "uTex"), 0);

&#x20;                       glUniform4f(glGetUniformLocation(realProgram, "uTint"), 1.0f, 1.0f, 1.0f, 1.0f);

&#x20;                       glEnableVertexAttribArray(0);

&#x20;                       glVertexAttribPointer(0, 2, GL\_FLOAT, GL\_FALSE, 4 \* sizeof(float), kAssetVerts);

&#x20;                       glEnableVertexAttribArray(1);

&#x20;                       glVertexAttribPointer(1, 2, GL\_FLOAT, GL\_FALSE, 4 \* sizeof(float), kAssetVerts + 2);

&#x20;                       glDrawArrays(GL\_TRIANGLE\_STRIP, 0, 4);

&#x20;                       probe.realAssetDrawCompleted = glGetError() == GL\_NO\_ERROR;

&#x20;                       glDisableVertexAttribArray(0);

&#x20;                       glDisableVertexAttribArray(1);

&#x20;                       glUseProgram(0);

&#x20;                   }



&#x20;                   if (assetTex != 0) glDeleteTextures(1, \&assetTex);

&#x20;                   if (probe.realAssetDrawCompleted) probe.stage = 10;

&#x20;               }

&#x20;           }

&#x20;           // ---- 阶段8 结束 ----



&#x20;           if (tex != 0) {

&#x20;               glDeleteTextures(1, \&tex);

&#x20;           }

&#x20;           if (realProgram != 0) {

&#x20;               glDeleteProgram(realProgram);

&#x20;           }

&#x20;           glDeleteShader(rvs);

&#x20;           glDeleteShader(rfs);

&#x20;       }



&#x20;       eglMakeCurrent(display, EGL\_NO\_SURFACE, EGL\_NO\_SURFACE, EGL\_NO\_CONTEXT);

&#x20;   }

&#x20;   else {

&#x20;       probe.eglError = eglGetError();

&#x20;   }



&#x20;   eglDestroyContext(display, context);

&#x20;   eglDestroySurface(display, surface);

&#x20;   eglTerminate(display);

&#x20;   return probe;

}



} // namespace rr3

```

