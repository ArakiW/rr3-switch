# Horizon OS 原生逆向移植状态

## 已完成的反编译

### Java / DEX

- 工具：JADX 1.5.6，项目私有安装在 `.tools/jadx-1.5.6`；
- 输入：Real Racing 3 14.0.1 APK；
- 处理类数：15,955；
- 输出 Java 文件：24,649；
- `com.firemint` 游戏平台层文件：45；
- JADX 报告 78 个方法级反编译错误，绝大部分输出仍可使用；
- 关键入口已恢复：`UnpackAssetsActivity`、`MainActivity`、`GLView`、`GLRenderer`、`ControllerManager`、`Input`、`Http`、`Platform`。

输出目录：`reverse/java/sources`。

### ARM64 游戏核心

- 已从 APK 提取全部 14 个 `arm64-v8a` 动态库到 `reverse/native/arm64`；
- 主库 `libRealRacing3.so` 为 33,110,472 字节的 AArch64 ELF；
- 工具：Ghidra 12.1.3；
- Ghidra 全库自动分析已成功完成并保存为 `reverse/ghidra-projects/RR3`；
- 已导出 77 个 Firemint/Firemonkeys JNI 函数，77/77 伪 C 成功；
- 生命周期、渲染与输入根节点的二层调用树共 222 条，168 个唯一函数，168/168 伪 C 成功；
- 自动分析对部分 GCC 异常元数据、外部 RTTI 表和嵌入数据产生警告，但目标 JNI 函数没有反编译失败。

主要输出：

- `reverse/native/decomp-jni/index.tsv`：JNI 导出索引；
- `reverse/native/decomp-jni/*.c`：77 个入口的伪 C；
- `reverse/native/decomp-calltree/index.tsv`：根节点、深度、地址和状态；
- `reverse/native/decomp-calltree/*.c`：168 个唯一函数的伪 C；
- `reverse/ghidra_scripts/*.java`：可重复执行的 Ghidra 导出脚本。

Ghidra 的 C-like 输出是分析材料，不是可以直接重新编译的原始 C++ 源码。类型、类布局、虚表、异常语义和函数名仍需持续恢复。

## 已确认的原生架构

`MainActivity` 依次加载 `libNimble.so`、`libfmodex.so` 和 `libRealRacing3.so`。游戏主库的关键稳定导出包括：

- `onCreateJNI`：分配一个 `0x540` 字节的平台 Activity 对象并进入构造函数；
- `onViewCreatedJNI`：初始化/重建 GL 对象和游戏子系统；
- `onViewChangedJNI`：更新渲染尺寸；
- `onViewRenderJNI`：执行每帧更新、渲染与帧率控制；
- `onStart/onResume/onPause/onStop/onDestroy`：生命周期；
- `ControllerConnectedJNI`、`SetButtonValueJNI`、`SetJoystickValueJNI`：手柄；
- `onKeyPressed/onKeyReleased`：按键；
- 触控和传感器接口可在 Switch 端按需映射。

主库有 706 个未解析动态导入，当前重要分类：

| 类别 | 数量 | 原生迁移策略 |
|---|---:|---|
| OpenGL ES | 142 | 优先映射到 switch-mesa/OpenGL；保留运行时 GLSL |
| FMOD | 54 | 需要合法 Switch FMOD 库或自建兼容音频层 |
| pthread | 33 | 映射到 libnx/newlib 线程与同步原语 |
| ARCore | 19 | Switch 首版禁用/桩实现 |
| Socket | 16 | 映射到 libnx socket 服务 |
| Android 日志 | 2 | 映射到 nxlink/文件日志 |
| C++ ABI | 245 | 优先复用同包 `libc++_shared.so`，并补 Bionic/TLS 兼容 |

JNI 调用大多通过 `JNIEnv` 函数表间接完成，因此没有表现为普通 ELF 未解析符号。需要按反编译调用点实现最小 JNI 函数表，而不是嵌入 Android Runtime。

## 原生实现路线

目标不是把 APK 改成 `.nro`，而是 **AArch64 ELF 静态重托管**：保留游戏核心机器码，在 libnx 程序内实现加载器与平台兼容层。

### 1. ELF 重定位加载器

- 映射 `PT_LOAD` 段并设置读/写/执行权限；
- 处理 AArch64 RELA、GOT/PLT、构造器和析构器；
- 加载主库、`libc++_shared.so` 与必要依赖；
- 为每个未解析符号建立显式 shim，不允许悄悄解析到错误 ABI。

