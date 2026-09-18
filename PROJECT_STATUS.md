# RR3 Switch 移植 — 项目状态总结

**更新时间**: 2026-09-09  
**最新 NRO SHA-256**: `38067168B1B0E52B74C9A5CF8720F37E9707251D9F08184306223CC2E6C2BDD3`

---

## 一、项目目标

将 Android ARM64 版 Real Racing 3（无源码 APK）移植到 Nintendo Switch Horizon OS，作为原生 libnx NRO 运行。

**成功标准**: 启动 → 进入菜单 → 选择离线比赛 → Joy-Con 驾驶 → 完成比赛 → 安全退出

**约束**:
- 不绕过 DRM、不伪造账户、不公开分发 EA 数据
- 仅离线/单人模式
- 只替换 TF 卡上的 NRO，七个数据卷不变
- 用户通过连接 Switch 复制文件 — **最小化真机测试轮次**

---

## 二、当前死循环问题

### 核心卡点

Guest 在 `hideSplash` 后进入主渲染路径，但**不加载菜单资产**（无 `MainMenu.gui`、无 `sprites_*` 打开）。屏幕显示蓝色棋盘格（Mesa undefined-texture checkerboard）或纯蓝 clear color。

### 已验证的工作状态

| 项目 | 状态 | 证据 |
|------|------|------|
| 4-DSO 加载器 | ✅ | 4 images, 142,661 RELATIVE relocations |
| TLS/JNI 生命周期 | ✅ | onCreate→onStart→onResume 全部返回 |
| 181 个构造函数 | ✅ | 设备验证通过 |
| Guest View 回调 | ✅ | Created/Changed/Render + eglSwapBuffers |
| GL 管线 | ✅ | host 全屏四边形显示正常 |
| Joy-Con 输入 | ✅ | ControllerManager JNI pump |
| 帧循环 | ✅ | connect() stub 后持续运行到 frame 170+ |
| hideSplash | ✅ | splashFrames 达到 6 后自动调用 |
| draw-arrays | ✅ | guest 发出 4 次 draw-arrays |

### 死循环的根因链

```
guest 调用 hideSplash
    ↓
进入主渲染路径 (FUN_013b4824 → FUN_01205160)
    ↓
检查 cloudcell 组件 (byte 1217)
    ↓
cloudcell 未真正初始化 (JNI stub 不做实际初始化)
    ↓
跳过菜单加载
    ↓
只做 clear + useProgram + bindBuffer (每帧 1+2+2 次)
    ↓
蓝屏 / 棋盘格
```

### 已尝试的修复（按时间顺序）

| 尝试 | 结果 | 原因 |
|------|------|------|
| 强制 frameState=0 | hideSplash 被调用 ✓ | 但菜单仍不加载 |
| connect() 快速失败 stub | 帧循环不再阻塞 ✓ | 重大进展 |
| cloudcell byte 1217=1 | 设置成功 ✓ | 但 vtable 未初始化 |
| CloudcellResume 多帧调用 | 调用成功 ✓ | 不解锁菜单 |
| isNetworkAvailable=true | 改变 cloudcell 流程 | 无帮助 |
| isDataAvailable=false | hideSplash 不再被调用 ✗ | 回退 |
| onHTTPResponse(id=0) | 崩溃 (空指针) ✗ | 移除 |
| onTimerEvent(id=0) | 崩溃 (空指针) ✗ | 移除 |
| GOT 两层解引用修复 | 读到正确的 cloudcell 指针 ✓ | |
| 假 vtable (40 条目) | 崩溃 (vtable+272=NULL) ✗ | |
| 假 vtable (64 条目全填) | 崩溃 (x0 被破坏) ✗ | |
| extern "C" + 返回 self | 保留 x0 ✓ | |
| 创建 cloudcell 组件 | 成功 ✓ | |
| 创建菜单加载器 | 崩溃 (RELRO 只读) ✗ | |
| svcSetMemoryPermission | 崩溃 (不同内存块) ✗ | |
| NOP 补丁 0xdacf40 | 崩溃 (权限错误) ✗ | 移除 |

