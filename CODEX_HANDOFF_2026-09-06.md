# Real Racing 3 Switch Native Rehost：开发进度与 Codex 交接

> 更新日期：2026-09-09  
> 项目目录：`E:\AI\RealRacing3-switch`  
> 当前版本：`0.22.4`，最新修复：`input-pump-v1`；保留 quiesce-v4 / termination-v2  
> 当前结论：Guest View + 帧循环已跑通（round 9：`frames=872`，`live=6`，棋盘格）。已按 Java `ControllerManager` ABI 接入 Joy-Con→`ControllerConnectedJNI`/`SetButtonValueJNI`/`SetJoystickValueJNI`。有 worker 时 PLUS 直接 dirty-exit（不恢复 native UI）。待真机：是否连上手柄、按键是否进 guest、画面是纯蓝还是棋盘格。  
> 本文用途：让没有此前聊天上下文的 Codex 接手开发。本文是状态快照与开发计划，不是完成声明。

## 1. 给 Codex 的首要指令

1. 先阅读本文，再核对对应源码；若代码或新真机证据发生变化，以新证据为准。
2. 不要从头重做 loader、TLS 或 JNI 生命周期；这些已有可工作的实现及历史真机验证。
3. 当前第一任务是定位 `(6)` 轮次 Render 前最后日志之后的报错退出，优先覆盖主动终止和异常采集盲区，再验收 Render/swap；不要直接跳到持久帧循环。
4. exception-v1 已确认旧工作线程空 TLS 故障，pthread-tls-v1 已交付；随后 Created/Changed 返回，最新日志到 onViewRenderJNI。用户确认黑屏更久后仍报错退出，crash 仅 armed，尚未取得这次退出的真实根因。
5. 最终目标是原始 AArch64 RR3 DSO 在 Switch 原生 NRO 内启动、进入离线比赛、接受手柄输入并完成一场比赛。用户明确要求“后面做到可以上机游玩再停”。
6. 若需要用户进行真机验证，交付可运行产物和具体步骤，明确等待哪项证据；不要把一次诊断迭代完成说成整个移植完成。
7. 保留 recovery guard。不要清除尚未备份的现场，不要绕过失败继续执行 guest。
8. 用户已明确授权：为了尽快解决本阶段，可直接替换不合适的现有实现，由开发者决定技术路线；这不代表允许覆盖原始游戏数据、丢弃未备份现场或跳过安全检查。

### 本次更新摘要

| 事项 | 当前状态 |
|---|---|
| callers-v1 真机日志 | 已收到，仍停在 stage 34，完整 caller/参数已保存 |
| malloc40 来源 | 已确认 RR3 `+0x1102940` 返回地址 |
| memcpy 来源及数据 | 已确认 RR3 `+0x15d13c4`，复制 367 字节顶点着色器源码 |
| free 来源 | 已确认 libc++ `+0xc7fd0`，字符串 `__grow_by_and_replace` |
| 真正故障 PC / 根因 | 已确认 RR3 `+0x1c69ab8`，新建 guest 工作线程 TPIDR_EL0 为零 |
| exception-v1 | 真机已成功捕获本次异常寄存器及落盘，仍非所有异常场景的保证 |
| shader-v1 | 真机最后日志为 glLinkProgram return，stage 45、program=4；链接状态未查询 |
| pthread-tls-v1 | 修复已交付；随后 `(6)` 日志到 Render 前，Created/Changed 返回，Render 尚未验收 |
| 线程退出自测 | 已增加正常返回、显式 pthread_exit、TSD destructor 的 TLS 检查；待在 Switch 启动时执行 |
| 阶段统计 | 已改为事件位图，不再以最大/最后编号推断较早事件发生 |
| 最新本地验证 | 构建成功，10/10 契约测试，七卷+program 哈希通过 |
| Guest View + frame loop | **round 9 真机**：`frames=872`，`live=6`，画面棋盘格 |
| Joy-Con 输入 | **input-pump-v1 已交付**：ControllerManager JNI 全套，见 `guest_input.cpp` |
| quiesce-v4 | dirty-exit → `handoff-process-exit` / `view-probe-dirty-exit` → `svcExitProcess` |
| 最新真机待办 | progress 应有 `guest-controller-connected`；帧循环内试 A/B/摇杆；画面是纯蓝（guest 没画）还是棋盘格（guest 画了垃圾） |

本次新增了真机异常捕获及根因确认；修复代码本地验证通过，不等于修复效果已通过真机验收。

## 2. 范围和平台约束

### 2.1 技术目标

- Nintendo Switch Horizon OS，原生 libnx NRO。
- 不是 Switchroot Android，不嵌入完整 Android Runtime。
- 原始 AArch64 guest DSO 与宿主处于同一进程。
- 核心映像：
  - `libRealRacing3.so`
  - `libNimble.so`
  - `libfmodex.so`
  - `libc++_shared.so`
- 宿主提供 ELF 加载、符号绑定、必要 Bionic/POSIX/JNI 兼容以及 EGL/GLES、输入、音频适配。
- 外部资源使用七个 FAT32-safe 只读 RomFS 数据卷；保存和缓存由现有文件系统适配层负责。

### 2.2 授权范围

这是已授权的本地兼容性和平台移植研究。

- 不修改游戏签名。
- 不绕过 DRM。
- 不伪造账号、购买或服务器认证。
- 不公开分发 EA 游戏数据或逆向得到的源码。
- 离线启动和本地单人兼容在范围内。
- 在线多人、账号认证、购买和服务端 entitlement 绕过不在范围内。
- 若发现离线启动仍依赖真实服务端授权，应记录限制，不得以伪造授权成功来推进。

## 3. 当前详细进度

### 3.1 证据等级

- **历史真机通过**：此前用户返回的 Switch 测试已证明相应边界。
- **本地验证**：编译、源码契约、静态 ELF 分析或产物哈希通过；不等同于游戏行为正确。
- **部分通过**：内部若干调用已返回，但整个功能未完成。
- **待真机验证**：代码已经交付，尚无该版运行结果。
- **未验收**：未建立满足最终目标的运行证据；不一定代表所有底层代码都不存在。

不提供任意“总完成百分比”：启动兼容性仍有未知阻塞，loader 完成不代表游戏移植接近完成。

### 3.2 模块状态表