### 2. Bionic / TLS / C++ ABI

这是当前最高风险项。反编译显示代码直接读取 `tpidr_el0 + 0x28` 作为栈保护/TLS 数据；Horizon/libnx 的线程控制块布局不同。需要验证是否能提供兼容 TCB，或对这些访问点做受控重写。仅仅因为两边都是 ARM64，二进制仍不能直接执行。

### 3. 最小 JNI 环境

先实现启动必需的 `JNIEnv` 和对象：字符串、字节数组、类/方法查找、字段访问、异常状态和少量 Activity/Platform 回调。广告、支付、ARCore、Firebase 与社交服务在离线启动阶段全部显式返回“不支持”，不做鉴权绕过。

### 4. 图形

游戏使用 EGL + OpenGL ES，并在运行时调用 shader API。首版用 switch-mesa 提供标准 GL 兼容面；deko3d 的 shader 流程偏向构建期编译，直接接管动态 GLSL 需要额外 shader 转译器，因此放到性能优化阶段。

### 5. 输入与音频

- libnx Npad/Joy-Con -> 已恢复的 Controller JNI 接口；
- 摇杆、扳机、A/B/X/Y/L/R/Plus/Minus 建立可配置映射；
- 音频先确认 FMOD ABI与许可可用性，否则实现 FMOD 调用子集或替换后端。

### 6. 数据与离线启动

先用 DXT 数据包建立只读 depot，完成开屏、主菜单和一场离线比赛。账号、购买、在线多人和服务器鉴权不在兼容性逆向范围内。

## 当前原生工程

`native-switch` 是纯 libnx Horizon homebrew 工程，不包含 Android 运行时。当前提供：

- Npad/Joy-Con 输入循环；
- `tpidr_el0`、`tpidrro_el0`、栈和内存映射 TLS 探针；
- libnx JIT 可执行内存创建、权限切换与一条 AArch64 `ret` 指令执行探针；
- 内嵌 `libRealRacing3.so` 的单文件 RomFS NRO，避免外部 SO 路径失败；
- ELF64/AArch64/ET_DYN、LOAD/DYNAMIC/TLS 段和重定位数量预检；
- 不执行构造器的 PT_LOAD 映像复制、BSS 清零和 RELATIVE 重定位干运行；
- 逆向 JNI ABI 契约。

项目私有工具链已安装：devkitA64 r30 / GCC 16.1.0、libnx 4.12.0、switch-tools 1.13.1。`rr3-native-rehost.nro` 已真实编译成功且无项目警告。它还没有执行游戏核心，所以不能称为可运行游戏移植；JIT 基础调用已获真机证明，TLS 兼容结论和 ELF 干加载仍需继续验证。

### 真机证据

Probe v1 已在真实 Switch 上启动并干净运行到等待退出：JIT 创建、切换到可执行、调用一条 AArch64 `ret`、切回可写和关闭均返回成功。首次截图同时暴露两个部署/显示问题：外部 SO 路径未命中，且摇杆状态覆盖了 TLS 输出行。

Probe v2 已修复行覆盖，并把 33,110,472 字节主库封装进 33,111,084 字节 RomFS。真机结果确认 RomFS 打开、PT_LOAD 复制、BSS 清零和 142,661 个 RELATIVE 重定位全部通过；`pending=9439`、`invalid=0`、预重定位哈希与主机参考完全一致。

Probe v3 进一步解析动态符号和版本契约：真机结果与主机参考完全一致，包括 706 个唯一外部符号、34 个内部符号重定位、9,405 个外部重定位、22 个 weak 符号、284 个带版本符号、最大索引 830、名称哈希及全部分类计数。v3 仍不写入外部地址，也不执行构造器。

Probe v4-v6 已依次在真机验证 121 项普通 C/newlib/libm 地址解析、13 项 Bionic fortify/log/errno 兼容层，以及覆盖全部 33 个导入名称的 guest-object pthread bridge。Bionic 与 pthread 自检均为 `3ff`，解析表累计 167 项；这些结果仍属于查找和独立自检，不代表已经向游戏映像写入外部地址。

Probe v7 已在真机验证内嵌的匹配版本 `libc++_shared.so`：1,428 个 RELATIVE 重定位、2,283 个内部符号重定位、160 个唯一外部符号及全部哈希均与主机参考一致。

