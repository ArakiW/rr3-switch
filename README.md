# Real Racing 3 → Nintendo Switch

把 Android 版 Real Racing 3 逆向移植成 Switch 原生程序。

> 仓库里只有移植层的源码。游戏本身的代码和数据来自你自己的那份应用。详见 [NOTICE.md](NOTICE.md)。

[English](README_EN.md) | 简体中文

![Status](https://img.shields.io/badge/status-In%20Development-orange)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red)
![Language](https://img.shields.io/badge/language-C%2B%2B-blue)
![License](https://img.shields.io/badge/license-MIT-blue)
![Repo](https://img.shields.io/badge/repo-source%20only-brightgreen)

---

## 现在能干什么

**能开机、能进菜单、能选车、能进比赛，而且手柄是真的能开车。**

| | |
|---|---|
| 启动到游戏主界面 | ✅ |
| 选车、进入比赛 | ✅ |
| 手柄开车 | ✅ |
| 音乐和音效 | ✅ |
| 自己开完一整圈，圈数正常计 | ✅ |
| 端口自己开的演示模式跑完一圈 | ⚠️ 还不行 |
| 主机上偶发黑屏 | ⚠️ 已定位到原因，修复中 |

各项的完整状态见 [docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md)。

## 怎么装

1. 去 [Releases](https://github.com/ArakiW/rr3-switch/releases/latest) 下最新的 zip（**约 6.5 MB**）
2. 解压，把 `switch\` 里的东西覆盖到卡根目录
3. **发布包不含游戏的内容**——从你自己的应用副本里取出客机运行时、界面字体和 `.depot` 文本，
   放到卡的 `sdmc:/switch/rr3-native-rehost/` 旁边（程序会自己去那里找）
4. 用 Homebrew Launcher 启动
5. 进比赛后，**比赛里的教程框要自己按一下 A**
6. 右扳机 / A 是油门，左摇杆是转向

完整步骤（放哪些文件、分卷整包、小卡空间不够的装法、该回传哪些日志）见
**[tools/README-hardware-test.md](tools/README-hardware-test.md)**。

默认是**你自己开车**。想看端口自己开，把 `rr3-gamepad.txt` 里的 `autopilot = 0` 改成 `1`。

## 怎么构建

```powershell
powershell -File tools\Build-NativeSwitch.ps1              # 构建（完整）
powershell -File tools\Build-NativeSwitch.ps1 -CleanLoader  # 构建（干净：RomFS 里不放任何 EA 材料）
powershell -File .build\gate.ps1                           # 构建 + 自检
```

需要 devkitPro 的 devkitA64 和 libnx。

**干净构建**（`-CleanLoader`）是你要发布的那一个：它的 RomFS 里只有本项目自己的东西
（libc++、OFL 授权的中文引导字体、字体许可），游戏的内容由使用者自己放到卡上。
默认构建会把它们打进 RomFS，所以仓库里不需要存放任何游戏内容——两种都成立。

## 最近修好了什么

- **发布包第一次可以不含任何 EA 材料**：6.5 MiB，实测 NRO 里没有 EA 文件的字节。
- **车终于归玩家开了。** 以前那个开关只管手柄那条路，你的主机摇杆其实是死的。
- **追踪方向不再每帧乱翻**，车不会被自己的控制器推着倒着走。
- **被护栏卡住能脱困**，不会撞一次就钉死在那儿不动。
- **主机冻死已经定位到原因**：客机等一个条件变量时，等的那个信号被丢了（宿主机条件变量没有记忆）。
  现在等待会自己记账、不会无限期死等，模拟器实测**从卡死的 532 帧变成持续渲染 3762 帧**。

<details>
<summary>这一轮的实测数字</summary>

- 玩家输入端到端到达车：24 个连续采样全部「脚本要的轴 = 客机收到的轴」，闭环未覆写。
- 方向锁定后：目标跑到车背后的采样 45 % → 13 %，圈进度 5.08 % → 8.65 %。
- 脱困修好后：比赛帧数 4770 → 5760，圈进度 8.65 % → 10.35 %。
- 冻死修复前后（同一模拟器、同一张卡、同样关掉 census）：
  渲染帧 **532 → 3762**，截图 **5 → 84**，诊断日志 **186 MB → 24 MB**。
- Eden 上端口自身只占帧时间 0.1 %（菜单）/ 1.0 %（比赛），其余是客机代码在被模拟执行
  ⇒ 模拟器的帧率不代表主机。
- 诊断日志量砍掉 20 %。

每一项的推导过程和原始数据在 [docs/OPEN-ISSUES.md](docs/OPEN-ISSUES.md)。

</details>

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
| [docs/FEATURE-MATRIX.md](docs/FEATURE-MATRIX.md) | 功能矩阵 |
| [tools/README-hardware-test.md](tools/README-hardware-test.md) | 主机安装与验证指南（中文） |
| [PORT_PROGRESS.md](PORT_PROGRESS.md) | 逐轮时间线 |
| [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md) | 逆向工程方法与结论 |
| [NOTICE.md](NOTICE.md) | 第三方代码与授权、合规边界 |

## 许可

本移植层代码以 **MIT** 发布（见 [LICENSE](LICENSE)）。第三方组件与其授权见
[NOTICE.md](NOTICE.md)。游戏本身的著作权属于 Electronic Arts；仓库不收录任何
游戏内容，**发布包也不含**——游戏的内容由使用者从自己的应用副本取出放到卡上。

## 致谢

- [devkitPro](https://devkitpro.org/) — Switch 开发工具链
- [libnx](https://github.com/switchbrew/libnx) — Switch homebrew 库
- [Mesa](https://mesa3gl.org/) — GL 实现
- 所有提供反馈和日志的用户