| 模块 | 当前成果 | 证据 / 状态 | 尚缺事项 |
|---|---|---|---|
| 原生构建 | devkitPro + libnx 可生成 NRO | 本地构建及历史真机运行通过 | 持续保持可复现构建 |
| 四 DSO loader | 映射、依赖解析、重定位、保留映像 | 历史真机通过 | 新符号兼容问题按证据处理 |
| 可执行内存和权限 | ModuleCode 风格映射、RX/R/RW、RELRO | 历史真机 11/11 权限范围通过 | 不破坏映射与回收顺序 |
| 动态链接 API | guest dlopen/dlsym/dladdr/dlclose/dlerror | 已实现，历史 loader 验证 | 不代表所有运行期加载场景通过 |
| 生命周期清理 | 逆序 finalizer、逆序 unload/unmap | 历史安全退出路径通过 | 后续帧循环和线程停止必须接入 |
| Guest TLS | TPIDR_EL0 临时指向 Android-compatible TLS，+0x28 canary | 真机 self-test `0xf` | 后续线程/TLS 行为继续审计 |
| Dependency constructors | libc++ 1、Nimble 0、FMOD 2 | 历史真机 3/3 返回 | FMOD 构造器成功不等于音频工作 |
| JNI_OnLoad | 在主构造器前运行，初始化 VM 缓存 | 历史真机返回 `0x00010006` | 更广 JNI 语义仍待覆盖 |
| RR3 main constructors | 181 个主构造器 | 历史真机全部返回 | 不等于主菜单启动成功 |
| Activity 生命周期 | onCreateJNI、onStartJNI、onResumeJNI | 历史真机返回，相关 unsupported JNI 为 0 | View 生命周期仍阻塞 |
| JNI bridge | fake JNIEnv/JavaVM/Activity，233 slots，分阶段统计 | 已实现且通过此前生命周期场景 | 输入、菜单及运行时 Java 服务按需补齐 |
| 外部数据 | 42,541 文件、9,630,901,568 原始内容字节、7 个卷 | 真机 7/7 mount，VFS self-test `0xfff` | 所有游戏资产访问路径仍需运行验证 |
| 宿主独立 renderer | EGL/GLES2、真实 DXT 资源解压/DDS/S3TC upload/draw | 历史真机通过 | 不能据此宣称 guest renderer 成功 |
| Guest View EGL | 同进程建立 EGL；host GL 在 GuestTlsScope 内外均测过 | 部分通过 | 三个 guest View callback 和 swap 未整体验收 |
| Guest View 内部初始化 | Created/Changed 已返回，最新文件到 onViewRenderJNI 前 | `(6)` 轮次；crash 仅 armed | Render 返回及 swap、实际运行状态待确认 |
| callers-v1 诊断 | 返回日志保留调用地址和 memcpy/free 参数 | 真机已验证字段写出，已完成调用归因 | 保留为后续异常定位上下文 |
| exception-v1 异常采集 | 预打开独立文件，记录 PC/LR/SP/FP/FAR/ESR/PSTATE/X0–X28 | 真机已成功捕获这次 TLS 空指针异常 | 仍保留 best-effort 限制 |
| shader-v1 调用追踪 | glCreateShader/glShaderSource/glCompileShader/glLinkProgram 边界 | 真机记录到 LinkProgram 返回 | 不代表编译/链接状态成功 |
| Guest pthread TLS | 每个新线程安装独立 GuestTlsScope，覆盖 guest 入口及退出析构 | pthread-tls-v1 已本地验证并交付 | 修复效果、运行期 self-test 及后续线程生命周期待真机验证 |
| 阶段事件统计 | 64 位事件位图；allocation flags 和 copy gate 按真实记录事件判断 | 已替换旧数值比较逻辑，本地测试通过 | 后续多线程与实际运行验证 |
| 持久帧循环 | 目前仍是一次性 View 探针 | 未验收 | 同进程 EGL/context 存续、update/render/swap |
| 游戏手柄输入 | loader UI 有输入；guest 驾驶映射尚未验收 | 未验收 | JNI 参数、连接、按钮、摇杆、菜单导航 |
| 音频播放 | 有 FMOD DSO 和兼容层基础 | 未验收 | 实际设备、线程、混音与音频输出 |
| 主菜单 / 离线比赛 | 尚无成功运行证据 | 未验收 | 进入比赛、驾驶、完成比赛 |

### 3.3 当前四映像 loader 的已验证计数

以下来自既有真机 loader report，不应与旧的单 DSO / 双 DSO 统计混用：

| 指标 | 数值 |
|---|---:|
| 映像数量 | 4 |
| 映射字节 | 35,999,744 |
| Relative relocations | 145,752 |
| Internal relocations | 3,026 |
| Host relocations | 731 |
| Cross-DSO relocations | 8,899 |
| Weak relocations | 37 |
| Patched writes | 158,445 |
| Verified writes | 158,445 |
| Missing strong relocations | 0 |
| Invalid relocations | 0 |
| 权限范围 | 11/11 |
| Dependency constructors | 3 |
| Main constructors | 181 |
| Startup plan fingerprint | `999a029e95729174` |

这个 plan fingerprint 不是 NRO 内容哈希，不能用它识别是否安装了最新诊断版本。

## 4. 当前阻塞和最新真机证据

### 4.1 最新现场：pthread-tls-v1 后已推进到 Render

用户在本次文档更新过程中提供 `(6)` 轮次文件：

```text
# rr3-native-rehost.guest-boot (6).progress
phase=onViewRenderJNI ordinal=0 target=0000000a13262614

# rr3-native-rehost.crash-000000cc89971ab9.txt
RR3 exception-v1
status=armed

# rr3-native-rehost.guarded-action (6).pending
pending=guest-boot
plan=999a029e95729174 images=4 dependency_ctors=3 main_ctors=181 image0=libc++_shared.so@0000000521a82000+101000:c1:f2 image1=libNimble.so@0000000d7fba0000+3000:c0:f2 image2=libfmodex.so@000000068249d000+189000:c2:f1 image3=libRealRacing3.so@0000000a11faf000+1fc8000:c181:f2
clear_only_after_success=1
```

按 `runGuestViewProbe()` 的顺序，这一轮已从 Created 和 Changed 回调返回，并写出了调用 Render 前的 breadcrumb。target 对应 RR3 `+0x12b3614`。尚无 Render 返回、swap 或完整 View 完成的记录。

用户随后明确说明：最后一次 ZL 后黑屏持续时间比之前更长，但仍然报错退出。因此不是仍在运行中采集，也不能把这轮记为成功。crash 只有 `status=armed`，说明没有落盘的异常寄存器；尚不能区分主动 abort/exit、未交给 handler 的系统终止或异常采集失败。Created/Changed 返回已有顺序证据，Render 前 breadcrumb 不等于精确退出位置。

### 4.2 已修复故障的真机证据：工作线程缺少 TLS

上一轮 exception-v1 的关键原始数据：

```text
file=rr3-native-rehost.crash-0000000857bcbeb2.txt
status=exception
handler=000000071c2f6130
error_desc=0000000000000101
pc=0000000ba8d9bab8
lr=0000000ba8d9b9dc
sp=000000000f037ef0
fp=000000000f037f50
far=0000000000000028
esr=0000000092000006
pstate=0000000020000000
tpidr_el0=0000000000000000
x23=0000000000000000
RR3_base=0000000ba7132000
```

同轮主探针最后记录 `view-gl-link-program-return ordinal=45 target=4`，`gl_caller=0000000ba7cafaec`（RR3 `+0xb7daec`）。这是主探针的进度，不是工作线程故障 PC。

地址及指令核对：

```text
PC - RR3_base = 0x1c69ab8
LR - RR3_base = 0x1c699dc
0x1c699d0: worker start wrapper
0x1c699d8: bl 0x1c69a98
0x1c69ab4: mrs x23, tpidr_el0
0x1c69ab8: ldr x8, [x23, #0x28]  # 此处异常
```

ESR 的 EC=0x24 为 lower-EL data abort，DFSC=0x06 为 level-2 translation fault；结合 FAR=0x28、X23=0 和前一条 MRS，确认是读取空 guest TLS 的 canary 槽，而不是 canary 比较失败。

静态重定位确认创建路径的 PLT slot `0x1f7eb48` 绑定 `pthread_create@LIBC`。旧 `guestPthreadCreate()` 直接执行 `::pthread_create(..., start, argument)`，没有新线程 GuestTlsScope，构成与现场一致的具体缺陷。修复实现见第 7.1 节。

### 4.3 历史现场：callers-v1 的 shader 字符串归因

以下为更早的 `(4)` 轮次，不能与上述异常或 `(6)` 的映像地址混用。临时文件数字会重复，按实际上传轮次及内容配对。

`rr3-native-rehost.guest-boot (4).progress`：

```text
phase=view-post-gl-free-return ordinal=34 target=0000000437a2d880 trace=callers-v1 malloc40_caller=0000000e6b5fd940 copy_caller=0000000e6bacc3c4 dst=0000000437a2d880 src=0000000e6a986b7b len=16f free_caller=0000000c28424fd0 ptr=0000000437a2d880
```

同轮 `rr3-native-rehost.guarded-action (4).pending`：

```text
pending=guest-boot
plan=999a029e95729174 images=4 dependency_ctors=3 main_ctors=181 image0=libc++_shared.so@0000000c2835d000+101000:c1:f2 image1=libNimble.so@00000007d1b31000+3000:c0:f2 image2=libfmodex.so@0000000dbfa0f000+189000:c2:f1 image3=libRealRacing3.so@0000000e6a4fb000+1fc8000:c181:f2
clear_only_after_success=1
```

