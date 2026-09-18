<div align="center">

# Real Racing 3 — Nintendo Switch Native Port

**Porting Android ARM64 Real Racing 3 to Nintendo Switch Horizon OS**

English | [简体中文](README.md)

![Status](https://img.shields.io/badge/status-In%20Development-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-Learning%20Only-green)

</div>

---

## Overview

This is an open-source project that ports **Real Racing 3** (Android ARM64 APK) to **Nintendo Switch**. Through a custom libnx NRO loader, the racing game runs natively on Switch.

> ⚠️ **Disclaimer**: This project is for learning and research purposes only. It does not bypass DRM, forge accounts, or distribute EA copyrighted data. Users must own a legitimate copy of the game.

---

## Key Features

### ✅ Completed

| Feature | Status |
|---------|--------|
| 4-DSO Loader | ✅ Done |
| TLS/JNI Lifecycle | ✅ Done |
| 181 Constructors | ✅ Done |
| 7 ROMFS Data Volumes | ✅ Done |
| Guest View Callbacks | ✅ Done |
| GL Pipeline Verification | ✅ Done |
| Joy-Con Input Bridge | ✅ Done |
| Frame Loop Running | ✅ Done |

### 🚧 In Development

| Feature | Status |
|---------|--------|
| Menu Asset Loading | 🚧 In Progress |
| Offline Race | ⏳ Pending |
| Audio Output | ⏳ Pending |

---

## Technical Architecture

```
┌─────────────────────────────────────────────┐
│           Nintendo Switch Horizon OS         │
├─────────────────────────────────────────────┤
│              libnx NRO Loader                │
│  ┌─────────────────────────────────────┐   │
│  │      4-DSO Dynamic Loader            │   │
│  │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐   │   │
│  │  │libc++│ │Nimble│ │FMOD │ │RR3  │   │   │
│  │  └─────┘ └─────┘ └─────┘ └─────┘   │   │
│  └─────────────────────────────────────┘   │
│  ┌─────────────────────────────────────┐   │
│  │         JNI Bridge Layer             │   │
│  │    (233-slot JNIEnv emulation)       │   │
│  └─────────────────────────────────────┘   │
│  ┌─────────────────────────────────────┐   │
│  │       VFS Path Remapping             │   │
│  │    (7 ROMFS data volumes)            │   │
│  └─────────────────────────────────────┘   │
└─────────────────────────────────────────────┘
```

### Core Components

| Component | Description |
|-----------|-------------|
| `multi_dso_linker.cpp` | 4-DSO dynamic loader with RELRO, TLS, relocation |
| `jni_bridge.cpp` | JNI bridge layer, 233 JNIEnv slot emulation |
| `guest_filesystem.cpp` | VFS path remapping, 7 ROMFS volume mounts |
| `guest_view_probe.cpp` | Guest View lifecycle probe |
| `symbol_resolver.cpp` | Symbol resolution and shim binding |

---

## Quick Start

### Requirements

- devkitPro (devkitA64)
- libnx
- Python 3.x (for tests)

### Build

```bash
# Build NRO
powershell -File tools/Build-NativeSwitch.ps1

# Run tests
cd native-switch/tests
python test_*.py
```

### Deploy

1. Copy `dist/rr3-native-rehost-complete-0.22.4/` to Switch SD card
2. Launch via Homebrew Launcher
3. Press ZR → ZL → ZL to enter Guest View

---

## Project Structure

```
rr3-switch/
├── native-switch/          # Switch native code
│   ├── source/            # C++ source files
│   ├── include/           # Header files
│   ├── tests/             # Contract tests
│   └── romfs/             # ROMFS resources
├── reverse/               # Reverse engineering materials
│   ├── native/arm64/      # Original DSO
│   └── decomp-calltree/   # Decompilation results
├── analysis/              # Analysis documents
├── tools/                 # Build tools
├── docs/                  # Project documentation
└── dist/                  # Release packages
```

---

## Current Progress

### Verified Milestones

| Milestone | Device Test | Notes |
|-----------|-------------|-------|
| NRO Build | ✅ | 4 DSOs loaded successfully |
| TLS/JIT | ✅ | Guest TLS scope verified |
| JNI Lifecycle | ✅ | onCreate→onStart→onResume all returned |
| Guest View | ✅ | Created/Changed/Render + eGL swap |
| GL Pipeline | ✅ | Host fullscreen quad displays correctly |
| Input Bridge | ✅ | Joy-Con → ControllerManager |

### Known Issues

| Issue | Description |
|-------|-------------|
| Menu Loading | Menu assets not loading after hideSplash |
| DSO Compatibility | Loader restricted on emulator |

---

## Documentation

- [中文文档](docs/zh-CN/README.md)
- [English Documentation](docs/en-US/README.md)
- [Project Status](PROJECT_STATUS.md)
- [Recovery Plan](docs/RR3_Recovery_Package/rr3_recovery/RR3_RECOVERY_PLAN.md)

---

## Contributing

Issues and Pull Requests are welcome!

### Development Guidelines

1. Follow existing code style
2. Add necessary tests
3. Update relevant documentation
4. Ensure all 11 contract tests pass

---

## License

This project's code is licensed under MIT. Game data copyright belongs to Electronic Arts.

---

## Acknowledgments

- [devkitPro](https://devkitpro.org/) - Switch development toolchain
- [libnx](https://github.com/switchbrew/libnx) - Switch homebrew library
- [Mesa](https://mesa3d.org/) - GL implementation
- All users who tested and provided feedback
