# RR3 Switch 移植 — 项目状态总结

**更新时间**: 2026-09-28  
**最新 NRO SHA-256**: `9F3800FE9D35E9D47C0B62EBBB79149FC848C359C4C48FE91447228AE94B7914`  
**NRO 大小**: 51,757,557 bytes  
**契约测试**: 11/11 PASS

---

## 一、项目目标

将 Android ARM64 版 Real Racing 3（无源码 APK）移植到 Nintendo Switch Horizon OS，作为原生 libnx NRO 运行。

**成功标准**: 启动 → 进入菜单 → 选择离线比赛 → Joy-Con 驾驶 → 完成比赛 → 安全退出

**约束**:
- 不绕过 DRM、不伪造账户、不公开分发 EA 数据
- 仅离线/单人模式
- 只替换 TF 卡上的 NRO，七个数据卷不变
- 用户通过连接 Switch 复制文件 — **最小化真机测试轮次**
- 用户偏好逆向分析而非反复真机测试

---

## 二、当前状态 (2026-09-28)

### 本日修复：真机 mapProcessCodeImages 失败（策略回摆到已验证 one-pass）

**现场**（guest-view 12）:
```
mapProcCode: real hw (segmented) ordinal=208
loader-early2: mapProcessCodeImages failed ordinal=203
startupPlan: active=0 imgCount=0 lifeCount=4
```

**根因**（对照 TEST_V15/V16/V17 真机证据）:
1. 当前 real HW 被 `tools/fix_*.py` 切到 `mapSegmentedImages`，其中 R/RW 走 `mapNormalSegmentedRange` + `svcMapMemory` 打进 CodeMemory VA — **V15 已证伪**（stage 14 / 0xdc01 InvalidMemoryRange）
2. `enforceSegmentPermissions` 整段 `Perm_None` 重置 — **V16 已证伪**（0xd401；AliasCode 初始即 None）
3. 失败日志不打印 stage/Result，诊断盲区

**修复**（`native-switch/source/multi_dso_linker.cpp`）:
- 真机恢复 **one-pass `svcMapProcessCodeMemory`**（TEST_V16 成功路径）
- 时序改为：reserve → **重定位到 image.data()** → map 最终重定位字节 → LOAD/RELRO 权限（无 whole-image None）
- `writableLinkBase=nullptr`，`executableLinkBase=targetBase`（loadBias 运行时正确）
- 调用 `enforceSegmentPermissions`（仅 21-style LOAD + RELRO；4 DSO 实测 11 次权限操作）
- 失败日志带 `stage/img/res/fails`；`permEnf` 打印 ranges/verified/failures

**闪退回归（本日第二轮）**: 映射已成功（`svcMapProcessCodeMemory ok ×4`），但 `permEnf: ranges=14 verified=11 fail=3` 后闪退。  
原因：按 phdr 逐段设权限（14 ops，3 个 RELRO 与 LOAD 重复设同一 range 失败）；且权限改完后对整镜像 `armICacheInvalidate`，打到剩余 None/空洞页导致异常（此前无 crash 文件，因 capture 尚未 arm）。

**二轮修复**:
- 权限改为与 `test_loader_memory_model` 相同的**页合并模型**（LOAD 涂页 + RELRO 覆盖为 R → 合并 run，预期 **11 ops**，无 None→None）
- 缓存维护移到权限**之前**（整段 AliasCode 仍为 None 时 invalidate）；去掉权限后全镜像 icache invalidate
- `permOp:` 失败日志带 va/size/perm/Result；`cache-flush-done` / `perm-enforce-done` 面包屑

**验收日志（真机应出现）**:
```
mapProcCode: real hw (one-pass)
mapProcCode: svcMapProcessCodeMemory ok (×4)
cache-flush-done
permEnf: ranges=11 verified=11 fail=0 enforced=1
perm-enforce-done
loader-diag: ... permEnf=1 ready=1
startupPlan: active=1 imgCount=4 lifeCount=4
```
若仍有 `permOp: set/verify fail`，日志带 va/size/perm/Result。不要再在权限后对整镜像做 icache invalidate。

### Eden 模拟器验证通过的完整启动路径