Probe v8 将 libc++ 外部解析提升到 154/160 个名称、155/162 个重定位，解析表累计 259 项，并新增独立的 libc++/Bionic POSIX 包装自检。保留的 6 个缺口不是普通函数缺失：`__cxa_atexit`、`__cxa_finalize`、`__cxa_thread_atexit_impl` 需要 guest DSO/线程析构生命周期；`__sF` 需要 Bionic `FILE` 对象桥；`dl_iterate_phdr` 需要常驻多 DSO 映射；`syscall` 需要按 Android 调用号逐项翻译。v8 构建已通过，等待真机验证。

Probe v9 将这些边界一次性收口并合并成集中测试版：解析表 265 项，libc++ 达到 160/160 名称和 162/162 重定位；`__cxa_*` 使用 guest DSO/线程析构登记表；`dl_iterate_phdr` 枚举同时保留的主库与 libc++ 映像；反汇编确认 libc++ 的 `syscall` 只调用 AArch64 `futex(98)` 与 `gettid(178)`，分别翻译到 Horizon 地址仲裁与线程 ID；`__sF` 按 Android ARM64 `FILE=0x98` 字节布局提供三个 guest 标准流对象，并在 `fprintf`/`fwrite`/`fflush` 等边界翻译到 newlib 标准流。

v9 同时新增 33,572 KiB JIT 首尾 `ret` 探针，以及双 DSO 常驻堆映像交叉链接器。主机参考结果为：144,089 个 RELATIVE、2,317 个内部重定位、8,842 个主库到 libc++ 的交叉重定位；累计写入并回读 155,658 个槽位，`invalid=0`。主库和 libc++ 的 182 个构造器及 4 个终结器目标全部完成地址验证，但仍不执行构造器。

v9 已在真实 Switch 集中验证通过：Bionic/pthread/libc++/C++ lifecycle/phdr/syscall/stdio 七组自检全部命中预期；33,572 KiB JIT 成功创建并执行首尾两条受控 `ret`；libc++ 为 160/160、162/162；双 DSO 映像保留成功，155,658 次写入全部回读一致；构造器/终结器目标 186/186；确定性计划哈希 `ecee08eba8dfe616` 与主机参考一致。

Probe v10 将加载范围扩展为实际启动依赖的 7 个 DSO：主库、Nimble、FMOD、ARCore C/JNI、libc++ 与 fuelmetrics。加载会话解析 40 条 `DT_NEEDED`（8 条由内嵌 DSO 满足、32 条由 Horizon 兼容层虚拟提供），建立全局导出作用域和 guest `dlopen/dlsym/dladdr/dlclose/dlerror`，创建 39,680 KiB 可执行别名，处理 174,187 个可写重定位槽，并生成依赖优先的 365 项构造器顺序与逆序 13 项终结器计划。7 个 DSO 均没有 `PT_TLS`，6 个 GNU RELRO 范围覆盖 166,249 个重定位目标。

v10 对 Horizon 分段权限采用能力门禁：如果 hbloader 同时暴露 SVC 0x73 和有效的 own-process handle，就先把 7 个完整别名重置为无权限 guard 区，再实施 21 个 LOAD 段和 6 个 RELRO 范围的 RX/RW/R 权限；否则明确报告 `perm=no`。无论权限能力结果如何，当前 333 个强外部重定位（其中主库 229 个）都会令 `activationReady=no`，因此不会执行任何 guest 构造器。加载器会话代码已完成，等待真机验证其多 JIT/权限路径。

v10 真机运行时，前置自检、完整 libc++、33,572 KiB JIT 与 rollback 自检全部通过；七 DSO 会话被内部 `loader relocation or lifecycle validation failed` 门禁拒绝，随后 phdr/dynamic-link 自检因会话已回滚而未通过。根因不是 ELF 数据损坏，而是加载器错误地要求 GNU RELRO 完整包含在单个 PT_LOAD 中；Android 的 RELRO 会向页边界扩展并合法跨越 LOAD 尾部 padding。v11 已改为对整个映射范围验证 RELRO，仍保持构造器零执行和失败回滚。

v11 真机确认修复有效：七 DSO 会话、174,187 次重定位写入/回读、40 条依赖、6 个 RELRO、378 个生命周期目标、phdr 与 guest 动态链接 API 全部通过，`invalid=0`。设备虽然暴露 SVC 0x73 与进程句柄，但 CodeMemory slave 的 34 次权限修改全部被拒绝，因此 `perm=no`、`ready=no`。v12 改用当前进程 SVC 0x02 作为回退：保持代码页原有 RX，只把非执行 LOAD 和 RELRO 页调整为 RW/R/None。