### 当前方案

- cloudcell 组件：静态缓冲区 + 64 条目 vtable（全填 stub）+ byte 1217=1
- 菜单加载器：静态缓冲区 + 32 条目 vtable（全填 menuLoaderStub）+ RELRO 安全写入
- vtable stub：`extern "C"`，返回 `self` 保留 x0
- 无 NOP 补丁

---

## 三、关键技术发现

### 地址映射

| 符号 | ELF VA | 从 onViewRenderJNI 的 delta |
|------|--------|---------------------------|
| onViewRenderJNI | 0x12B3614 | 0 |
| cloudcell GOT | 0x1f6d5d0 | +0xCB9FBC |
| 菜单加载器全局 | 0x1f6df50 | +0xCBA93C |
| FUN_014bcbac | 0x13BCBAC | +0x109598 |
| FUN_012034fc | 0x11034FC | -0x1B0118 |
| CloudcellResume | 0x1cf2ad8 | +0xA3F4C4 |
| hideSplash 检查 | 0x12aeee0 | -0x4E1634 |

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
- `renderer+0x1f5a9`: splash→menu 触发位
- `renderer+0x1f5ab/0x1f5ac/0x120`: 副门控

### 内存区域

- `.got` (0x1f6d598): RELRO 只读，写入需 svcSetMemoryPermission
- `.text` (0x988000): RX，svcSetMemoryPermission 无效（不同内存块）
- `.data` (0x1f80880): 可写
- `.bss` (0x1f94f20): 可写

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

`libRealRacing3.so` SHA-256: `57edb6314fb1f73916446dccf2aab2223a932ae63ccdf2149cffeddb5aa73051` — 与用户 APK 完全一致

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

---

## 六、下一步方向

### 方向 A：深入 cloudcell 初始化
- 反汇编 cloudcell Constructor 的真实实现
- 找到 vtable 的真实函数地址
- 用真实函数替换 stub

### 方向 B：绕过 cloudcell
- 找到菜单加载的直接触发点
- 从 host 端直接调用菜单加载函数 (0xdacf08)
- 需要找到菜单管理器对象

### 方向 C：二进制补丁（需解决权限问题）
- 找到正确的方式修改 guest 代码段权限
- 或在 loader 阶段就以 RWX 映射 .text

### 方向 D：分析 LoadingScreen_Init.gui
- 该文件被 guest 成功读取
- 可能包含菜单加载的配置信息
- 需要解密 (magic 31423a44)

---

## 七、构建与测试流程

```bash
# 构建
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\AI\RealRacing3-switch\tools\Build-NativeSwitch.ps1"

# 测试 (11 个契约测试)
cd native-switch/tests && python test_*.py

# 打包
# NRO 复制到 dist/rr3-native-rehost-complete-0.22.4/
# 更新 rr3-data-manifest.json 的 program.bytes/sha256
# 重新生成 checksums.sha256
```

**设备测试**: ZR → ZL → ZL → 等 30 秒 → 复制 `sdmc:/switch/rr3-native-rehost.guest-view.log`

---

## 八、关键文件

| 文件 | 作用 |
|------|------|
| `native-switch/source/guest_view_probe.cpp` | View 探针、FSM 强制、对象创建 |
| `native-switch/source/jni_bridge.cpp` | JNI stub 层 |
| `native-switch/source/guest_filesystem.cpp` | 路径重映射 |
| `native-switch/source/symbol_resolver.cpp` | 符号绑定、connect stub |
| `native-switch/source/multi_dso_linker.cpp` | 4-DSO 加载器 |
| `reverse/native/arm64/libRealRacing3.so` | guest DSO |
| `reverse/native/decomp-calltree/` | Ghidra 反编译 |