| 阶段 | 状态 | 证据 |
|------|------|------|
| 4-DSO 加载器 | ✅ | `loader-diag: missRel=0 needMiss=0 wxSeg=0 tlsSeg=0 permEnf=1 ready=1` |
| 启动计划 | ✅ | `startupPlan: active=1 imgCount=4 lifeCount=4` |
| 自动启动 | ✅ | `auto-boot: extData=1 fsTest=fff loader=1 plan=1` |
| Stage 3 构造函数 | ✅ | `initDeps-stage3` → `initDeps-tls-scope` → `initDeps-completing` |
| JNI_OnLoad | ✅ | 缓存 JavaVM 指针 |
| 181 个主构造函数 | ✅ | `initializeGuestMainConstructors` 完成 |
| onCreate/onStart/onResume | ✅ | 进度文件 `phase=onResumeJNI` |
| 21 次 JNI 调用 | ✅ | getClassLoader/loadClass/getOsVersion/getDeviceUID 等 |
| 资源路径请求 | ✅ | 11 个 path-req + 3 个 fopen-other |
| Guest View 导出解析 | ✅ | `view-resolve-ok` (Created/Changed/Render 全部解析) |
| Guest View EGL | ❌ | 挂死在 `eglGetDisplay` |

### 当前卡点：Guest View `eglInitialize` 自旋（**旧文档写的是 `eglGetDisplay` 挂死，是错的**）

**更正（2026-10-02 实测）**：旧版本此处写"`eglGetDisplay` 后无更多日志，Eden CPU 持续增长"。
`eglGetDisplay` 实际上**毫秒级返回有效 EGLDisplay**。加面包屑边界后定位到真正的点是
**`eglInitialize` 自旋**。

**证据**（`.build/eden-runs/20261002-154634-cpu-series`）:
```
view-egl-probe-begin
egl-nodisplay: vendor=(null) version=(null) apis=(null) ext_len=191
egl-proc: platformDisplayEXT=0x80fe5ec0 platformDisplay=0x80fe5a70
view-egl-probe-end
view-egl-get-display
view-egl-get-display-ret      <-- eglGetDisplay 正常返回
view-egl-init-begin           <-- 之后无日志，挂在这里
```
CPU 序列 `10.25 / 15.22 / 20.25 / 25.20`（每 10 秒 +5.0 秒 = 恒定半个核）
⇒ **自旋，不是死锁**。

**为什么 Eden 上必然失败（三条独立证据）**:
1. `eden.exe` 无 `/dev/dri` / `drmOpen` / `gbm_create` ⇒ Eden 不模拟 DRM 设备；
   devkitPro `libEGL.a` 只有 nouveau 后端（无 llvmpipe/swrast/softpipe）。
2. 运行时扩展串只有 `EGL_EXT_device_base / EGL_EXT_device_enumeration /
   EGL_EXT_device_query / EGL_EXT_platform_base / EGL_EXT_platform_device /
   EGL_KHR_client_get_all_proc_addresses / EGL_EXT_client_extensions / EGL_KHR_debug`
   ⇒ **只有 DRM device 平台**，没有 surfaceless、没有 gbm、没有软件平台。
3. `eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)` 正常返回 ⇒ Mesa 本身活着，
   问题在打开显示设备那一步。

⇒ **在 Eden 上不可能拿到可用的 EGL**。这是平台事实，不是待优化项。
真机上 Tegra X1 由 nouveau 驱动支持，`eglInitialize` 是否可用**仍未验证**。

**因此**：客机的 EGL/GLES 不能再路由到宿主 Mesa（`symbol_resolver.cpp:192` 起 3 个 EGL +
142 个 GLES 绑定）。已派工实现自研 null-GLES 记录后端（`RR3_GFX=null` 切换），
默认路径保持 Mesa 不动。

### 上游回归（2026-10-02 已修）

9/28 的 `mapProcessCodeImages` 重构在模拟器路径上删掉了短路，导致 Eden 上
`permEnf=0` → `activationReady=0` → `auto-boot loader=0` → 依赖初始化与 Guest View
全被跳过。已在 `multi_dso_linker.cpp` 的 `enforceSegmentPermissions` 补回。