临时文件目录：`C:\Users\L\AppData\Local\Temp\`。文件可能被后续附件覆盖，本文保留原始内容供交接。

### 4.4 历史 caller 地址归因

| 字段 | 换算结果 | 解释 |
|---|---|---|
| malloc40_caller | RR3 `+0x1102940` | 与 `0x110293c` 的间接 allocation 调用一致 |
| copy_caller | RR3 `+0x15d13c4` | 对应 `0x15d13c0` 的 `memcpy@plt` 调用 |
| src | RR3 `+0x48bb7b` | 本地原始 DSO 中的顶点着色器源码 |
| len | `0x16f = 367` 字节 | 源码包含 a_pos/a_uv/a_colour、u_mvp、gl_Position |
| free_caller | libc++ `+0xc7fd0` | `basic_string::__grow_by_and_replace` 内的释放调用之后 |
| dst / ptr | 同为 `0x437a2d880` | 先被选中为 memcpy 目标，后被选中释放；不单凭地址相同认定 use-after-free |

libc++ 包含该返回地址的动态符号：

```text
_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE21__grow_by_and_replaceEmmmmmmPKc
```

静态工具已核对 ELF PT_LOAD：这两个代码地址在 VA 与文件偏移相同的首个 segment 中；其他 segment 有差值，仍必须按 ELF 映射换算。

**已确认**：stage 34 不是此前猜测的 vector storage free；执行已涉及顶点着色器文本处理及 libc++ 字符串扩容路径，被追踪的 memcpy/free 均返回。guest-boot marker 仍保留，完整 View 生命周期尚未验收。

### 4.5 历史 stage 34 证据的解释边界

以下限制针对当时仅有 stage 34 的证据；后续新增的异常根因和 Render 进展见 4.1–4.2。

- 不能把 stage 34 归因于 RR3 `0x1102984` 的 vector 释放。
- 不能把最后返回的 free 当成精确崩溃位置。
- 不能认定字符串扩容、分配器或 libc++ ABI 一定有错。
- 不能认定源码已提交 GPU，或 glCompileShader/glLinkProgram 已执行成功。
- 没有动态证据直接证明 `0x1110058` 第二次调用返回、completion byte 写入或 canary 检查失败。
- 不能因缺少 stack-failure breadcrumb 就排除栈损坏、写盘失败或更晚异常。
- 当前 memcpy/free trace 只选择第一次符合条件的调用，不能排除后续未记录的同类调用。

**重要纠错**：此前把静态条件分支说成“真机已发生栈检查失败”的结论已撤回；当前没有这样的运行证据。下一轮用异常寄存器和 shader 边界验证，不再沿该假设修复。

## 5. 当前诊断实现：exception-v1 + shader-v1 + callers-v1

### 5.1 主要源码

- [guest_view_trace.cpp](native-switch/source/guest_view_trace.cpp)：trace 状态、malloc/memcpy/free/memset/glGetString 包装和 stack failure 诊断。
- [guest_view_trace.hpp](native-switch/include/rr3/guest_view_trace.hpp)：对外 trace 接口。
- [recovery_guard.cpp](native-switch/source/recovery_guard.cpp)：durable progress 写入、flush、fsync、marker 管理。
- [recovery_guard.hpp](native-switch/include/rr3/recovery_guard.hpp)：`writeGuestBootProgress(..., const char* details = nullptr)`。
- [bionic_shims.cpp](native-switch/source/bionic_shims.cpp)：`bionicStackChkFail()` 先记录 trace，再维持原有 abort。
- [symbol_resolver.cpp](native-switch/source/symbol_resolver.cpp)：guest malloc/memcpy/free/memset/glGetString 的宿主绑定。

### 5.2 完整阶段表

| Stage | 意义 |
|---|---|
| 1 / 2 | GL_VERSION enter / return |
| 3 / 4 | GL_VENDOR enter / return |
| 5 / 6 | GL_RENDERER enter / return |
| 7 / 8 | GL_EXTENSIONS enter / return |
| 9 / 10 | malloc(0x2800) enter / return |
| 11 / 12 | malloc(0x3e38) enter / return |
| 13 / 14 | memset(0x1550) enter / return |
| 15 / 16 | memset(0x320) enter / return |
| 17 / 18 | memset(0xe98) enter / return |
| 19 / 20 | malloc(0x478) enter / return |
| 21 / 22 | memset(0xc01) enter / return |
| 23 / 24 | 第一个 malloc(0x240) enter / return |
| 25 / 26 | malloc(0x130) enter / return |
| 27 / 28 | 被计数的第一个 malloc(0x40) enter / return |
| 29 / 30 | 被计数的第二个 malloc(0x40) enter / return |
| 31 / 32 | stage 条件满足后的第一个 memcpy enter / return |
| 33 / 34 | 上述 memcpy 返回后的第一个 free enter / return |
| 35 | stack failure：trace 入口与失败时 TLS / canary 相同 |
| 36 | stack failure：TPIDR_EL0 与 trace 入口不同 |
| 37 | stack failure：TLS 相同但 canary 不同，或基线不可用 |
| 38 / 39 | glCreateShader enter / return |
| 40 / 41 | glShaderSource enter / return |
| 42 / 43 | glCompileShader enter / return |
| 44 / 45 | glLinkProgram enter / return |

35、36、37 是互斥分类，不是依次执行的三个阶段。38–45 对 View 探针期间每次相应调用写出；重复使用同一阶段编号不表示只有一次调用。

- CreateShader enter 的 target 为 shader type，return 为 shader ID。
- ShaderSource enter 的 target 为 count，return 为 shader ID。
- CompileShader / LinkProgram 的 target 为 shader / program ID。
- 调用返回不代表 `GL_COMPILE_STATUS` / `GL_LINK_STATUS` 成功；当前包装器没有主动追加这些查询。
- 所有 shader 参数原样传给真实 host GL；诊断不解引用来源字符串指针，也不修改 shader 内容。

### 5.3 实际过滤规则及局限

- wrapper 未启用时，调用原有 host malloc/memcpy/free 等，不改变其语义。
- `g_checkpointInProgress` 用于避免诊断写入重入干扰相关包装器。
- malloc(0x240) 只选第一次。
- malloc(0x40) 只有 `guestViewTraceReached(26)` 才参与计数，只选前两次。
- memcpy 需要 trace 启用、非 checkpoint、`guestViewTraceReached(30)`，并且此前选中的 memcpy 尚未返回。
- free 在选中的 memcpy 返回后只选第一次。
- **过滤依据是 size / 顺序 / 已记录事件，不是 guest call-site 身份。**
- `g_lastStage` 仅表示最近记录的阶段；新增 `uint64_t g_seenStages` 位图保存阶段事件，enable 时重置。
- `guestViewTraceReached(stage)` 按位判断；View report 的 allocation called/returned 字段已改用它，不再使用 `lastTraceStage >= N`。
- 位图表示本进程 trace 曾记录该事件，不是所有 breadcrumb 都已可靠写盘的证明；durable 文件仍是中断进程证据。
- 这项修正避免 shader 阶段编号较大时错误开启 memcpy 追踪或把未发生的 allocation 标成完成。
- 当前共享 trace 字段仍不是通用多线程事件系统，但 `g_enabled` 已改为 host thread_local，只有 probe 线程启用该日志流；其他线程的 CPU fault 通过 exception handler 获取。
- 返回阶段写出后，到 wrapper 真正返回 guest 之间仍有少量宿主指令；breadcrumb 不等于精确故障 PC。

### 5.4 callers-v1 保留的字段

从 stage 30 起，progress 行附加：

```text
trace=callers-v1 malloc40_caller=<hex> copy_caller=<hex> dst=<hex> src=<hex> len=<hex> free_caller=<hex> ptr=<hex> gltrace=shader-v1 gl_caller=<hex> gl_object=<hex>
```

| 字段 | 含义 |
|---|---|
| `malloc40_caller` | 被计数的第二个 0x40 allocation 的返回地址 |
| `copy_caller` | 被追踪 memcpy 的返回地址 |
| `dst` / `src` | memcpy 的目标 / 来源地址 |
| `len` | memcpy 字节数，十六进制 |
| `free_caller` | 被追踪 free 的返回地址 |
| `ptr` | 调用 free 前保存的 allocation 地址 |
| `gl_caller` | 最近一次受追踪 shader API 的返回地址 |
| `gl_object` | 最近的 shader/program ID；CreateShader enter 时为 0 |

`gltrace=shader-v1` 标识该版具备 shader 追踪，不单凭这个字段认定 shader API 已执行；还需检查实际阶段及 gl_caller。

- 所有数值按十六进制解释。
- 对 caller 字段，0 表示尚未捕获；参数中的 0 也可能是真实零长度或 null，必须结合阶段解释。
- 返回日志保留这些字段，避免覆盖 enter 日志后失去调用来源。
- 不读取或转储 guest 指针指向的内容，不在 free 后解引用指针。
- `.progress` 完整文件可能比 dashboard/report 缓冲区长，因此必须索取原文件，而非只要截图或截断的 report。
- 新版仍可能停在 stage 34；这不意味着诊断没有价值，关键是这次能还原 caller 和参数。

### 5.5 地址换算

对每个 caller：

1. 先判断地址是否落在同次 pending marker 的某个 guest image 范围。
2. 若落入，减去该映像的运行时 load bias。当前映像布局可结合 marker base 使用，但应核对 ELF 最小 PT_LOAD 虚拟地址，不要泛化到任意 ELF。
3. 得到的是返回地址对应的 guest VA；普通 AArch64 `bl/blr` 调用点通常是该值减 4。
4. 结合原始指令确认是否有 thunk / tail call；不要盲目把任何地址都当直接调用点。
5. 如果地址不属于四个 guest image，检查 host 或 thunk 来源，不得强行减 RR3 base。
6. 对堆地址 dst/src/ptr，不要减 RR3 base 后作为代码地址解释。
7. 每次启动地址会变，严禁跨测试轮次混用 pending 和 progress。
8. 反汇编时用 ELF PT_LOAD 将 VA 转成文件偏移，不能默认 `file_offset == VA`。

### 5.6 exception-v1：真实异常现场采集

实现位置：[recovery_guard.cpp](native-switch/source/recovery_guard.cpp)。

**进入探针前**：

1. `main.cpp` 在关闭 native UI 和进入 Guest View 前调用 `prepareGuestCrashCapture()`。
2. 使用 `fsdevGetDeviceFileSystem("sdmc")` 获取 SD 文件系统。
3. 创建独立文件 `/switch/rr3-native-rehost.crash-<system-tick>.txt`；旧文件不会被覆盖，若名字冲突则创建失败。
4. 预打开 `FsFile`，写入并 flush `RR3 exception-v1 / status=armed`。
5. 创建、打开或 flush 失败时阻止本次 Guest View，保留 recovery marker。
6. 若 probe 正常返回，调用 `finishGuestCrashCapture()` 关闭文件。文件仍可能显示 armed，不代表异常或成功的独立证明。

**异常回调**：

- 已根据本地 libnx `exception.o` 反汇编确认 `__libnx_exception_handler(ThreadExceptionDump*)` 接口。
- 提供强符号异常处理函数、16-byte aligned 的 64 KiB exception stack 和相应 stack size。
- 使用原子重入保护，重复进入时直接终止进程。
- 固定 2 KiB 输出缓冲，自行格式化十六进制，不使用 malloc、stdio、snprintf 或 guest 指针解引用。
- 记录 `handler` 自身运行时地址、error_desc、PC、LR、SP、FP、FAR、ESR、PSTATE、X0–X28。
- `tpidr_el0` 是 handler 执行时读取的寄存器值，不冒充异常结构中不存在的已保存字段。
- 使用 `svcOutputDebugString` 及预打开文件的 `fsFileWrite(..., FsWriteOption_Flush)`。
- 最后 `svcExitProcess()`，**不恢复 guest，不执行 guest finalizers，不清除 guard**；因此可能直接回到 hbmenu，而不是显示原来的错误对话框。

**局限必须明确**：

- 异常入口是全局的，但文件仅在 Guest View 前准备；更早的启动异常不保证有该文件。
- 这是 best-effort 捕获。调试器、未交给该入口的 fatal、第二次异常、文件系统服务失败/阻塞或掉电，都可能导致没有完整 dump。
- 只有 `status=armed` 时，不能说 handler 已被触发；应把文件一起发回排查。
- 当前异常时写入的返回值没有持久化，不能宣称所有异常一定成功落盘。
- 不读取 guest 栈内容、不做自动 backtrace；用保存的寄存器和 ELF 静态映射分析。
- exception-v1 已有一次真机成功捕获及持久化证据，不能扩大为所有故障都能捕获。

`0x1e130` 是捕获工作线程故障的旧 exception-v1 构建中 handler 的 ELF 地址，不是永久地址。pthread-tls-v1 已重新链接；定位 host PC 必须用对应版本 ELF 的 nm 地址和 dump.handler 计算 load bias，不能跨版本混用。

## 6. 静态反汇编已有线索

下表保留早期初始化函数的静态线索，不再将它们当作 stage 34 释放的来源；真实 copy/free 归因见第 4 节。

原始库位置：`reverse/native/arm64/libRealRacing3.so`。

| Guest VA | 静态含义 |
|---|---|
| `0x110204c` | 已分析过的对象初始化 helper |
| `0x110261c` | 后续初始化函数入口 |
| `0x1102710` | 第一次调用 `0x1110058` |
| `0x1102744` | 间接分配 0x130 |
| `0x110284c` | 第一个候选 0x40 vector storage allocation |
| `0x110293c` | 第二个候选 0x40 vector storage allocation |
| `0x1102958` | 条件路径上的 memcpy 调用 |
| `0x1102984` | 条件路径上的间接释放调用，实际绑定应再核查 |
| `0x1102a60` | 再次调用 `0x1110058` |
| `0x1102a68` | 写 completion byte |
| `0x1102a74` | 比较 canary |
| `0x1102a78` | 条件跳转到失败路径 |
| `0x1102a98` | 正常 return |
| `0x1102a9c` | 调用 stack failure PLT |
| `0x1110058` | element size 为 0x48 的 vector helper |
| `0x1e17c00` | 已由符号化反汇编确认 `memcpy@plt` |
| `0x1e17b70` | 已由符号化反汇编确认 `__stack_chk_fail@plt` |

补充：

- `0x1110058` 初始 capacity growth 可能分配 `8 * 0x48 = 0x240`。
- 再次插入时，如果容量充足，可以不经过 allocation/copy/free import，而直接执行 guest load/store。
- 因此“没看到新的 malloc”不等于 helper 没有执行。
- 早先把后续增长猜成第二个 0x240，后来根据反汇编改为两个 0x40。不要恢复 `postGlMalloc240Second` 或加入没有证据的 0x360。

## 7. 关键架构和不可破坏的不变量

### 7.1 Guest TLS

源码：[tls_probe.cpp](native-switch/source/tls_probe.cpp)、[tls_probe.hpp](native-switch/include/rr3/tls_probe.hpp)。

- GuestTlsScope 保存旧 TPIDR_EL0，安装包含 +0x28 canary 的 TLS block，退出恢复原值。
- 宿主 libnx/newlib 的线程变量访问使用 TPIDRRO_EL0；不能笼统认为“在 guest TLS 下调用任何宿主 libc 都必然冲突”。
- 某些第三方库仍可能有自身 TLS 假设，需按实际调用验证。
- 当前同步 View 探针中的 GuestTlsScope 是局部对象。后续持久帧循环 / 异步 guest 线程必须重新审查其生命周期，不得让线程继续引用已销毁的 block。

### 7.1a pthread-tls-v1：已交付的工作线程 TLS 修复

源码：[pthread_bridge.cpp](native-switch/source/pthread_bridge.cpp)。

- `GuestThreadStartContext` 保存 guest start routine 与 argument；创建前由宿主 malloc 分配。
- `guestPthreadCreate()` 改为把宿主 `guestThreadStart` trampoline 传给 pthread_create，而非直接传 guest 入口。
- 子线程复制 start context 后释放它，再在自己的线程栈创建 `GuestTlsScope`。不复用父线程 TLS，不依赖父线程栈寿命。
- GuestTlsScope active 后才调用 guest routine；若安装失败则终止，不带着无效 TLS 继续执行。
- guest routine 正常返回后调用 `guestPthreadExit(result)`，而不是从 trampoline 返回后先销毁 TLS frame。
- 显式 guest pthread_exit 也在同一活跃 TLS frame 下终止。
- 本地 libnx 反汇编证实 pthread_exit → threadExit 会同步执行 thread-specific-data（TSD）析构；因此必须保持 worker TLS 至这些析构完成。
- 此终止路径不会执行栈上 GuestTlsScope 的 C++ 析构；线程结束后线程栈由宿主生命周期处理，没有需要手工释放的 TLS 堆对象。
- pthread_create 失败时由父线程释放尚未消费的 start context，返回错误；成功后父线程不再访问该 context。
- 已检查生成指令：TLS constructor → guest blr → pthread_exit，中间没有 TLS destructor。

**自测变化**：原 `pthreadBridgeSelfTest()` bit 6 现在检查两条退出路径：正常返回与显式 pthread_exit。要求 worker TLS 非零且不同于 parent TLS、canary 非零、join 返回值正确、TSD destructor 仍看到同一 worker TLS/canary、parent TLS 未改变。总预期值仍为 `0x3ff`。本地静态契约通过不代表该运行期自测已在真机通过；需要 dashboard/报告或启动输出证据。

**诊断并发修正**：`guest_view_trace.cpp` 的 enable flag 改为 `thread_local bool g_enabled`；只启用 probe 所在线程的 breadcrumb，工作线程不再竞争改写共享 trace 字段。生成的 host `__aarch64_read_tp` 使用 TPIDRRO_EL0，和 guest TPIDR_EL0 分开。worker 的 CPU fault 仍由进程异常 handler 捕获。

**最新验证边界**：修复包已发给用户，随后 `(6)` 轮次日志已推进到 onViewRenderJNI，表明 Created/Changed 已返回。这是运行推进证据，但没有单独证明所有 worker、自测、退出析构或 Render 均成功。修复没有实现线程关闭协调；未来在 EGL teardown、finalizers 和 unmap 前仍必须确认 guest 活动已停止。

### 7.2 JNI 顺序与 ABI

源码：[jni_bridge.cpp](native-switch/source/jni_bridge.cpp)、[jni_bridge.hpp](native-switch/include/rr3/jni_bridge.hpp)。

现有顺序：

```text
加载/重定位/权限完成
→ dependency constructors
→ JNI_OnLoad
→ 181 个主构造器
→ onCreateJNI
→ onStartJNI
→ onResumeJNI
→ Guest View EGL probe
```

- JNI_OnLoad 先于主构造器是当前兼容实现中已验证的关键顺序：Nimble 的 getEnv() 依赖先缓存 JavaVM。
- JNIEnv table 有 233 slots；232 为 GetObjectRefType。
- JavaVM GetEnv 在 index 6 / byte offset 48；AttachCurrentThread 在 index 4。
- 使用 `guestJniEnvironment()` 和 `guestActivityObject()`，不要传未初始化或 null 的替代句柄。
- View-changed 的 JNI diff 必须保持 `diffJniHistograms(afterChanged, afterCreated)`，不能与 beforeCreated 比较。
- unsupported JNI 为 0 只说明已执行场景没有走 unsupported slot，不证明所有 fake 方法的语义等同于 Android。

### 7.3 EGL / NWindow 所有权

源码：[main.cpp](native-switch/source/main.cpp)、[guest_view_probe.cpp](native-switch/source/guest_view_probe.cpp)。

当前顺序必须保留：

```text
prepareGuestCrashCapture() 成功
→ nativeUiExit()
→ nwindowGetDefault() 重新获取
→ EGL display/config/surface/context/makeCurrent
→ host GL outside/inside GuestTlsScope
→ guest View callbacks
→ swap（若能够到达）
→ EGL teardown
→ finishGuestCrashCapture()
→ nativeUiInit()
```

- native framebuffer UI 与 EGL 不能同时拥有 default NWindow。
- 独立 renderer child NRO 的成功不能证明 guest callbacks 成功。
- 当前 View 探针调用三个 callback 后释放 EGL，不是持久游戏 loop。
- 当前 callback ABI：Created `(env, activity)`；Changed `(env, activity,1280,720,0,0)`；Render `(env,activity,2,0)`。
- Render 的 `2,0` 是当前探针的 mode/flags，不是宽高。真正帧驱动时继续核对 ABI 和时序。

### 7.4 Recovery guard

- 危险 guest 操作前必须已可靠写入 pending marker。
- 在 View 探针期间沿用已 armed 的 guest-boot guard。
- 正常完成 finalizer/unmap 后才清除 marker。
- 不做 unguarded retry、catch-and-continue 或在故障后继续调用 guest。
- 进程已闪退时，不能要求用户在已关闭的程序里按 B 或 PLUS。
- 先备份现场；之后重启到 Recovery 且无初始化运行时，才允许 B 清除旧 marker。

## 8. 源码导航

| 文件 / 目录 | 接手时用途 |
|---|---|
| `native-switch/source/main.cpp` | ZR/ZL guard、生命周期编排、UI/EGL handoff |
| `native-switch/source/multi_dso_linker.cpp` | 四映像重定位、映射、构造/析构、export resolution |
| `native-switch/include/rr3/elf_preflight.hpp` | loader/probe 对外结构与接口 |
| `native-switch/source/elf_preflight.cpp` | ELF 校验和早期加载分析 |
| `native-switch/source/guest_view_probe.cpp` | 同进程 EGL 与三个 View callbacks |
| `native-switch/source/guest_view_trace.cpp` | caller 参数、shader 边界、事件位图和 stack-failure 分类 |
| `native-switch/source/recovery_guard.cpp` | durable marker/progress、exception-v1 文件准备与异常 handler |
| `native-switch/source/tls_probe.cpp` | GuestTlsScope |
| `native-switch/source/jni_bridge.cpp` | JNIEnv/JavaVM/handles/调用统计 |
| `native-switch/source/symbol_resolver.cpp` | host/shim symbol bindings |
| `native-switch/source/bionic_shims.cpp` | fortify 与 stack failure |
| `native-switch/source/pthread_bridge.cpp` | 后续异步 guest 和 FMOD 线程适配审计入口 |
| `native-switch/source/guest_filesystem.cpp` | 文件访问、数据卷、guest path ABI |
| `native-switch/source/graphics_probe.cpp` | 已工作的独立宿主 EGL/DXT 路径 |
| `native-switch/source/ui_dashboard.cpp` | report 文本、诊断字段 |
| `native-switch/source/native_ui.cpp` | 原生 loader UI，不是 guest 游戏 UI |
| `native-switch/tests/` | 九项源码/产物契约测试及 ELF reference 工具 |
| `tools/Build-NativeSwitch.ps1` | 当前构建入口，显式生成 rr3.romfs |
| `tools/verify_rr3_data_volumes.py` | 数据卷哈希、sentinel、manifest/NRO 校验 |
| `reverse/native/arm64/` | 本地原始 guest DSO |
| `native-switch/FORMAL_UI_0.22.4_TEST.md` | 当前真机操作与阶段说明 |

## 9. 当前交付物和本地验证

### 9.1 精确产物身份

- 构建产物：`native-switch/rr3-native-rehost.nro`
- 打包产物：`dist/rr3-native-rehost-complete-0.22.4/switch/rr3-native-rehost/rr3-native-rehost.nro`
- 本文编写时已重新核对两者字节一致。
- 大小：`51,638,773` bytes。
- SHA-256：

```text
83E3B615EF7CECB5EC59A9EC12F073F49D4D483670935AF32CF6CD92FE201CC3
```

- NRO 内存在 `RR3 exception-v1`、`gltrace=shader-v1` 和 `trace=callers-v1` 字符串。
- 内嵌 RomFS 大小：`43,882,800` bytes。
- 本文编写时重新验证 embedded RomFS 与 `native-switch/rr3.romfs` 内容一致。
- 多轮诊断复用了 0.22.4 和相同文件名，甚至文件大小相同；仅看版本/大小无法确认安装的是哪轮。

### 9.2 最近本地验证结果

- 最新 pthread-tls-v1 构建：exit code 0，NRO 大小仍为 51,638,773 字节，但内容哈希已变。
- 九项契约测试：9/9 通过；新增 worker TLS 安装、退出寿命、create-failure 释放和 thread-local trace enable 检查。
- 生成指令检查：trampoline 在调用 guest 前安装 TLS；正常返回走 pthread_exit，不先销毁 TLS frame；host thread-local helper 使用 TPIDRRO_EL0。
- 链接产物检查：exception handler 为强符号，64 KiB stack 已链接；handler 反汇编确认调用 raw FS/SVC 并以 svcExitProcess 终止，没有 malloc/printf 调用。
- 包内 checksums：10/10 独立校验通过。
- manifest 的 program bytes/hash 已更新；最近文档核对时 source/package/manifest/embedded RomFS 一致。
- 七个数据卷内容未变；本轮 `verify_rr3_data_volumes.py` 再次全部通过。
- 本地测试没有运行原始 guest 游戏完成比赛，不应称为 gameplay tests。
- callers-v1：stage 34 字符串路径归因已完成。
- exception-v1 + shader-v1：真机已捕获 worker 空 TLS 故障；主 probe 同时推进到 LinkProgram 返回。
- pthread-tls-v1：已交付；随后 `(6)` 日志到 Render 调用前，Created/Changed 已返回。尚无 Render returned/swap、自测 0x3ff 或完整线程退出验证报告。

这些本地检查不等于异常 handler 能在目标硬件的所有错误场景下成功落盘，也不等于 shader 编译或链接已经成功。

## 10. 可复现的构建和校验步骤

以下命令在项目根目录的 Git Bash 中执行；PowerShell 由命令显式调用。

### 10.1 构建

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1"
```