v12 的回退权限计划共 29 个范围：7 个可执行 LOAD 只查询并保持 RX，14 个非执行 LOAD 调整为 RW/R，2 个映像空洞设为 None，6 个 RELRO 最后覆盖为 R。只有 29/29 查询均与目标权限一致才设置 `perm=yes`；任何失败仍保持 `ready=no`，不执行构造器。

v12 真机返回 `7/29, fail=22, p73fail=34`：7 个原有 RX 代码范围查询正确，但 14 个非执行 LOAD、2 个 gap 和 6 个 RELRO 的 SVC 0x02 修改全部被 CodeMemory slave 拒绝。结论是不能原地改变 slave alias 的权限。下一阶段改为分段 CodeMemory 映射：先保留连续 guest 地址区间，再为合并后的 RX/R/RW 区间分别创建 CodeMemory 对象，guard 区保持未映射。

v13 实现原生分段映射。每个 DSO 先保留一个连续 guest 地址范围，所有重定位重新按最终 guest base 写入原始映像；随后按页合并为 21 个权限区间。RX/R 区间以独立 CodeMemory slave 映射到 guest 地址并保留私有 RW owner，RW 区间把 owner 直接映射到 guest 地址，gap 不建立映射，RELRO 在合并前覆盖为 R。任一 handle、owner、slave、权限查询或重定位复核失败都会逆序拆除全部区间和 reservation，并保持构造器门禁。

v13 真机显示第二遍最终基址重定位仍为 `174187/174187`、`invalid=0`、RELRO 6、生命周期 `378/378`，但 21 区间映射阶段失败并触发回滚。v14 在每个 MapSlave 前刷新 owner D-cache，并输出失败 image、region、stage、Result 与实际/目标权限，以区分 CreateCodeMemory、MapOwner、MapSlave、权限查询和内容一致性失败。

v14 精确定位为 image 3、region 10、stage 3：第 11 个 `svcCreateCodeMemory` 返回内核 Result `0x0001ce01`（module 1, description 231），此前 10 个区间已成功，说明分段布局与内容路径成立但触及对象/资源限制。v15 改用每 DSO 一个 `svcMapProcessCodeMemory`，共 7 个 ModuleCode 映射，再用 SVC 0x73 实施 7 个 guard reset、21 个 LOAD 和 6 个 RELRO 权限。若该路径不可用，mode 2 仅为 7 个 RX 区间创建 CodeMemory，对 R/RW 区间改用普通映射。

v15 最终显示的是 mode 2 回退错误：image 0、stage 14 的 `svcMapMemory` 返回 `0x0000dc01`（Kernel InvalidMemoryRange），确认普通映射不能使用 CodeMemory 目标区；它覆盖了更早的 mode 1 错误。v16 删除 mode 2，并记录 `svcMapProcessCodeMemory`/SVC 0x73 的原始 stage、Result，以及 backing/target 的 MemoryInfo type、permission、attribute。

v16 证明 `svcMapProcessCodeMemory` 已成功：backing 为 Heap `type=5, RW, attr=0`，target 为 AliasCode `type=8, None`。随后第一条权限操作把整块 target 从 None 再设为 None，内核以 `0x0000d401`（InvalidMemoryState）拒绝。v17 删除 7 条冗余 guard reset；AliasCode gap 天然保持 None，只执行 21 个 LOAD 权限与 6 个 RELRO 覆盖。

在停止逐版 TF 测试后，加载器按 nx-hbloader 的单次流程重构：直接保留 guest 地址、按最终 base 重定位 heap backing、一次 `svcMapProcessCodeMemory`，再仅设置真实 LOAD 与 RELRO 权限，不再先建整映像 JIT 后二次 remap。核心集合缩减为主库、Nimble、FMOD、libc++；ARCore 明确返回设备不支持，fuelmetrics 明确禁用。Mesa EGL/GLES 145 个入口、socket/newlib、semaphore、setjmp/longjmp、Bionic fortify/mmap/statfs/stdio/ctype 等已加入，主机参考达到 0 强缺口、0 DT_NEEDED 缺口、158,445/158,445 重定位、invalid 0。

交互层已改为四页原生 Loader Control Center：总览、运行时自检、DSO/重定位、诊断/下一步；支持 L/R/A 翻页、X 手动导出报告、PLUS 退出。正式 UI、Mesa 生命周期、构造器崩溃保护和恢复流程完成前，不再提供 TF 测试包。

