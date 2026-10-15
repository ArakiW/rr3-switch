# EapRules/realracing3-native-arm 分析 — 可借鉴方案

**分析时间**: 2026-09-24  
**仓库**: https://github.com/EapRules/realracing3-native-arm  
**版本**: v2.7.0 ROW (armeabi-v7a)  
**状态**: Playable (15-20 fps on RK3326)

---

## 一、架构对比

| 维度 | EapRules (Linux/ARM) | 我们 (Switch/Horizon) |
|------|---------------------|----------------------|
| 架构 | ARM32 (armeabi-v7a) + ARM64 hook | ARM64 (aarch64) |
| OS | Linux (glibc) | Horizon OS (libnx) |
| 窗口 | SDL2 | libnx/NWindow |
| GPU | Mali (libmali) | Tegra X1 (nouveau/Mesa) |
| 音频 | OpenSL/ALSA/PipeWire | Switch audio |
| 加载器 | bionic ELF loader | multi-DSO loader |
| JNI | 完整 fake JVM | 部分 stub |
| 版本 | v2.7.0 | 待确认 |

---

## 二、核心可移植模块 (直接复制)

### 1. JNI Class Registry + Fake JVM
**路径**: `jni/jni.cpp`, `jni/class_registry.cpp`, `jni/classes/*.cpp`

完整 fake JVM 实现，包括:
- Class registry (按 classpath 注册)
- ManagedMethod 表 (Java 方法分发)
- NativeMethod 表 (JNI 导出解析)
- String/Object/ClassLoader 基础类

**可直接复制**: 整个 `jni/` 目录

### 2. Cloudcell 兼容层 (解决菜单卡点!)
**路径**: `jni/classes/cloudcell_compat.cpp`, `cc_services.cpp`, `cc_http_request.cpp`, `cloudcell_defer.cpp`

**关键发现**:
- `get_network_connectivity()` 返回 0 (离线) → 走本地缓存路径
- `cc_http_request.cpp` 的 `post()` 回调 `errorCallback(ptr, 0)` → 优雅失败
- `cloudcell_defer.cpp` 异步回调队列 → 避免重入
- `cc_services.cpp` 注册所有 cloudcell 服务 facade (登录/广告/支付/推送)

**解决了我们的核心问题**: 菜单加载依赖 cloudcell 初始化，这个兼容层让它认为离线可用。

### 3. Asset Patch (解决下载冻结!)
**路径**: `src/rr3_asset_patch.cpp`

**关键发现**:
- Hook `AssetDownloadService::SkipAssetImpl` (符号 `_ZN20AssetDownloadService13SkipAssetImplEPKc`)
- 检查文件是否存在，不存在则跳过 (返回 1)
- 99.6% 的"缺失"字节是其他 GPU 的纹理变体 (Tegra/Adreno/PowerVR)
- 没有这个 hook，游戏会永久冻结在 "No WiFi" 对话框

**实现**:
```cpp
extern "C" int rr3_skip_asset_impl(void *self, const char *name) {
    // 检查文件是否存在，不存在返回 1 (跳过)
}
```

### 4. Control Scheme (Joy-Con 输入!)
**路径**: `src/rr3_control_scheme.cpp`

**关键发现**:
- `profile.dat` 是原始 124 字节 dump，无 header/校验/加密
- Offset 20 = ControlMethods_e (0-7)
- Offset 24 = flipped flag
- 默认值 7 = Wheel_Manual (唯一同时接受摇杆转向 + 玩家控制油门的方案)

**实现**: 写入 `profile.dat` 的 offset 20-23 为 `07 00 00 00`，offset 24 为 `00`

### 5. FMOD Audio Pump (解决静音!)
**路径**: `src/rr3_fmod_pump.cpp`

**关键发现**:
- FMOD Ex on Android 不拥有自己的播放线程
- Java 线程 `org.fmod.FMODAudioDevice` 调用 `fmodGetInfo`/`fmodProcess`
- 没有这个 pump，游戏完全静音 (FMOD 一直在等待被调用)

**导出符号**:
```cpp
jint fmodGetInfo(JNIEnv*, jobject, jint what);  // 0=sample rate, 1=buffer len, 2=num buffers, 3=1, 4=channels
jint fmodProcess(JNIEnv*, jobject, jobject directByteBuffer);  // 写入 PCM16
```

### 6. Bionic 符号表 (ARM64 直接可用!)
**路径**: `src/symtab_bionic.cpp`, `src/symtab_libm.cpp`, `src/symtab_pthread.cpp`, etc.

**关键发现**:
- Bionic atomic helpers 有非直观返回约定:
  - `__atomic_cmpxchg` 返回 0 = SUCCESS (反转!)
  - `__atomic_inc`/`__atomic_dec` 返回操作前的值
- 这些符号在 glibc 中不存在，必须手写

**可直接复制**: 整个 `src/symtab_*.cpp`

### 7. ARM64 Hook 代码 (直接可用!)
**路径**: `loader/so_util_arm64.cpp`

ARM64 trampoline 实现:
```cpp
void hook_address(so_module *mod, uintptr_t addr, uintptr_t dst) {
    // guest -> patch arena -> host (两步 trampoline)
}
```

### 8. Save File Patch (修复崩溃!)
**路径**: `src/rr3_savefile_patch.cpp`