注意：

- 本地工具链位于 `.tools/devkitPro/msys64/`。
- 构建脚本复制四个 DSO、原始 UI 资源，并显式重建 `native-switch/rr3.romfs`，再执行 make。
- 只编辑 `romfs/` 后直接使用旧 NRO，不会更新内嵌内容。
- 最近构建存在 `find: 'romfs': No such file or directory` 的递归 make warning，但最终成功链接。不要把这个 warning 自动当成成功或失败；检查 exit code 和产物。
- `tools/Stage-NativeSwitch.ps1` 的既有内容仍针对旧版 0.18.3，不能直接拿它当当前 0.22.4 的打包入口。

### 10.2 运行九项契约测试

```bash
python -c "import pathlib, subprocess, sys; tests=sorted(pathlib.Path('native-switch/tests').glob('test_*.py')); results=[subprocess.run([sys.executable,str(t)]).returncode for t in tests]; print(f'Contracts passed: {results.count(0)}/{len(tests)}'); sys.exit(any(results))"
```

包括 dashboard、external-data VFS、graphics isolation、guest TLS、cleanup、memory model、native UI、release artifact、startup plan。

`compileall` 只能检查 Python 语法，不能替代执行这些测试。

### 10.3 打包

每次 NRO 改变后，按顺序完成：

