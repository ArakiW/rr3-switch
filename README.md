# Real Racing 3 → Nintendo Switch（Horizon OS 原生移植）

**把 Android ARM64 版 Real Racing 3 14.0.1 逆向移植成 Switch 原生 `.nro`。**

> 本仓库只提供移植层的源码；游戏自身的代码与数据来自你自己的那份应用。
> 详见 [NOTICE.md](NOTICE.md)。

English | [简体中文](README.md)

![Status](https://img.shields.io/badge/status-In%20Development-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-MIT-blue)
![Compliance](https://img.shields.io/badge/EA%20assets-not%20redistributed-brightgreen)

---

## 现在到哪一步了（2026-11-05）

**能开机、能进前端、能进比赛、手柄能真的开车。**

| 能力 | 状态 | 证据 |
|---|---|---|
| libnx NRO 引导 + 4-DSO 动态加载（TLS / 重定位 / RELRO） | ✅ | `multi_dso_linker.cpp`，门禁逐轮验证 |
| JNI 桥（233 槽 JNIEnv 模拟）+ 181 个构造器 | ✅ | `jni_bridge.cpp` |
| 客机 View 生命周期（created / changed / render）+ eGL 呈现 | ✅ | `guest_view_probe.cpp` |
| 文件系统路径重映射（7 个 ROMFS 数据卷） | ✅ | `guest_filesystem.cpp` |
| **进前端、选车、进比赛** | ✅ | 端口自己按客机自己的控件树找到比赛按钮（`ui-ctl`） |
| **人工驾驶** | ✅ | 2026-11-04 实测：脚本要的轴 = 客机收到的轴，闭环未覆写，车速 1750→6464 |
| 手柄（Pro / Joy-Con 原生，DS4 / DualSense / Xbox 需 hidretro 或 dinput_redux） | ✅ | `rr3-gamepad.txt` 的 22 条绑定 |
| 语音 / 音乐 / 音效（FMOD） | ✅ | `fmod_shim.cpp` |
| **跑完一整圈、圈数计数器** | ⚠️ | **人工驾驶下正常**；端口自己的自动驾驶（attract mode）还跑不完，见下 |

### 端口自己的自动驾驶（可选的 attract mode）

`rr3-gamepad.txt` 里 `autopilot = 0`（你开）或 `1`（端口开，默认 **0**）。
自动驾驶跟的是赛道**中心线**，而赛道自带的赛车线（`ai_spline`）比中心线短 67 m/圈、
相差 2.92~8.25 m ⇒ 车在弯道外侧跑，每场撞护栏约 10 次，**最好只跑到一圈的 10.35 %**。
**这是自动驾驶的质量问题，不是移植缺陷**——你自己开，圈数就正常计。

### 这一轮改掉的东西（都有实测数字）

* **追踪方向不再每帧重判** —— 以前方向是按瞬时车头逐帧重算的，车被磕一下就把前瞻点
  甩到车另一侧，控制器满舵反向、车倒着走、车头再翻，形成正反馈回路。
  现在方向**按场次锁定**，只判一次，用的是车在 48 采样环上的位移窗口。
  实测：目标跑到车背后的采样 **45 % → 13 %**，圈进度 **5.08 % → 8.65 %**。
* **车被护栏压死时能脱困** —— 停转检测的「在线上就不脱困」豁免把真正卡死的车也放过了
  （被压死的车横向残差 2.07 m，落在 3 m 豁免线内）。改用**车的世界坐标**判「钉死」。
  实测：比赛帧数 **4770 → 5760**，圈进度 **8.65 % → 10.35 %**。
* **玩家能真的接手开车** —— 以前 `autopilot = 0` 这个开关**只管物理手柄那条路**，
  而出货包带着实验用的手柄脚本，脚本一旦激活**物理手柄整条路完全不会被走到**
  ⇒ 主机上你的摇杆是死的。现在模式统一管两条路，脚本检测到你在动手柄就自动让位，
  并且脚本路补上了自己的仪表（`scripted-state`）。
* **黑屏有账可查了** —— 每帧日志现在带 `tt/tr/ts/tp`：整帧 / 客机渲染 / 端口自身 / 呈现。
  Eden 上实测：**端口自身只占 0.1 %（菜单）/ 1.0 %（比赛）**，其余全是客机的 ARM64
  代码在**被模拟执行** ⇒ **模拟器上的帧率不代表主机**，主机的黑屏更可能是启动阶段。
* 诊断日志量砍掉 **20 %**（两个去重表本来都太小）；guest-thread census 实测**不是**
  帧率杠杆但白吃 9 % CPU。

---

## 构建

```powershell
# 构建 NRO
powershell -File tools\Build-NativeSwitch.ps1

# 门禁：构建 + 输入检查 + 39 个契约测试 + 符号解析
powershell -File .build\gate.ps1
```

门禁当前状态：**测试 39/39 通过**；Cloudcell 符号 **38 个解析成功 / 16 个确认不存在 /
0 个意外缺失**。

构建产物：`native-switch/rr3-native-rehost.nro`（含内嵌 RomFS）。
**仓库里不含 EA 的任何东西**——客机二进制、文本、字体、FMOD 来自你自己那份应用，
构建时被 stage 进 RomFS；Release 上的包因此带着它们。

## 装到主机

完整步骤见 **[tools/README-hardware-test.md](tools/README-hardware-test.md)**（中文，
含分卷拼接、只更新应用、小卡空间不足三种装法，以及回传什么日志）。

要点：

1. 把 `dist/rr3-switch-app-*.zip` 解压后覆盖到卡根目录（45 MB，只含应用与控制脚本）；
2. 游戏自身的文件来自你自己的那份应用，端口会在 NRO 同目录
   （`sdmc:/switch/rr3-native-rehost/`）找到它们；
3. 启动，进入比赛后**比赛内的教程框要自己按一下 A**；
4. 右扳机 / A 是油门，左摇杆是转向。

## 仓库结构

```
native-switch/          Switch 原生移植层（libnx + Mesa）
  source/               C++ 源码
  include/              头文件
  tests/                契约测试（宿主自测 + Python）
  card/                 卡上控制脚本（随包出货的那几份）
tools/                  构建与部署脚本、主机安装说明
docs/                  文档（见下）
reverse/                逆向工程材料（不进发行物）
dist/                   打包产物
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md) | **主交接文档**：定位到的问题、实测数字、还在开着的项 |
| [PORT_PROGRESS.md](PORT_PROGRESS.md) | 逐轮时间线 |
| [docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md) | 功能矩阵 |
| [tools/README-hardware-test.md](tools/README-hardware-test.md) | 主机安装与验证指南（中文） |
| [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) | 逆向工程方法与结论 |
| [NOTICE.md](NOTICE.md) | 第三方代码与授权、**合规边界** |

## 许可

本移植层代码以 **MIT** 发布（见 [LICENSE](LICENSE)）。第三方组件与其授权见
[NOTICE.md](NOTICE.md)。游戏本身的著作权属于 Electronic Arts；仓库不收录任何
游戏内容，发布包里的客机运行时与资源来自使用者自己的应用副本。

## 致谢

- [devkitPro](https://devkitpro.org/) — Switch 开发工具链
- [libnx](https://github.com/switchbrew/libnx) — Switch homebrew 库
- [Mesa](https://mesa3d.org/) — GL 实现
- 所有提供反馈和日志的用户