Formal UI 0.18.0 真机确认四页 UI、导航、四核心 DSO 统计、零缺口重定位与哈希均正确。process-code mapping 使用合法 Heap RW backing 且目标初始未映射；前三个权限范围成功，第 4 个 RELRO 二次覆盖返回 `0x0000d401`。因此 0.18.1 不再先设 LOAD 再覆盖 RELRO，而是先合并每页最终权限，再提交 11 个互不重叠的 ModuleCode 范围，使每页只经历一次 AliasCode 权限转换。

Formal UI 0.18.1 真机完成加载器闭环：Core DSO、11/11 最终权限、4/4 process-code 映像、158,445/158,445 重定位、0 strong missing、0 invalid、phdr/dynamic-link、自检与恢复状态全部通过，Activation 显示 READY。加载器框架在当前四核心 DSO 范围内完成；Mesa 离屏探针尚未在所给照片中执行，构造器仍保持默认禁用。

0.18.1 的 Y 动作因固定子目录无法创建 recovery marker 而在 EGL 初始化前被阻止。Mesa 探针本身只执行 native host code，不应强制写卡；0.18.2 将 Y 改为完全只读，恢复标记只留给未来 guest 构造器动作，并移到稳定的 `sdmc:/switch/` 路径。

0.18.2 的只读 pbuffer Mesa 探针安全返回但未完成绘制。switch-mesa 官方路径使用 `nwindowGetDefault()` 的 window surface，而 dashboard 软件 Framebuffer 同时占用该 NWindow。0.18.3 的 Y 动作会先 `consoleExit` 释放 Framebuffer，在默认 NWindow 创建 GLES2 window/context、绘制并 swap 一帧，销毁 EGL 后重新 `consoleInit` 恢复 UI；Runtime 页同步显示 stage/mode/EGL/GL 错误。

0.18.3 真机按 Y 后进程立即终止并由 Horizon 显示通用软件错误，证明同一进程内的 console/EGL 热切换并不安全；该路径已经完全删除。0.18.4 把渲染器改为启动独占模式：在 `consoleInit` 之前短暂采样按住的 Y，若命中则在默认 NWindow 上完成 EGL/GLES 创建、单帧绘制、交换、解绑、销毁、`eglTerminate` 与 `eglReleaseThread`，随后才初始化软件 console。普通启动不会调用 EGL，运行期 Y 只显示重启提示。静态契约要求全程序仅保留最终退出的一次 `consoleExit`，且唯一的 `runWindowGraphicsProbe` 调用必须早于 `consoleInit`；0.18.4 已从零编译并通过 NRO/NACP、四个 AArch64 RomFS DSO、11 个最终权限范围和 158,445 次重定位参考检查，等待一次真机启动模式验证。

0.18.4 真机证明运行期 Y 已安全退化为提示，但 hbloader 交接应用时没有可靠保留启动按键状态，按住 Y 未进入渲染模式。0.18.5 删除启动按键采样，改用 libnx 的 `envSetNextLoad`：运行期按 Y 只把当前 NRO 路径和 `--rr3-renderer` 参数写入 hbloader 提供的内存缓冲区并正常退出；hbloader 随即在全新进程中重载同一 NRO。新进程通过 argv 识别渲染模式，在 `consoleInit` 之前运行 EGL，因而既不热切换显示后端，也不创建 TF 卡标记文件。

0.18.5 真机进入 Y 路径后仍由 Horizon 报告软件错误。复核发现它虽然在新进程中启动 EGL，但 EGL 销毁后仍在同一进程调用 `consoleInit`，保留了反向的 EGL-to-console 后端切换，而官方 GLES2 示例在 EGL 生命周期中从不初始化软件 console。0.18.6 改为三个隔离进程：console-only 控制面板调度 EGL-only 子进程；子进程按官方顺序执行 `eglGetDisplay -> eglInitialize -> eglChooseConfig -> eglCreateWindowSurface -> eglCreateContext -> eglMakeCurrent -> eglSwapBuffers -> teardown`，把位图标志、stage、EGL/GL 错误和版本编码进 hbloader argv 后直接退出；第三个 console-only 进程只负责显示结果。源码与链接顺序均有静态契约，保证 EGL 子进程在 `consoleInit` 之前无条件 return。