另有一个更深的加载器缺陷：**重定位基址与运行时链接基址不一致**。
`reserveSegmentedTargets` 的 `virtmemFindCodeMemory` 预留地址在 Eden 上是每次运行都变的
宿主地址，`applyLoaderTable` 以它为基址算完所有 `R_AARCH64_RELATIVE` 之后，
`mapProcessCodeImages` 却丢弃该基址而不重算。已由 `rebaseImagesToRuntimeBase()` 修复，
实测 `init0 == newBias + 0x98E620` 精确吻合，181 个主构造函数跑完。

---

## 三、关键技术发现

### DSO 加载器模拟器兼容性 (关键)

```
svcMapProcessCodeMemory → 0xe401 (InvalidMemoryState) on Eden
svcSetMemoryPermission → 0xd801 on Eden
envGetOwnProcessHandle() → INVALID_HANDLE on Eden
```

**修复**: 跳过 `svcMapProcessCodeMemory`，直接使用 `memalign` 背衬内存作为目标。必须设置:
- `gSegmentedSessionActive = true`
- `gLoaderResolutionPlanHash = 1`
- `result->segmentPermissionsEnforced = true`
- `image.writableLinkBase = processBacking`
- `image.executableLinkBase = processBacking`

### 依赖初始化流程

```
runGuardedDependencyInitialization()
  → 检查 recoveryMode (为 true 则阻塞)
  → 检查 externalData.ready && filesystemTest == 0xfff
  → 检查 loader.activationReady && startupPlan.available
  → 调用 initializeGuestDependencies()
      → Stage 1: preflight (检查 gSegmentedSessionActive, gLifecycleOrderCount, gLoaderResolutionPlanHash)
      → Stage 2: lifecycle verification
      → Stage 3: GuestTlsScope + 构造函数执行
      → Stage 4: 完成
```

### Guest View 启动流程 (第二次 runGuardedGuestBootProbe)

```
runGuardedGuestBootProbe() 第二次调用
  → 检查 entryProbe.resumeReturned && !viewProbe.attempted
  → prepareGuestCrashCapture()
  → nativeUiExit() (关闭帧缓冲 UI)
  → nwindowGetDefault()
  → runGuestViewProbe(window, progressPath)
      → resolveMainImageExport (Created/Changed/Render) ✅
      → eglGetDisplay(EGL_DEFAULT_DISPLAY) ← 挂死
      → eglInitialize / eglChooseConfig / eglCreateWindowSurface
      → eglCreateContext / eglMakeCurrent
      → GuestTlsScope
      → onViewCreatedJNI / onViewChangedJNI / onViewRenderJNI
      → eglSwapBuffers
```

### 资源路径请求 (onResume 后)

```
path-req: /storage/emulated/0/Android/data/com.ea.games.r3_row/files/.depot/langs.utf8
path-req: files/.depot/gui_assets/2048x1536/test
path-req: files/.depot/gui_assets/960x640/test
path-req: Tracks/Menu/processed/menu_transition.banim
path-req: vehicles/2013_bentley_cgtspeed/a
path-req: vehicles/2018_porsche_911_gt2_rs/a
path-req: vehicles/2024_toyota_yaris_wrc/a
path-req: files/.depot/../secret
path-req: .depot/text_cn-Hans.txt
path-req: sdmc:/switch/rr3-native-rehost/.depot
path-req: sdmc:/switch/rr3-native-rehost/apk
fopen-other: sdmc:/switch/rr3-native-rehost/.nomedia
fopen-other: sdmc:/switch/rr3-native-rehost/doc/telemetry_optin.bin
fopen-other: sdmc:/switch/rr3-native-rehost/doc/telemetry_opt_in.bin
```

### .depot 资产加密 (ChaCha20)

- 算法: ChaCha20-256 + 96-bit nonce ("expand 32-byte k" 常量在 libRealRacing3.so 0x1B567A0)
- `.vsh` 文件: 4 字节明文 magic 在 offset 2 ("1B:D")
- `.gui` 文件: 21 字节明文头部 (gui/ 子树用固定 nonce)
- 密钥: 未恢复，需从 0x1B567A0 附近反汇编提取
- 工具: `tools/decode_rr3_depot.py` (romfs/chacha20/gui-headers/keystream 子命令)

