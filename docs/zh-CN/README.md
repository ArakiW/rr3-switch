# Real Racing 3 Switch 移植 — 中文文档

## 项目概述

本项目将 Android ARM64 版 Real Racing 3 移植到 Nintendo Switch Horizon OS，作为原生 libnx NRO 运行。

### 目标

- 启动游戏
- 进入菜单
- 选择离线比赛
- Joy-Con 驾驶
- 完成比赛
- 安全退出

### 约束

- 不绕过 DRM
- 不伪造账户
- 不分发 EA 版权数据
- 仅离线/单人模式
- 只替换 TF 卡上的 NRO

---

## 技术架构

### 4-DSO 加载器

项目使用自定义的 4-DSO 动态加载器，加载以下共享库：

| DSO | 说明 |
|-----|------|
| libc++_shared.so | Android C++ 标准库 |
| libNimble.so | EA Nimble 框架 |
| libfmodex.so | FMOD 音频引擎 |
| libRealRacing3.so | 游戏主逻辑 |

### JNI 桥接层

模拟 Android JNI 环境，支持 233 个 JNIEnv slot：

- 生命周期回调（onCreate/onStart/onResume）
- 设备信息查询
- 文件系统访问
- 输入处理
- GL 状态管理

### VFS 路径重映射

将 Android 路径映射到 Switch ROMFS：

```
/storage/emulated/0/... → sdmc:/switch/rr3-native-rehost/...
files/.depot/...        → rr3core:/files/.depot/...
gui_assets/2048x1536    → rr3gui0:/...
gui_assets/960x640      → rr3gui1:/...
```

---

## 构建指南

### 环境要求

- Windows 10/11
- devkitPro (devkitA64)
- Python 3.8+

### 构建步骤

```powershell
# 1. 构建 NRO
powershell -File tools/Build-NativeSwitch.ps1

# 2. 运行测试
cd native-switch/tests
python test_*.py

# 3. 打包
# 构建脚本会自动打包到 dist/ 目录
```

### 部署到 Switch

1. 将 `dist/rr3-native-rehost-complete-0.22.4/` 复制到 SD 卡
2. 确保以下目录结构：
   ```
   sdmc:/switch/rr3-native-rehost/
   ├── rr3-native-rehost.nro
   └── data/
       ├── core.romfs
       ├── gui-hd.romfs
       ├── gui-sd.romfs
       ├── tracks.romfs
       ├── vehicles-classic.romfs
       ├── vehicles-mid.romfs
       └── vehicles-modern.romfs
   ```
3. 通过 Homebrew Launcher 启动
4. 按 ZR → ZL → ZL 进入 Guest View

---

## 当前进度

### 已完成

| 里程碑 | 状态 |
|--------|------|
| NRO 构建 | ✅ |
| 4-DSO 加载 | ✅ |
| TLS/JNI | ✅ |
| 构造函数 | ✅ |
| Guest View | ✅ |
| GL 管线 | ✅ |
| 输入桥接 | ✅ |
| 帧循环 | ✅ |

### 进行中

| 任务 | 描述 |
|------|------|
| 菜单加载 | hideSplash 后菜单资产未加载 |
| cloudcell 初始化 | 需要完整初始化才能加载菜单 |

---

## 调试

### 日志文件

| 文件 | 说明 |
|------|------|
| `sdmc:/switch/rr3-native-rehost.guest-view.log` | Guest View 日志 |
| `sdmc:/switch/rr3-native-rehost.guest-boot.progress` | 启动进度 |
| `sdmc:/switch/rr3-native-rehost.loader-report.txt` | 加载器报告 |

### 模拟器调试

使用 Eden 模拟器（基于 yuzu）：

```powershell
# 启动模拟器
E:\Eden\eden.exe "path\to\rr3-native-rehost.nro"
```

注意：模拟器上 DSO 加载器可能受限。

---

## 常见问题

### Q: 为什么菜单不显示？

A: 菜单加载依赖 cloudcell 初始化完成。当前正在解决此问题。

### Q: 数据卷在哪里获取？

A: 数据卷需要从合法游戏副本中提取。本项目不分发游戏数据。

### Q: 支持哪些 Switch 型号？

A: 支持所有运行 Horizon OS 的 Switch 型号（需 Homebrew Launcher）。

---

## 联系方式

- GitHub Issues: 项目仓库
- 测试反馈: 请通过 Issue 提交