复制出的 Atmosphere 报告推翻了“新进程”假设：`envSetNextLoad` 只是让 hbloader 在同一 Application 进程中卸载并重载 NRO。0.18.3 的两个本体报告 Build ID 精确匹配 0.18.3，Result `0x2B59` 解码为 libnx `BadGfxDequeueBuffer`，确认热切换失败在 NWindow dequeue。0.18.5 后的最新报告则只有 `hbl` 模块，Result `0xD401` 为 Kernel `InvalidMemoryState`；0.18.5 Build ID 不在报告中，说明错误发生在 NRO 已返回之后。根因是四个 guest ProcessCode 映射被有意 retained，却没有在返回 hbloader 前逆序释放。0.18.7 新增事务式 `unloadLinkElfSet`：Y 和 PLUS 都必须先完成 4/4 `svcUnmapProcessCodeMemory`、释放四个 backing/reservation 并清空动态链接作用域；任何失败都留在 UI 并显示首个 Result，不再把脏内存状态交给 hbloader。

0.18.7 集中真机序列报告 passed：PLUS 在逆序卸载后干净返回 hbmenu，同一 NRO 可再次启动；Y 的卸载门禁与 renderer NRO阶段完成并回到 console-only 结果阶段，没有再次出现 `BadGfxDequeueBuffer` 或 `hbl 0xD401`。至此当前四 DSO 范围的加载器同时具备真机 map/link/permission 与 unmap/return 双向闭环，后续工作转入显式崩溃保护下的 guest provider/constructor 初始化。

0.19.0 将 184 个构造器拆成依赖 3 个与 RR3 主库 181 个：依赖顺序为 libc++(1) → Nimble(0) → FMOD(2)，零依赖环；对应依赖 finalizer 共 5 个。反汇编确认启用的三个目标只初始化 libc++ iostream/locale 与 FMOD 全局内存/对象状态并通过既有 `__cxa_atexit` 登记析构，不直接进入 JNI、Activity、ARCore 或 RR3 主入口。ZR 是唯一触发键：调用前用一次 `fsync` 写入计划哈希及四映像 runtime base/size 快照，先验证三个目标均位于 RX PT_LOAD，再逐个调用；成功后 marker 保留到退出。PLUS/Y 会先按逆序运行 5 个依赖 finalizer，再执行已通过的 4/4 ProcessCode unmap，最后清除 marker。任何预检、finalizer 或 unmap 失败都留在 UI；181 个主库构造器仍不可达。

0.19.0 真机按 ZR 后软件终止；随后正常启动准确进入 Recovery，Overview 显示前次 guarded action interrupted，ZR 被 marker 阻止，证明单写快照与防重复门禁生效，181 个 RR3 主构造器没有执行。当前本地 crash_reports 副本尚未包含这次新增报告，且 marker 基址快照尚未复制；在取得二者并把 PC 归属到 libc++ `0x94398` 或 FMOD `0x215FC/0x216B8` 前，不再扩大构造器范围。

新增的 `01788162015` 报告已经精确归因：0.19.0 Build ID 匹配；匿名 guest 映像范围大小对应 libc++；LR 为 `0x943DC`，来自唯一 libc++ 构造器 `0x94398`；PC 为 `DoIOSInit+0x28` (`0x92F4C`)。指令序列先 `mrs x23,tpidr_el0` 再 `ldr x8,[x23,#0x28]`，报告 X23=0、Fault Address=0，因此 FMOD 尚未执行，根因是 guest Android 栈保护 TLS 缺失。0.19.1 新增 `GuestTlsScope`：保存当前 TPIDR_EL0，安装对齐的 0x100-byte guest TLS，在 +0x28 写入稳定 canary，包裹所有构造器/finalizer 调用，返回后恢复原寄存器；独立自检覆盖安装、读回 canary 与恢复，预期位图 `f`。

为了缩短硬件往返，工程私有工具目录安装 Eden v0.2.1 Windows 便携模拟器。CLI 无需导入 keys 即识别并开始加载 homebrew NRO，完成 Vulkan/音频/时钟/字体/applet 服务初始化并建立 60 FPS 窗口，但该版本目前在模拟器 Applet SelfController 层退出，尚未进入可自动操作的 dashboard；GUI 的解密密钥安装提示被拒绝。模拟器只作为回归辅助，Horizon MemoryState、NWindow 与 TPIDR_EL0 仍以真机为准。

## 边界

本项目只做本地兼容性研究与平台移植。不会修改游戏签名、绕过 DRM、伪造账号/购买状态或规避服务器鉴权，也不发布 EA 的游戏数据和反编译源码。