1. 验证 NRO magic `NRO0`。
2. 从 NRO `0x18` 读取 image size，定位其后的 ASET header。
3. 从 ASET `+0x28/+0x30` 读取 RomFS offset/size，校验实际 embedded bytes 与 `rr3.romfs` 哈希一致。
4. 复制 NRO 到当前 0.22.4 package。
5. 更新 `switch/rr3-native-rehost/data/rr3-data-manifest.json` 的 `program.bytes` 和 `program.sha256`。
6. 重新生成包根目录 `checksums.sha256`，排除 checksum 文件自身，使用 UTF-8 保留 `README-一次复制.txt` 文件名。
7. 用流式哈希处理 GB 级数据卷，不把所有卷一次读入内存。
8. 独立验证 checksums，而不是只生成后就宣称校验通过。
9. 若只改 host 诊断，无需重建七个外部数据卷。
10. 交付 NRO 时附上 build 标识和 hash。若同步完整 package，manifest/checksums 也必须同步。

### 10.4 校验数据卷与 manifest

```bash
python tools/verify_rr3_data_volumes.py dist/rr3-native-rehost-complete-0.22.4
```

该命令验证七卷哈希、FAT32 文件大小限制、sentinel 路径及 program 大小/哈希。

## 11. 真机复现与现场采集