### 地址映射

| 符号 | ELF VA | 从 onViewRenderJNI 的 delta |
|------|--------|---------------------------|
| onViewRenderJNI | 0x12B3614 | 0 |
| cloudcell GOT | 0x1f6d5d0 | +0xCB9FBC |
| 菜单加载器全局 | 0x1f6df50 | +0xCBA93C |
| FUN_01175720 (XML loader) | 0x1175720 | -0x13DEF4 |
| CloudcellResume | 0x1cf2ad8 | +0xA3F4C4 |
| hideSplash 检查 | 0x12aeee0 | -0x4734 |

### 主渲染路径 (0x12aefc8)

```
ldr x8, [0x1f6d000+1488]  // cloudcell GOT
ldr x0, [x8]              // cloudcell 组件
ldrb w8, [x0, #1217]      // ready 标志
cbz w8, skip              // 为 0 则跳过菜单加载
ldr x8, [x0]              // vtable
ldr x8, [x8, #48]         // vtable+48
blr x8                    // 调用 (必须保留 x0)
ldr x8, [x0]              // vtable
ldr x8, [x8, #136]        // vtable+136
blr x8                    // 调用 (返回 0 = 继续)
tbnz w0, #0, skip         // 返回非零则跳过
ldr x0, [x23]             // 菜单加载器 (x23 = *(0x1f6df50))
mov w1, #2
mov w2, #0x60
ldr x8, [x0]              // vtable
ldr x8, [x8, #168]        // vtable+168
blr x8                    // 菜单加载调用
```

### FSM 状态机

- `activity+0x4d8` (frameState): 1/2/3=warmup, 4=GL reset, 5=splash, 其他=主渲染
- `activity+0x4dc` (splashFrames): onResume 设 1，6 帧后调 hideSplash 并重置
- `engine+0xa`: render early-outs / empty update while 0
- `container+0xa`: menu draw
- `container+0x20`: ready
- `renderer+0x1f5a9`: splash→menu 触发位

---

## 四、数据卷状态

| 卷 | 大小 | 内容 |
|----|------|------|
| core.romfs | 429 MB | Shaders, gui, eds |
| gui-hd.romfs | 1.66 GB | 2048x1536 纹理 |
| gui-sd.romfs | 501 MB | 480x320/960x640 纹理 |
| tracks.romfs | 1.91 GB | 赛道 |
| vehicles-classic.romfs | 1.86 GB | 经典车辆 |
| vehicles-mid.romfs | 1.79 GB | 中期车辆 |
| vehicles-modern.romfs | 1.47 GB | 现代车辆 |

源 `.depot`: 41,019 文件, 8.92 GB  
数据卷总计: 8.97 GB (romfs 开销 1.01x)

`libRealRacing3.so` SHA-256: `57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051`

---

## 五、JNI Stub 覆盖

92 个方法 stub，关键的：

| 方法 | 返回值 | 备注 |
|------|--------|------|
| getComponent | dummy object | |
| isDataAvailable | true | false 会导致 hideSplash 不被调用 |
| isNetworkAvailable | false | |
| getCloudcellEnvironment | "switch" | |
| hideSplash/showSplash | no-op + 日志 | |
| loadTextureFromMemory | PNG 解码 + 占位符 | |
| connect() | ECONNREFUSED | 阻止网络阻塞 |

---

## 六、下一步方向

### 优先级 1：把客机图形栈从 Mesa 切到自研后端（进行中）

Eden 上 Mesa 必然失败（见上）。已开始实现 null-GLES 记录后端，通过 `RR3_GFX` 环境变量
选择，默认仍走 Mesa（真机路径不变）。落地后客机可在 Eden 上跑完整个帧循环，并借此
枚举真实 GL 负载、捕获 GLSL ES 源码（deko3d 只吃预编译 blob，这是后续 PC 端
GLSL→SPIR-V 烘焙的前提）。

**真机验证（仍未做）**: 一次真机运行即可决定 Mesa+nouveau 路线是否成立。
若真机上 `eglInitialize` 同样失败，则 deko3d 路线是唯一解。

### 优先级 2：菜单加载 (Guest View 成功后)