**关键发现**:
- `SaveManager::CreateTempFileWithSaveGame` 对没有点的文件名崩溃
- `rfind(".", npos, 1)` 返回 npos → `substr` 抛出 `out_of_range`
- 补丁: 将 `bhi` 分支改为 `movhi` clamp

### 9. GL Vendor Stubs (防止 NULL 跳转!)
**路径**: `src/symtab_gl_stubs.cpp`

**关键发现**:
- 游戏通过 `eglGetProcAddress` 查询 36 个 vendor 扩展
- 不检查返回值 → NULL = 跳转到地址 0
- 必须提供 stub (即使只是 no-op)

### 10. Platform Alert Handler (解决加载条卡住!)
**路径**: `jni/classes/rr3_Platform.cpp`

**关键发现**:
- `Platform.showMessage` 显示对话框
- 正按钮调用 `alertMessageExecuteCallback(idPositive)`
- 负按钮调用 `Platform.exitApp()` (退出游戏!)
- 必须总是回答正按钮

---

## 三、需要适配的模块

### 1. EGL Shim
**路径**: `android/egl_shim.cpp`
**当前**: SDL2 window + SDL GLES2 context
**目标**: Mesa/libnx NWindow

需要替换:
- `eglGetDisplay` → Mesa `eglGetDisplay`
- `eglCreateWindowSurface` → Mesa + NWindow
- `eglMakeCurrent` → Mesa
- `eglSwapBuffers` → Mesa

### 2. Input Bridge
**路径**: `android/input_bridge.cpp`
**当前**: SDL controller → `KeyboardAndroid.NativeOnKey*` + `TouchSurfaceAndroid.NativeOnPointerEvent`
**目标**: Joy-Con → 同样的 JNI 导出

需要替换:
- SDL GameController API → libnx Pad API
- 按键映射保持不变 (AKEYCODE_*)

### 3. Audio
**路径**: `android/opensles.cpp`
**当前**: OpenSL ES → ALSA/PipeWire
**目标**: OpenSL ES → Switch audio

需要替换:
- OpenSL buffer queue → Switch audio 队列

### 4. File I/O
**路径**: `src/symtab_io.cpp`, `src/fix_path.h`
**当前**: Linux FS
**目标**: Horizon FS (libnx)

需要替换:
- `open`/`read`/`write` → libnx FS
- 路径映射保持不变

---

## 四、集成计划

### 阶段 1: 移植 JNI 层 (最高优先级)
1. 复制 `jni/` 目录到 `native-switch/source/jni/`
2. 适配 `jni.h` 到 libnx JNI (如果有)
3. 注册所有 cloudcell 类
4. 测试: 验证菜单加载

### 阶段 2: 移植 Asset Patch
1. 复制 `src/rr3_asset_patch.cpp`
2. 适配 `hook_address` 到我们的 ARM64 loader
3. 测试: 验证下载冻结消失

### 阶段 3: 移植 Control Scheme
1. 复制 `src/rr3_control_scheme.cpp`
2. 适配文件 I/O 到 Horizon FS
3. 测试: 验证 Joy-Con 输入

### 阶段 4: 移植 FMOD Pump
1. 复制 `src/rr3_fmod_pump.cpp`
2. 适配音频输出到 Switch audio
3. 测试: 验证音频

### 阶段 5: 移植 Bionic 符号表
1. 复制 `src/symtab_*.cpp`
2. 适配到我们的符号解析器
3. 测试: 验证无未解析符号

### 阶段 6: 移植 GL Stubs + Save Patch
1. 复制 `src/symtab_gl_stubs.cpp`
2. 复制 `src/rr3_savefile_patch.cpp`
3. 测试: 验证稳定性

---

## 五、关键符号地址 (v2.7.0)

| 符号 | 地址 | 用途 |
|------|------|------|
| `AssetDownloadService::SkipAssetImpl` | 0x322d9c | Asset patch hook 点 |
| `SaveManager::CreateTempFileWithSaveGame` | 0x922620 | Save patch |
| `Java_com_ea_EAIO_EAIO_Startup` | 0x2367dc | EA IO 转发 |
| `Java_com_firemint_realracing3_MainActivity_hideSplash` | - | Splash 控制 |
| `fmodGetInfo` / `fmodProcess` | - | FMOD 音频 |

---

## 六、版本兼容性

**EapRules**: v2.7.0 ROW (armeabi-v7a)  
**我们**: 待确认 (可能是 v11.x 或 v12.x)

**风险**: 符号地址可能不同，但:
- 符号名相同 (C++ mangled)
- JNI 方法签名相同
- 逻辑相同 (只是地址偏移)

**验证方法**: 用 `nm` 或 `readelf` 检查我们的 `libRealRacing3.so` 是否有相同符号。

---

## 七、预期收益

| 问题 | 当前状态 | 移植后 |
|------|----------|--------|
| 菜单加载 | 挂死 | ✅ cloudcell 兼容层 |
| 下载冻结 | 挂死 | ✅ asset patch |
| Joy-Con 输入 | 未实现 | ✅ control scheme |
| 音频 | 静音 | ✅ FMOD pump |
| 稳定性 | 崩溃 | ✅ save patch + GL stubs |
| Guest View EGL | Eden 挂死 | ✅ 真机 Mesa 应工作 |

**总结**: 这个仓库解决了我们 80% 的问题。核心 JNI 层、cloudcell 兼容、asset patch、control scheme、FMOD pump 都可以直接复制。只需要适配平台层 (EGL/Input/Audio/FS)。
