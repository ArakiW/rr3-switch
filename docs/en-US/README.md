# Real Racing 3 Switch Port — English Documentation

## Project Overview

This project ports Android ARM64 Real Racing 3 to Nintendo Switch Horizon OS as a native libnx NRO.

### Goals

- Launch the game
- Enter the menu
- Select offline race
- Joy-Con driving
- Complete race
- Safe exit

### Constraints

- No DRM bypass
- No account forgery
- No distribution of EA copyrighted data
- Offline/single-player only
- Only replace NRO on TF card

---

## Technical Architecture

### 4-DSO Loader

The project uses a custom 4-DSO dynamic loader to load these shared libraries:

| DSO | Description |
|-----|-------------|
| libc++_shared.so | Android C++ standard library |
| libNimble.so | EA Nimble framework |
| libfmodex.so | FMOD audio engine |
| libRealRacing3.so | Main game logic |

### JNI Bridge Layer

Emulates Android JNI environment, supporting 233 JNIEnv slots:

- Lifecycle callbacks (onCreate/onStart/onResume)
- Device info queries
- Filesystem access
- Input handling
- GL state management

### VFS Path Remapping

Maps Android paths to Switch ROMFS:

```
/storage/emulated/0/... → sdmc:/switch/rr3-native-rehost/...
files/.depot/...        → rr3core:/files/.depot/...
gui_assets/2048x1536    → rr3gui0:/...
gui_assets/960x640      → rr3gui1:/...
```

---

## Build Guide

### Requirements

- Windows 10/11
- devkitPro (devkitA64)
- Python 3.8+

### Build Steps

```powershell
# 1. Build NRO
powershell -File tools/Build-NativeSwitch.ps1

# 2. Run tests
cd native-switch/tests
python test_*.py

# 3. Package
# Build script auto-packages to dist/ directory
```

### Deploy to Switch

1. Copy `dist/rr3-native-rehost-complete-0.22.4/` to SD card
2. Ensure directory structure:
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
3. Launch via Homebrew Launcher
4. Press ZR → ZL → ZL to enter Guest View

---

## Current Progress

### Completed

| Milestone | Status |
|-----------|--------|
| NRO Build | ✅ |
| 4-DSO Loading | ✅ |
| TLS/JNI | ✅ |
| Constructors | ✅ |
| Guest View | ✅ |
| GL Pipeline | ✅ |
| Input Bridge | ✅ |
| Frame Loop | ✅ |

### In Progress

| Task | Description |
|------|-------------|
| Menu Loading | Menu assets not loading after hideSplash |
| Cloudcell Init | Needs full initialization to load menu |

---

## Debugging

### Log Files

| File | Description |
|------|-------------|
| `sdmc:/switch/rr3-native-rehost.guest-view.log` | Guest View log |
| `sdmc:/switch/rr3-native-rehost.guest-boot.progress` | Boot progress |
| `sdmc:/switch/rr3-native-rehost.loader-report.txt` | Loader report |

### Emulator Debugging

Use Eden emulator (based on yuzu):

```powershell
# Launch emulator
E:\Eden\eden.exe "path\to\rr3-native-rehost.nro"
```

Note: DSO loader may be restricted on emulator.

---

## FAQ

### Q: Why doesn't the menu display?

A: Menu loading depends on cloudcell initialization completion. This is currently being resolved.

### Q: Where to get data volumes?

A: Data volumes must be extracted from a legitimate game copy. This project does not distribute game data.

### Q: Which Switch models are supported?

A: All Switch models running Horizon OS (requires Homebrew Launcher).

---

## Contact

- GitHub Issues: Project repository
- Test feedback: Please submit via Issues