### 11.1 安装

只替换诊断 NRO：

```text
/switch/rr3-native-rehost/rr3-native-rehost.nro
```

七个 `.romfs` 数据卷保持不变。若只替换 NRO，卡上旧 manifest 的 program hash 可能不再对应新 NRO；包校验时要使用一起更新的 manifest，不能误判为数据卷损坏。

### 11.2 已有 crash marker 时

1. 先从 TF 卡备份下面的 crash-*.txt、progress、pending 和已有 report。
2. 启动程序，停在 Recovery。
3. 确认没有 guest 初始化活动，再按 B 清除旧 guard。
4. 运行 `ZR → ZL → ZL`。
   - ZR：依赖构造器。
   - 第一次 ZL：JNI_OnLoad、主构造器、Activity 生命周期。
   - 第二次 ZL：Guest View EGL 探针。

### 11.3 如果进程闪退

不要尝试在关闭的程序内按键。优先直接复制：

```text
/switch/rr3-native-rehost.crash-<system-tick>.txt
/switch/rr3-native-rehost.guest-boot.progress
/switch/rr3-native-rehost.guarded-action.pending
/switch/rr3-native-rehost/loader-report.txt
```

- 请发回最新同轮的前三份完整原始文件，即使 crash 文件只有 `status=armed`。
- report 可能不存在，或只是重启后导出的 Recovery 状态；它是补充，不替代 crash/progress/pending。
- 如果 probe 正常返回也可能留下 armed 文件，需结合 UI 和 report 判断，不能仅凭文件存在认定崩溃。
- Recovery 新进程中的 `guest_init=0`、`guest_view=0` 不是中断进程的执行历史。
- 同轮 crash/progress/pending 必须配对，文件名中的 tick 不应跨系统重启简单当成绝对时间戳。
- 下次重点查看 crash 是否为 `status=exception`，以及 progress 的 shader 阶段、`gltrace=shader-v1`、`gl_caller`。
- 若 crash 缺失、为空或仍为 armed，原样保存并说明当时是系统报错、直接返回 hbmenu 还是卡死。
- 再次启动前保存旧现场，防止覆盖。

### 11.4 如果探针返回且进程仍存活

- 检查三个 callback returned 和 swap 状态。
- `L+R+X` 开 developer mode，松开 L/R 后按 X 导出 report。
- 保存 report / progress，再通过 PLUS 运行正常 guarded cleanup。
- 一次 probe 成功仍不是可玩验收。

## 12. 后续开发计划：按依赖顺序推进

