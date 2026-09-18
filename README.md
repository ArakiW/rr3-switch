<div align="center">

# Real Racing 3 — Nintendo Switch 原生移植

**将 Android ARM64 版 Real Racing 3 移植到 Nintendo Switch Horizon OS**

[English](README_EN.md) | 简体中文

![Status](https://img.shields.io/badge/status-开发中-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-仅供学习-green)

</div>

---

## 项目简介

这是一个将 **Real Racing 3**（Android ARM64 APK）移植到 **Nintendo Switch** 的开源项目。通过自定义的 libnx NRO 加载器，在 Switch 上原生运行这款赛车游戏。

> ⚠️ **免责声明**：本项目仅供学习和研究目的。不绕过 DRM，不伪造账户，不分发 EA 版权数据。用户需自行拥有合法游戏副本。

---

## 主要特性

### ✅ 已完成

| 特性 | 状态 |
|------|------|
| 4-DSO 加载器 | ✅ 完成 |
| TLS/JNI 生命周期 | ✅ 完成 |
| 181 个构造函数 | ✅ 完成 |
| 7 个 ROMFS 数据卷挂载 | ✅ 完成 |
| Guest View 回调 | ✅ 完成 |
| GL 管线验证 | ✅ 完成 |
| Joy-Con 输入桥接 | ✅ 完成 |
| 帧循环运行 | ✅ 完成 |

### 🚧 开发中

| 特性 | 状态 |
|------|------|
| 菜单资产加载 | 🚧 进行中 |
| 离线比赛 | ⏳ 待开始 |
| 音频输出 | ⏳ 待开始 |

---

## 技术架构

```
┌─────────────────────────────────────────────┐
│           Nintendo Switch Horizon OS         │
├─────────────────────────────────────────────┤
│              libnx NRO 加载器                │
│  ┌─────────────────────────────────────┐   │
│  │      4-DSO 动态加载器                │   │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐   │   │
│  │  │libc++│ │Nimble│ │FMOD │ │RR3  │   │   │
│  │  └─────┘ └─────┘ └─────┘ └─────┘   │   │
│  └─────────────────────────────────────┘   │
│  ┌─────────────────────────────────────┐   │
│  │         JNI 桥接层                   │   │
│  │    (233 slot JNIEnv 模拟)            │   │
│  └─────────────────────────────────────┘   │
│  ┌─────────────────────────────────────┐   │
│  │       VFS 路径重映射                 │   │
│  │    (7 个 ROMFS 数据卷)               │   │
│  └─────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

### 核心组件

| 组件 | 说明 |
|------|------|
| `multi_dso_linker.cpp` | 4-DSO 动态加载器，支持 RELRO、TLS、重定位 |
| `jni_bridge.cpp` | JNI 桥接层，233 个 JNIEnv slot 模拟 |
| `guest_filesystem.cpp` | VFS 路径重映射，7 个 ROMFS 卷挂载 |
| `guest_view_probe.cpp` | Guest View 生命周期探针 |
| `symbol_resolver.cpp` | 符号解析与 shim 绑定 |

---

## 快速开始

### 环境要求

- devkitPro (devkitA64)
- libnx
- Python 3.x (用于测试)

### 构建

```bash
# 构建 NRO
powershell -File tools/Build-NativeSwitch.ps1

# 运行测试
cd native-switch/tests
python test_*.py
```

### 部署

1. 将 `dist/rr3-native-rehost-complete-0.22.4/` 复制到 Switch SD 卡
2. 通过 Homebrew Launcher 启动
3. 按 ZR → ZL → ZL 进入 Guest View

---

## 项目结构

```
rr3-switch/
├── native-switch/          # Switch 原生代码
│   ├── source/            # C++ 源文件
│   ├── include/           # 头文件
│   ├── tests/             # 契约测试
│   └── romfs/             # ROMFS 资源
├── reverse/               # 逆向工程材料
│   ├── native/arm64/      # 原始 DSO
│   └── decomp-calltree/   # 反编译结果
├── analysis/              # 分析文档
├── tools/                 # 构建工具
├── docs/                  # 项目文档
└── dist/                  # 发行包
```

---

## 当前进度

### 已验证里程碑

| 里程碑 | 真机测试 | 说明 |
|--------|----------|------|
| NRO 构建 | ✅ | 4 个 DSO 成功加载 |
| TLS/JIT | ✅ | Guest TLS 作用域验证通过 |
| JNI 生命周期 | ✅ | onCreate→onStart→onResume 全部返回 |
| Guest View | ✅ | Created/Changed/Render + eGL 交换 |
| GL 管线 | ✅ | Host 全屏四边形显示正常 |
| 输入桥接 | ✅ | Joy-Con → ControllerManager |

### 待解决

| 问题 | 描述 |
|------|------|
| 菜单加载 | hideSplash 后菜单资产未加载 |
| DSO 兼容性 | 模拟器上加载器受限 |

---

## 文档

- [中文文档](docs/zh-CN/README.md)
- [English Documentation](docs/en-US/README.md)
- [项目状态](PROJECT_STATUS.md)
- [恢复计划](docs/RR3_Recovery_Package/rr3_recovery/RR3_RECOVERY_PLAN.md)

---

## 贡献

欢迎提交 Issue 和 Pull Request！

### 开发规范

1. 遵循现有代码风格
2. 添加必要的测试
3. 更新相关文档
4. 确保 11 个契约测试全部通过

---

## 许可证

本项目代码采用 MIT 许可证。游戏数据版权归 Electronic Arts 所有。

---

## 致谢

- [devkitPro](https://devkitpro.org/) - Switch 开发工具链
- [libnx](https://github.com/switchbrew/libnx) - Switch homebrew 库
- [Mesa](https://mesa3d.org/) - GL 实现
- 所有测试和反馈的用户