1. 验证 cloudcell byte 1217 正确设置
2. 验证菜单加载器 vtable+168 被调用
3. 检查 `ESportsMainMenu.xml` 是否通过 FUN_01175720 加载

### 优先级 3：.depot 资产解密

1. 用 Ghidra 跟踪 `0x1B567A0` 附近汇编，找到 ChaCha20 key 指针
2. 以 MD5(文件) 或 0 作 nonce 解 2dflat.vsh 验证
3. 用同样 key（不同 nonce）解 LoadingScreen_Init.gui

### 优先级 4：Joy-Con 输入与比赛

1. 验证 ControllerManager JNI pump
2. 验证离线比赛选择
3. 验证驾驶控制
4. 验证完成比赛与安全退出

---

## 七、构建与测试流程

```bash
# 构建
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1"

# 测试 (11 个契约测试)
cd native-switch/tests && python test_*.py

# Eden 模拟器测试
# 1. 杀掉 Eden
# 2. 删除恢复标记: E:\Eden\user\sdmc\switch\rr3-native-rehost.guarded-action.pending
# 3. 删除旧日志: E:\Eden\user\sdmc\switch\rr3-native-rehost.guest-view.log
# 4. 复制 NRO: E:\Eden\user\sdmc\switch\rr3-native-rehost\rr3-native-rehost.nro
# 5. 启动: E:\Eden\eden.exe E:\Eden\user\sdmc\switch\rr3-native-rehost\rr3-native-rehost.nro
# 6. 等 60 秒，检查 guest-view.log
```

**真机测试**: 启动 → 自动执行全部阶段 → 复制 `sdmc:/switch/rr3-native-rehost.guest-view.log`

---

## 八、关键文件

| 文件 | 作用 |
|------|------|
| `native-switch/source/multi_dso_linker.cpp` | 4-DSO 加载器、mapProcessCodeImages 模拟器回退 |
| `native-switch/source/guest_view_probe.cpp` | View 探针、EGL 初始化、FSM 强制 |
| `native-switch/source/jni_bridge.cpp` | JNI stub 层 |
| `native-switch/source/guest_filesystem.cpp` | 路径重映射 |
| `native-switch/source/symbol_resolver.cpp` | 符号绑定、connect stub |
| `native-switch/source/main.cpp` | 自动启动、诊断日志 |
| `native-switch/source/tls_probe.cpp` | GuestTlsScope |
| `tools/decode_rr3_depot.py` | .depot 解密工具 (ChaCha20) |
| `reverse/native/arm64/libRealRacing3.so` | guest DSO |
| `reverse/native/decomp-calltree/` | Ghidra 反编译 |

---

## 九、诊断日志标签

| 标签 | 含义 |
|------|------|
| `mapProcCode: emulator fallback` | 模拟器跳过 svcMapProcessCodeMemory |
| `loader-diag: missRel=...` | 加载器重定位诊断 |
| `startupPlan: active=...` | 启动计划状态 |
| `auto-boot: extData=...` | 自动启动条件 |
| `initDeps-start/preflight-ok/stage3/tls-scope/completing` | 依赖初始化阶段 |
| `guest-boot-start/returned` | Guest 启动开始/返回 |
| `calling-guest-view` | 第二次调用 runGuardedGuestBootProbe |
| `view-branch-enter` | 进入 Guest View 分支 |
| `view-probe-enter` | 进入 runGuestViewProbe |
| `view-resolve-created/changed/render/ok` | 导出解析 |
| `view-egl-get-display/init-ok/config-ok/surface-ok/context-ok/current-ok` | EGL 初始化 |
| `view-tls-scope-enter/ok` | GuestTlsScope |
| `view-call-created/changed/render` | View 生命周期调用 |
| `view-created-ret/changed-ret/render-ret` | View 生命周期返回 |
| `jni-obj/static-obj/void` | JNI 方法调用 |
| `path-req` | 资源路径请求 |
| `fopen-other` | 文件打开 |

---

## 十、GitHub 发布

- **仓库**: https://github.com/ArakiW/rr3-switch
- **最新 Release**: https://github.com/ArakiW/rr3-switch/releases/tag/v0.22.4
- **文件**: `rr3-native-rehost.nro`, `rr3-switch-v0.22.4.zip`