### 近期计划状态总览

| 阶段 | 状态 | 下一验收 |
|---|---|---|
| P0 调用来源归因 | 已完成 | 已证实 shader 文本 + libc++ string growth，不需重做 |
| P1a 异常/着色器诊断实现 | 已完成，并在真机成功捕获故障 | 保留异常诊断继续定位新阻塞 |
| P1b worker TLS 根因修复 | 根因确认，pthread-tls-v1 已交付；新日志到 Render 前，随后黑屏更久后报错退出 | 排查 Render 后退出原因并验收 TLS 自测 |
| P2 完整 View 生命周期 | Created/Changed 返回，Render/swap 未完成 | `(6)` 轮次最后 breadcrumb 为 onViewRenderJNI | Render 返回、swap 及安全 teardown |
| P3 持久帧循环 | 已规划，未验收 | 同进程 EGL/TLS/guest 连续运行并安全退出 |
| P4 手柄 | 已规划，未验收 | 菜单导航和比赛驾驶有效 |
| P5 音频 | 已规划，未验收 | 实际输出、线程生命周期和暂停恢复正常 |
| P6 菜单/离线比赛 | 已规划，未验收 | 真机驾驶并完成一场比赛 |
| P7 稳定性/发布 | 已规划，未验收 | 回归清单、诊断策略和可复现打包 |

### P0：callers-v1 现场归因——已完成

- [x] 收到 `trace=callers-v1` 的完整同轮 progress/pending。
- [x] 提取 malloc40/copy/free 返回地址、src/dst/len/ptr。
- [x] 按各自 DSO base 换算地址，并核查 ELF segment 映射。
- [x] 确认 RR3 `+0x1102940` allocation 返回、`+0x15d13c4` memcpy 返回。
- [x] 在本地原始 DSO 确认 367 字节顶点着色器文本。
- [x] 确认 free 位于 libc++ `+0xc7fd0` 的字符串扩容函数。
- [x] 排除“这个 stage 34 就是旧 vector 释放”的归因，更新调查方向。

完成的是**调用来源识别**，不是完整对象内存安全、字符串 ABI 或故障根因验证。

### P1a：异常寄存器与 shader 边界——已实现并完成一次真机捕获

- [x] 核查本地 libnx 异常入口及 callback ABI。
- [x] 实现预创建/预打开独立 crash 文件，准备失败则阻止 View。
- [x] 实现固定缓冲的异常寄存器输出，随后终止进程，保留 guard。
- [x] 实现 glCreateShader/glShaderSource/glCompileShader/glLinkProgram enter/return。
- [x] 实现事件位图，替换由阶段编号大小推测 allocation 完成的旧逻辑。
- [x] 本地构建、9/9 契约、链接符号/handler 反汇编检查、10/10 checksum。
- [x] 交付最新 NRO；数据卷不变。
- [x] 真机回收 crash/progress/pending，异常文件包含 status=exception。
- [x] PC/FAR/X23 与原始指令一致，确认 worker TLS 空基址；主 probe 同时到 stage 45。

### P1b：worker TLS 修复——代码已交付，运行推进已观察

- [x] 定位 RR3 +0x1c69ab8，确认 pthread_create 入口缺少 TLS。
- [x] 为创建桥增加 start context 和 worker-local GuestTlsScope trampoline。
- [x] 保留 TLS frame 至 pthread_exit 内的 TSD 析构；覆盖显式退出路径。
- [x] 创建失败释放 context；成功时由子线程接管；不共享父 TLS。
- [x] 扩展 pthreadBridgeSelfTest bit 6，涵盖私有 TLS、canary、正常/显式退出和析构。
- [x] 使用 host thread_local 开关隔离 probe 日志，保留异常捕获。
- [x] 9/9 契约、构建、生成指令检查、10/10 哈希、七卷验证通过并交付。
- [x] 后续 `(6)` 日志到 onViewRenderJNI，Created 和 Changed 已返回。
- [ ] 取得完整报告，确认 Guest pthread bridge 自测 `0x3ff`。
- [x] 用户确认 `(6)` 轮次是最后 ZL 黑屏更久后报错退出，不是运行中采集；crash 仍只有 armed。
- [ ] 排查 Render 后主动 abort/exit、系统终止或异常采集失败，取得这次退出的真实位置。
- [ ] 验证 Render 返回及 swap，继续按实际现场处理新的故障。
- [ ] 审计 guest worker 停止/加入与 EGL/finalizer/unmap 顺序；不能把 TLS 修复当成线程清理完成。

### 后续故障处理决策表

以下用于新阻塞，不能反过来替换已经确认的 worker TLS 根因：

| 新证据 | 下一操作 |
|---|---|
| crash 有 PC/LR/FAR/ESR | 先确定 guest/host 映像，换算 fault PC；解码异常类型，结合寄存器检查实际 fault 指令 |
| PC 位于 libc++ string growth | 检查对应对象、容量/长度、分配指针、跨 DSO 字符串布局和调用参数；不先替换 libc++ |
| PC 位于 host Mesa/driver | 用 handler 地址和同版 host ELF 求 load bias；调查真实 GL 调用、参数/context/TLS |
| progress 停在 shader enter | 结合 crash 区分 host 调用内故障与其他线程异常；没有 PC 时保留不确定性 |
| shader compile/link 已返回但画面仍失败 | 根据实际路径获取 compile/link status 和 info log；单纯 return 不是编译成功 |
| crash 只有 armed、为空或不存在 | 核查是否运行新版、准备是否成功、异常入口/终止方式及 FS 限制；优先改善现场采集 |
| View 三回调与 swap 都成功 | 保存 report/画面证据，进入 P2 验收和 P3 设计，不直接宣称可玩 |

新阻塞的调查原则：

- 以同轮原始文件确认新 PC/模块/指令，不把旧 crash 当成修复后重现。
- Render 无返回日志且 crash 只有 armed 时，先确认应用实际状态；必要时设计 Render 内部边界或挂起诊断。
- 若需读取 guest 内存，先验证 readable range。
- 不跳过失败 guest 指令，不恢复异常 guest，不伪造平台成功。
- 每次修复重新测试、构建、校验并取得真机反馈。

worker TLS 代码交付已完成；整个 View 启动仍未验收，不能标成可玩。

### P2：完成整个 View 生命周期

依赖：P1。

- [x] onViewCreatedJNI、onViewChangedJNI 返回（由 `(6)` 轮次到 Render 前的顺序证明）。
- [ ] onViewRenderJNI 返回。
- [ ] 一次真实 guest frame 和 eglSwapBuffers 成功。
- [ ] 分别检查 created/changed/render JNI diff；按真实 Java 方法语义补齐必要服务。
- [ ] 区分 loader UI、独立 renderer、guest 游戏画面。
- [ ] 检查 GL/EGL 错误及资源路径错误，避免以黑屏但无异常视为成功。
- [ ] 验证 EGL teardown、native UI 恢复、安全退出无回归。

**验收标准**：同进程完整 View 探针可重复在干净启动中完成，保存 report 和画面证据。

### P3：建立持久游戏 update/render loop

依赖：P2。

- [ ] 将一次性 EGL 探针演进为明确的游戏运行状态，不在每帧重建 EGL。
- [ ] 保持 guest 映像、TLS、EGL context/surface 及相关资源在整个运行期有效。
- [ ] 依据实际 guest ABI 建立 update/render 时序和 frame pacing。
- [ ] 处理 applet 退出、前后台、暂停/恢复和屏幕变化。
- [ ] 退出时先停止 guest 活动及相关线程，再销毁 EGL、执行 finalizers 和卸载映像。
- [ ] 正常模式不每帧 fsync，大量逐帧诊断不能成为正式 loop 的默认开销。

**建议验收标准**：稳定连续渲染至少数分钟，能够正常退出，无已观察到的持续内存增长或 NWindow 竞争。

### P4：接入 Switch 手柄及菜单输入

依赖：P2/P3 的可交互画面；ABI 研究可提前做。

- [ ] 核查 guest 导出和 Java 包装层中的输入签名，不能只按方法名字猜参数。
- [ ] 调查 ControllerConnectedJNI、SetButtonValueJNI、SetJoystickValueJNI 等候选入口是否存在及真实 ABI。
- [ ] 映射 libnx Npad/Joy-Con 连接、断开、按钮按下/释放、摇杆方向和死区。
- [ ] 支持菜单确认/返回、暂停和驾驶必需操作。
- [ ] 若界面依赖触摸，确定合法的触摸/指针适配方式。
- [ ] 不让 loader 的 ZL/ZR/PLUS 控制与游戏运行态输入冲突。

**验收标准**：真机可以从菜单选择内容，驾驶时转向、油门、刹车、暂停/返回有效；断开重连不会保留卡住的按键状态。

### P5：完善 FMOD / 音频兼容

依赖：可稳定运行的 guest；静态 ABI 和线程审计可与 P4 并行。

- [ ] 区分 FMOD 构造器完成、设备初始化、混音线程、实际音频输出四个层级。
- [ ] 跟踪缺失设备/API、pthread、TLS、同步和缓冲区要求。
- [ ] 实现实际需要的音频输出适配，不把空 stub 当成音频成功。
- [ ] 检查音频回调寿命、退出时线程 join、暂停/恢复和 underrun。
- [ ] 若临时静音用于隔离问题，应显式标记，不作为最终音频验收。

**验收标准**：菜单/比赛音效能输出，暂停/恢复和退出不死锁或访问已卸载代码。

### P6：主菜单、存档和离线单人比赛

依赖：P3、P4，P5 按实际启动依赖推进。

- [ ] 主菜单可进入且关键资产正常显示。
- [ ] 审计真实资源请求、shader/texture 能力差异、保存和缓存路径。
- [ ] 进入合法可用的离线单人内容，加载车辆与赛道。
- [ ] 完成一次真实控制的比赛，记录结果界面或其他完成证据。
- [ ] 保存后重启验证数据持久化；不要破坏用户现有存档。
- [ ] 若缺少合法数据或必须服务端授权，准确报告阻塞，不伪造解锁。

**最终验收标准**：真实 Switch 上可启动、进入菜单、选择离线比赛、使用手柄驾驶并完成比赛，能安全退出。

### P7：稳定性与交付整理

依赖：P6 达成基本可玩。

- [ ] 建立重复启动/退出、暂停/恢复、比赛重载的回归清单。
- [ ] 记录性能、内存和音频稳定性，不要求在启动失败阶段提前优化。
- [ ] 给每个测试包独立 build ID，避免同版号同文件名混淆。
- [ ] 清理已无必要的高频诊断，同时保留可用的 crash/progress 机制。
- [ ] 整理实际可复现的 staging 脚本，替代旧版硬编码入口。
- [ ] 更新过时项目文档，明确哪些阶段是真机通过、哪些仅本地测试。

## 13. 历史误区与后续注意事项

1. `PORT_PROGRESS.md` 混有旧单/双 DSO 数量和过时百分比，甚至仍有“No game constructor ... executed”的描述；不能用它否定已通过的 181 个构造器。
2. 旧计划中“第二个 0x240”已被修正，不恢复其字段或 stage 定义。
3. 静态反汇编显示 `b.ne __stack_chk_fail` 只说明存在分支，不是分支被执行的证据。
4. 当前 trace 不是所有 malloc/memcpy/free 的完整历史，缺失某个日志不等于对应功能未调用。
5. memcpy/free 已返回不排除先前内存破坏；下一轮必须结合参数和真实代码路径判断。
6. `target` 在 allocation/free return 中是堆地址，不用于定位 guest PC。
7. Recovery report 的零值是新进程状态，必须与 durable progress 分开解读。
8. stage 35-37 表示 stack-failure 分类；`ordinal` 不是“此前最后完成的阶段”。
9. 当前 canary 分类只比较 trace 入口和失败时的 TLS/canary；没有直接读取并验证 guest 保存的栈槽，不能单凭分类断言具体哪次写越界。
10. 仅修改 `romfs/` 不代表 NRO 内嵌 RomFS 自动更新，使用构建脚本并校验 ASET。
11. PowerShell 放在 Bash 双引号里时 `$变量` 可能被提前展开；优先调用 `.ps1`，或正确使用外层单引号。
12. checksum 文件含 Unicode 路径，使用 UTF-8，不能用 ASCII 生成。
13. subprocess 调用 Windows ELF 工具时可用绝对路径，避免此前相对可执行文件路径解析失败的问题。
14. `python -c` 多行代码不要把字面 `\n` 直接放在语句之间；用有效单行语句、正确多行引用或实际脚本。
15. 本文生成时项目按非 Git 仓库环境工作；接手先核查 Git 状态，不假设存在分支/提交，也不要擅自初始化仓库或覆盖未知改动。

## 14. 可直接复制给 Codex 的任务说明

```text
请接手 E:\AI\RealRacing3-switch 的 Real Racing 3 Switch native rehost。
先阅读 CODEX_HANDOFF_2026-09-06.md，再核对当前源码和最新 NRO。

已验证：四 DSO loader、重定位/权限、Guest TLS、JNI_OnLoad、181 主构造器、
onCreate/onStart/onResume、七卷外部数据和独立宿主 EGL/DXT renderer。
尚未可玩：最新 `(6)` 真机 breadcrumb 为 onViewRenderJNI，说明 Created 和
Changed 已返回，Render/swap 未验收；crash 文件只有 status=armed。

前一轮异常已确认：RR3 PC +0x1c69ab8 执行 ldr x8,[x23,#0x28]，前一条
mrs x23,tpidr_el0，X23=0、FAR=0x28、ESR=0x92000006。guestPthreadCreate
直接把 guest 入口交给 host pthread，没有子线程 TLS，这是本次已确认的缺陷。

最新 NRO 为 pthread-tls-v1：host trampoline 在每个 worker 上安装独立
GuestTlsScope，保持至 pthread_exit 的 TSD 析构完成；创建失败释放 context。
已增加正常/显式退出与 TSD TLS 自测，并把 breadcrumb enable 改为 host thread_local。
保留 exception-v1/shader-v1/callers-v1。构建、9/9 契约、10/10 哈希及七卷验证通过。
最新包 SHA256：
83E3B615EF7CECB5EC59A9EC12F073F49D4D483670935AF32CF6CD92FE201CC3

用户已确认 `(6)` 轮次是黑屏更久后报错退出，crash 仍为 armed。
下一步调查 Render 后主动终止或异常采集盲区；不要把最后 breadcrumb 当作故障 PC。
获取完整报告确认 pthread 自测 0x3ff、Render returned/swap；若发生新异常，
使用同轮 crash/progress/pending 定位，不能继续套用旧 vector/string/shader 假设。

保留 recovery guard 和 UI/EGL 所有权隔离，异常捕获后终止进程，不恢复 guest。
不要盲加 malloc/operator-new shim，不跳过 guest 失败分支，不绕过账号、DRM
或服务端授权。用户允许直接重写不合适的兼容/诊断实现，以尽快解决实际阻塞。
每轮运行九项本地契约测试、构建并校验产物，交付真机步骤；现场先备份，
重启到 Recovery 且无初始化活动后才清除旧 marker。

P0 来源识别、P1a 异常捕获已完成；P1b worker TLS 修复已交付并观察到推进，
当前重点为 P2 的 Render/swap 及线程自测验收，后续再进入持久帧循环。
之后依次完成 P2 View 生命周期、P3 持久帧循环、P4 手柄、P5 音频、P6 离线比赛。
最终验收是真实 Switch 手柄驾驶并完成一场离线单人比赛。
如果等待硬件证据，准确报告等待项，不把本地测试通过说成游戏已可玩。
```
