# RR3 Native Rehost — 实机（Nintendo Switch）测试包

《真实赛车 3》安卓客机 → Nintendo Switch 原生移植（libnx + Mesa / nouveau / NVN）。

**这个包是自包含的**：应用、控制脚本、7 个数据卷、端口的实机配置
`sdmc:/switch/rr3-gfx.conf`，全都在里面，不需要再去别的卡上找。

> **这一版的关键变化：卡上从 10 546 个文件 / 11.21 GiB 变成 186 个文件 / 9.58 GiB。**
>
> 上一版把整棵散落素材树 `.depot`（9 873 个文件）原样拷上卡。实测证明那是**死重量**：
> 七个数据卷先被查（46 574 个文件），卡上那份松散副本只是兜底。
>
> 这一版只装客机**真的要过**的那些文件（`.depot` 157 + `cache` 41 + `doc` 18），
> 并且给一个 `-Profile mini` 档：再省 2.92 GiB。详见 §1.3。

---

## 0. 这一版修的实机崩溃（**如果你之前是「黑屏 + 10 秒后报错关闭」，看这里**）

### 0.1 现象与根因

实机黑屏约 10 秒，然后系统弹「由于发生错误，软件已关闭」。
实机自己的日志说清楚了死在哪：

* 实机日志 **292,710 字节 / 1,175 行**；模拟器日志 39 MB，而它在 **t=10s 时是 295,222 字节**。
  ⇒ **实机死在和模拟器第 10 秒完全相同的位置**（差 0.8%）。
* 逐行对比：两边到 `phase=guest-frame-selftest verdict=SELFTEST_PASS` 完全一致——
  **那正是实机日志的最后一行**。模拟器的下一行是 `phase=gl-ctx-early`。
  ⇒ **实机一次 EGL 调用都没发生就死了。**

死因是端口那个**「无 surface 的早期 EGL 上下文」**（`eglMakeCurrent(display,
EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)`）。它在模拟器的软件 Mesa 上合法，
**在真机的 NVN 驱动上不合法** ⇒ 内核里 fault ⇒ 系统回收进程。

### 0.2 修法

那个上下文本来只是为了让客机提前采一次 `glGetString(GL_VERSION)`。
**端口本来就拦截了客机的 `glGetString`**，只是以前把它转发给驱动。
现在：**没有当前上下文时由宿主直接作答**，字符串和 Mesa 报的完全一样
（`OpenGL ES 3.2 Mesa 20.1.0-rc3`），于是

* 客机解析出的版本号和以前一模一样（major=3，GL 断言守卫 `major < 3` 不会静默失败），
* **端口在客机自己的「带窗口」上下文之前，一个 EGL 调用都不做**。

⇒ `glctxearly` 的默认值改成 **0**（`glctxearly=1` 可以把旧行为调回来做 A/B）。
模拟器实测（归档 `20261102-172615-eden-rr3-q121-no-early-gl`）：

```
phase=gl-ctx-early ... disabled by conf
phase=view-egl-init-begin -> view-egl-init-ok -> view-egl-surface-ok
   -> view-egl-context-ok -> view-egl-current-ok
```

### 0.3 这一版还修好了「货币图标变成字母」

帧里（`20261102-154600-eden-rr3-q120`，`frames/frame-000300.png`）顶栏写的是

```
MS 500,000      RS 22,141,500      C 20,354
```

—— 那三个本该是**游戏的货币图标**。原因是端口把游戏的符号字体
`r3_symbols.ttf` 换成了正文字体（`ConduitITCPro-Regular`，653 个字形、没有符号），
于是图标码位被查成了字母；而且符号字体的 upem 是 **2048**、正文字体是 **1000**，
**同一个字号下图标被画成了一半大**。

⇒ 现在 `r3_symbols.ttf` **用它自己**（从卡上打开），实测
（`20261102-173050-eden-rr3-q122-real-symbol-font`）：

```
req=sdmc:/…/.depot/r3_symbols.ttf  face=sdmc:/…/.depot/r3_symbols.ttf
substituted=0 symbol=0  upem=2048  glyphs=236
```

帧里那个金币已经是游戏自己的图标了。

### 0.4 你要做的

**如果卡上已经有数据卷**：下 `rr3-switch-app-20261102.zip`（45.3 MiB，一个文件），
解包，把 `switch\` 里的东西覆盖到卡根即可。**不要删 `switch/rr3-gfx.conf`。**

**如果是新卡**：用下面的分卷整包。

---

## 1. 装到卡上

### 1.1 方式 A：分卷 zip（推荐，脚本全自动）

下载**全部 4 个分卷** `rr3-switch-20261102.zip.001` … `.004`，加上
`Join-And-Extract.ps1`，放进**同一个文件夹**，然后：

```powershell
powershell -ExecutionPolicy Bypass -File Join-And-Extract.ps1 -Card F:
```

它会：把分卷拼成一个 zip → 解包到 `.\rr3-package` → 调用包里的 `Copy-To-SwitchCard.ps1`
→ 把整棵树装到 SD 卡。`F:` 换成你的卡。

> 分卷必须**一个不缺**。脚本会把找到的分卷按文件名排序拼接，少一个就会得到一个坏 zip。

### 1.2 方式 B：只更新应用（**45 MB，一个文件**）

如果卡上已经有数据卷，只想换应用和脚本（比如升到带手柄映射的这一版），
用 `rr3-switch-app-20261102.zip`：解包，把 `switch\` 里的东西覆盖到卡根即可。
**不要删 `switch/rr3-gfx.conf`**（见 §4）。

### 1.3 方式 C：小卡（空间不够）

```powershell
powershell -ExecutionPolicy Bypass -File Copy-To-SwitchCard.ps1 -Card F: -Profile mini
```

`mini` 档少装两个数据卷，**省 2.92 GiB**（9.56 → 6.64 GiB）：

| 不装的卷 | 大小 | 实测依据 | 代价 |
|---|---|---|---|
| `gui-hd.romfs` | 1.55 GiB | 被查 272 次（gui-sd 是 2 321 次），且**全部**是 2048×1536 那套 | 载入画面回退到 960×640 |
| `vehicles-modern.romfs` | 1.37 GiB | 被查 **0 次**（跑的是 2015 前的车） | 2021 年后的新车不可用 |

> **4 GB 的卡装不下全部游戏数据。** 最小的一组（core + gui-sd + tracks +
> vehicles-classic）已经是 4.87 GiB。要真正塞进 4 GB，得按「一条赛道 + 一辆车」
> 裁数据卷 —— 那是下一个工程，不是打包能解决的。

### 1.4 卡上会长什么样

```
<SD卡>/switch/
├── rr3-gfx.conf                 端口的实机配置（见 §4，**不要删**）
└── rr3-native-rehost/
    ├── rr3-native-rehost.nro     53.5 MB   应用
    ├── rr3.romfs                 44.8 MB   内嵌 RomFS
    ├── rr3-touch.txt                       48 跳触屏脚本
    ├── rr3-drive.txt                       转向控制器配置
    ├── rr3-pad.txt / rr3-action.txt
    ├── rr3-screen.txt                      屏幕尺寸（默认 1280×720）
    ├── rr3-settings.txt                    含 AA_LEVEL = 4（抗锯齿开）
    ├── rr3-ui.txt / rr3-open.txt
    ├── rr3-gamepad.txt                     **手柄映射 + 谁在开车（见 §10）**
    ├── data/                               7 个 *.romfs 数据卷 + manifest
    ├── .depot/  cache/  doc/              只放实测被读到的那些文件
    └── proc/ save/
```

启动器标题栏显示的就是 `switch\rr3-native-rehost` 这个路径。

> **包里还附了一个 `optional/card-depot-full.zip`**：完整的散落素材树（10 413 个文件）。
> 默认**不装**——那正是低容量卡不想要的东西。如果游戏报缺素材，再解到卡上：
> `Copy-To-SwitchCard.ps1 -Card F: -OptionalTree`。

---

## 2. 启动

用 homebrew 加载器（Tinfoil / nxlink / hbmenu）打开上面的路径。
建议 **插底座 + 电视 1080p 输出**（1280×720 的画面放大到 1080p 是正常的）。

端口自己的中文载入界面**默认不显示**：实机配置里的 `nofb=1` 让端口不去创建/销毁默认
NWindow 上的 framebuffer——2026-10-11 实测过「创建再关闭」会把 NWindow 弄坏，之后
Mesa 的 `eglInitialize` 永远不返回。代价只是少一个进度条。

---

## 3. 这一版相对上一版的实质变化

| 项 | 状态 | 数字 |
|---|---|---|
| **抗锯齿** | 已开（`rr3-settings.txt` 里 `AA_LEVEL = 4`） | 摩天轮辐条单像素尖峰 6.78% → **1.55%（−77%）**；Switch 上是硬件 MSAA |
| **开机随机冻结** | 已修（根因：客机 `strtoul` 绑宿主实现 ⇒ 宿主在客机线程上无界扫描） | 同一配置 3 帧 → **3360 帧跑满 300 秒** |
| **`.spline` 赛车线** | 已解码并进端口 | `centre_spline` 389 节点、环路 98941 单位，三个 chunk 互证（长度差 <1.5%） |
| **race-state 身份判据** | 改用客机自己的 RTTI 类名 + vptr 复核 + `mres==0x3f` | 之前日志里的 `cls=` 是格式串写死的常量，现在是实测值 |
| **实机配置** | **新增并随包安装 `switch/rr3-gfx.conf`** | 见 §4：上一版把它**删掉**了，那会把客机的四个行为开关一起关掉 |
| **拷贝脚本** | 修好 `.depot` 递归 | 上一版 robocopy **漏了 `/E`**，只拷顶层 ⇒ 卡上会缺 `Tracks\` `vehicles\` `gui_assets\` 等 9872 个素材文件 |

---

## 4. `switch/rr3-gfx.conf`：这次**必须留着**

上一版的说明写的是「这个文件的存在会强制空后端 ⇒ 黑屏，务必删掉」。**那句话是反的。**

代码里的规则（`native-switch/source/gfx/gfx_backend.cpp:809-812`）是：

> 文件不存在、`backend=` 键不存在、或值**不是** `null`/`0`/`on` ⇒ 选 **Mesa**（真机路径）。
> 只有字面写 `backend=null` 才会选那个「所有 GL 调用查表应答、永远不画东西」的空后端。

**所以真正会黑屏的是卡上残留着 `backend=null`**，而不是「文件存在」。

更重要的是，这个文件除了选后端，还决定**客机自己能不能跑起来**：

| 键 | 作用 | 关掉会怎样 |
|---|---|---|
| `ccdisp=2` | 把 JNI 托管方法派发回客机 | 回到 2026-10-19 之前的行为，Cloudcell 回调根本不触发 |
| `ccloginok=1` | Cloudcell 登录答成功 | 客机卡在 `Sub Sync Queued: 10765`，**第 4 帧就死** |
| `ccsocialok=1` | 社交会话报有效 | 客机报 `Unable to send Sync, Social Media not initialised.` 同样空转 |
| `objid=1` | 每个 NewObjectV 给一个真句柄 | 所有实例共用一个假对象，实例方法写错字段 |

删掉这个文件 = Mesa + 上面四个开关全关 = 客机进了云同步空转、**永远进不了比赛**。
三个脚本原来都让你删它（`tools/Use-Real-Hardware.ps1`、`tools/Deploy-To-Switch.ps1`、上一版包的拷贝脚本），现在都改成**安装**它。

### 本包随附的那份就是实测通过的那份

包里的 `switch/rr3-gfx.conf` 与模拟器上跑进比赛的那份**逐字节相同**（sha256 `65643a55…`，3975 B）。
它没有被加注释：端口把这个文件读进 4096 字节缓冲区、**超出的尾巴静默丢弃**，而这份已经 3975 B 了。

| 键 | 值 | 为什么是这个值 |
|---|---|---|
| `backend` | `mesa` | 与「文件不存在」等价，只是让文件自解释 |
| `nofb` | `1` | 实测：不创建再销毁默认 NWindow 的 framebuffer，否则 Mesa 的 `eglInitialize` 永不返回。代价是**端口自己的中文载入界面不显示** |
| `nogo` | `0` | **实测必须为 0**（见下） |
| `mkdiridem` | `0` | 实测值。只会让日志多 4 行 `Error creating directory: .../.depot/CC_Data`，无害 |
| `ccdisp` / `ccloginok` / `ccsocialok` / `objid` | `2` / `1` / `1` / `1` | 见上表 |
| `ccmode` | `bg` | 从后台线程调用捕获到的 Cloudcell 续体 |

### `nogo=1` 被实测否决了

原本有个理由：真机有 Wi-Fi，客机会**真的去连 EA 的服务器**然后卡在 `send()`，
所以想让端口干脆不起 socket 栈（`nogo=1`），让每个外连立刻失败。

**实测（q106，300 秒）把这个理由变成了反例**：

| | q105（`nogo=0`） | q106（`nogo=1`） |
|---|---|---|
| 帧数 | 960 | 13680（在空转） |
| 背缓冲点亮 | **921600 / 921600** | **0 / 921600** |
| draw 调用 | 38390 | **0** |
| `ASSET COMPLETE` | 8 | **0** |
| `EventMapScreen` | 262 | **0** |
| 起跑灯 / 比赛画面 | 7 / 10 | **0 / 0** |
| 场景 FBO 尺寸 | 1280×720 | **0×0** |
| 客机日志里 `download` | **301 条** | **0 条** |

差别在客机自己的日志里：

```
q105:  CC: AssetManager::StartDownloadService()
       CC: AssetManager::StartListDownload() : Downloading asset_list_updates.txt
q106:  （这两行从来没有出现）客机最后停在 "startMusic disabled during download"
```

⇒ **客机的素材管线需要 socket 层存在**（哪怕只是让它快速失败）。socket 栈根本不起，
整个 Cloudcell / 素材机器就是死的。

### ⇒ 真机请**先关掉 Wi-Fi**

`nogo=0` 意味着 socket 栈会起来。真机连着 Wi-Fi 时，客机会去够 EA 的服务器（服务早就没了），
有可能卡在一个等回复的 socket 上。**开机前先把 Switch 的 Wi-Fi 关掉**：
没有网络时，外连会像模拟器上一样立刻失败，客机走它的离线路径。

这是本包唯一的、需要你在真机上动手的一步。

---

## 5. 预期现象（按顺序）

1. 启动 → 端口初始化 → JNI 生命周期 → `Created/Changed/Render`（载入界面被 §2 的 `nofb=1` 跳过）。
2. 进前端地图（EventMapScreen）。
3. **进入比赛**（起跑灯 → 3D 场景 → HUD 显示圈数）。
4. 端口的转向控制器接管：它从客机自己的 `.spline` 赛车线算出一个前视目标点，用纯追踪打方向。

---

## 6. 已知问题（**这一版请预期**）

- **比赛现在能开始了，而且端口会开车。** 之前「车一直不动」有三个原因，都已定位并修掉：
  1. 客机自己的**比赛内教程对话框**不吃端口的合成触摸（客机确实收到了，坐标也对，但它不响应）。
     ⇒ 现在 `rr3-pad.txt` 在 frame 600 起**按住客机的确认键 BTN_A**，把它关掉。
     实测：frame 1000 抓到的画面从「选择赛车」变成了**比赛发车格**。
  2. 油门在 frame 6000，而客机在 300 秒里只跑到 frame ~1100 ⇒ **油门永远踩不到**。
     ⇒ 现在油门在 frame 950（比赛真实位置）。
  3. 端口等的「进度」成员全程冻结、而客机自己的行驶距离在动 ⇒ 端口以为车卡住了，就一直刹车。
     ⇒ 进度信号改用**客机自己的每车行驶距离**，差值按**有符号 32 位**算（那个计数器会回绕）。
- **实测（q113/q114/q115）：** `mres=0x3f`（六个成员全解析）、`posok=1`、**`line=1`（采用了客机自己的赛车线）**、
  `lres=0.18~0.20` 米（车就压在赛道上）、**`drive=1`（转向控制器接管）**、`sawprog=1`；
  比赛连续跑了几千帧，HUD 在、位置显示第 7。
- **但车还压不住线。** 转向手性已经实测定为 `steersign=-1`（q114 时车开进了沙地，翻转后 q115 到了赛道边缘的
  铺装缓冲区、顶着轮胎墙）。⇒ **车会跑、会转，但会滑出沥青路面**；
  一旦出界，端口的「脱困」逻辑每 240 帧重复一次刹车，车就一直卡在护栏边。
  **别指望这一版能自己跑完圈**——它能进比赛、能开，但你得看着它。
- **圈数可能仍是 0**（客机自己的圈数成员还是 −1），转向手性以外的跟踪精度还没调。

---

## 7. 回传什么（要日志）

跑完后把这两个文件拷回来（纯文本）：

```
sdmc:/switch/rr3-native-rehost/rr3-native-rehost.guest-view.log   ← 主日志（最有价值）
sdmc:/switch/rr3-native-rehost/rr3-native-rehost.teardown-repair.log
```

日志里**最关键的是 `phase=race-state` 那些行**（每 30 帧一条）。请重点看：

| 字段 | 含义 | 期望值 |
|---|---|---|
| `cls=` | 客机自己的类名（实测） | `27RuleSetModifier_RaceMetrics` |
| `mres=` | 六个成员解析成功的掩码 | `0x3f` |
| `cpos=` | 车的世界坐标（1/256 米） | 非 `0,0,0` |
| `line=` | 是否采用了赛车线目标 | `1` |
| `lres=` | 赛车线拟合残差（米） | 几米量级 |
| `pur=` | 纯追踪是否接管 | `1` |
| `perr=` | 转向误差（弧度） | 非 0 |
| `laps=` | 圈数 | 最终 ≥ 1 |

另外**请顺带说三件事**：① 车有没有动；② 转向往左偏还是往右偏（或原地打转）；③ 画面卡不卡。

---

## 8. 改配置（不用重新打包）

改卡上 `sdmc:/switch/rr3-native-rehost\` 里的纯文本即可：

| 文件 | 键 | 说明 |
|---|---|---|
| `rr3-drive.txt` | `steersign=1\|-1` | 纯追踪的轴向手性。**实测为 −1**（q114 用 +1 时车开进沙地，翻成 −1 后到了赛道边缘） |
| `rr3-drive.txt` | `enable=1\|0` | 转向控制器总开关（`0` = 完全交给客机自己开） |
| `rr3-drive.txt` | `steergain=<rad>` | 纯追踪的增益（满舵对应的角度，0.50 rad ≈ 28.6°）——车压不住线时先调它 |
| `rr3-screen.txt` | `platform= / ui= / view=` | 屏幕尺寸，见下 |
| `rr3-settings.txt` | `AA_LEVEL = 4` | 抗锯齿档位（`2` = 2x，`0` = 关） |
| `../rr3-gfx.conf` | `nogo=1\|0` | 不起 socket 栈（默认 0）。**实测 `nogo=1` 会让客机的素材下载服务起不来 ⇒ 画面全黑，别改** |

**屏幕尺寸三行是三个不同的旋钮**（实测）：

- `view=` ⇒ 客机版式/视口/触摸坐标空间。**超过 1280×720 会被夹回 1280×720**（端口没有离屏目标）。
- `ui=` ⇒ **客机选哪棵 2D 图集**（图集过滤用的就是它）。`ui=2048x1152` ⇒ 客机改用 **2048×1536** 的 HD 图集；
  `ui=1280x720` ⇒ 用 960×640 那棵（**默认**，也就是「UI 不用高清」）。
- `platform=` ⇒ 平台屏幕尺寸。

⇒ **本包默认 `platform=ui=view=1280×720`**：3D 素材本来就是 HD 树（`assets_2048x1536`，每轮读 352 次），
UI 保持 960×640 ⇒ 正是「高清只给车辆和环境」。

---

## 9. 校验

`checksums.sha256` 是本包**每一个文件**的 SHA-256（相对路径，`sha256sum -c` 格式）：

```powershell
Get-Content checksums.sha256 | ForEach-Object {
  $h, $p = $_ -split '  ', 2
  $f = Join-Path $PWD $p
  if ((Get-FileHash $f -Algorithm SHA256).Hash.ToLower() -ne $h) { Write-Host "BAD $p" }
}
```

NRO 与 RomFS 的哈希可以和构建产物对照（构建是可复现的：门禁重编出来的字节完全一致）：

```
rr3-native-rehost.nro  sha256 = 6c64057fa737437195d42ac73d861535fb6eaaa5a60489cddc38cce91888d769
rr3.romfs              sha256 = 790e6241be77b7380e018f90522725ec8ae3d497ab52250f744b92da71e32dde
```

---

## 10. 手柄：PS4 / PS5 / Xbox 360 / Xbox Elite，以及「谁来开车」

### 10.1 支持哪些手柄，怎么接

| 手柄 | 怎么接 | 说明 |
|---|---|---|
| Switch Pro / Joy-Con | **原生**，直接插 | 机器自己就把它当 Npad，端口直接读 |
| Sony DS4 / DualSense (PS4/PS5) | CFW 的 **hidretro** 或 **dinput_redux** | 它们不是 Npad 设备，靠驱动翻译成 Npad |
| Xbox 360 / One / Series | 同上 | 同一个 0x20 报告 |
| Xbox 精英一代 / 二代 | 同上 | 手柄型号不影响（精英的拨杆在单独的 0x21 报告里，客机没有拨杆概念，映射也不需要） |

**为什么不是「直接读原始 HID 报告」**：这个主机的 HID 服务只暴露 Npad 状态。
本 SDK 的 `services/hid.h` 里全是 Npad 和 Palma 的调用，**没有原始报告读取器**；
`hidbus.h` 只覆盖 Joy-Con 导轨（`HidbusBusType_LeftJoyRail` / `RightJoyRail`）。
所以「读原始报告」这件事得由驱动做，端口这一层只管映射。

> 报告布局以 Linux 内核 `drivers/hid/hid-playstation.c` 为准（它每天对着真机校验），
> DS4 与 DualSense 的按钮位置**不一样**（DS4 是 `x,y,rx,ry,buttons[3],z,rz`，
> DualSense 是 `x,y,rx,ry,z,rz,seq,buttons[4]`），方向键是 HAT 而不是四个键。
> 客机侧 Xbox 的 Y 轴要取反、X 不取反。这些解码器在宿主自测里逐字节验过（189 项）。

**面键位置**（驱动按位置翻译，所以最下面的那颗永远是 A）：

```
DS4 / DualSense    × → A    ○ → B    □ → X    △ → Y
Xbox               A → A    B → B    X → X    Y → Y
```

### 10.2 映射文件在哪，怎么改

`sdmc:/switch/rr3-native-rehost/rr3-gamepad.txt`。**没有这个文件也能玩**——
端口会用内置默认，而内置默认就是实测能跑的行为。文件在的时候，
**文件对它提到的那些输入是权威的**；没提到的输入保留默认。

文件格式（`|` 分隔多个目标，和客机自己 profile 的写法一样）：

```
autopilot = 1
deadzone = 0.12

A = BTN_A
R2 = BTN_A | BTN_R2          ← 右扳机同时是油门键（实测）
LSTICK_X = AXIS_LX
RTRIGGER = AXIS_RT
```

可用的输入名：`A B X Y L1 R1 L2 R2 SELECT START HOME DPAD_UP DPAD_DOWN
DPAD_LEFT DPAD_RIGHT L3 R3` 和 `LSTICK_X LSTICK_Y RSTICK_X RSTICK_Y LTRIGGER RTRIGGER`。

可用的客机控件：`BTN_A BTN_B BTN_X BTN_Y BTN_L1 BTN_R1 BTN_L2 BTN_R2
BTN_DPAD_LEFT BTN_DPAD_RIGHT BTN_DPAD_UP BTN_DPAD_DOWN BTN_SELECT BTN_START
BTN_DPAD_CENTER` 和 `AXIS_LX AXIS_LY AXIS_RX AXIS_RY AXIS_LT AXIS_RT`。

选项：`scale=<f>`、`invert=<0|1>`、`thresh=<f>`（轴当按钮用时，多大算按下）。
`<输入> = none` 把这个输入整个去掉。名字**大小写不敏感**。

### 10.3 「谁来开车」——这是最要紧的一条

```
autopilot = 1        端口自己开（实测能让车跑起来的模式）
autopilot = 0        你开。端口完全不碰油门、刹车、转向
autopilot = assist   你开，端口只在你没碰的那个控制上补位
```

> **要自己跑完整一圈 ⇒ `autopilot = 0`。**
> 这是「端口闭环会和你抢方向盘」的开关。

### 10.4 常用配方

```ini
# 右扳机当油门（任何手柄）
R2 = none
RTRIGGER = BTN_A | AXIS_RT thresh=0.2

# 方向盘反了
LSTICK_X = AXIS_LX scale=-1

# 方向盘挪到右摇杆
LSTICK_X = none
RSTICK_X = AXIS_LX
```

### 10.5 怎么确认手柄被认出来了

端口每 300 帧往 `sdmc:/switch/rr3-native-rehost.guest-view.log` 写两行：

* `gamepad-map` —— 它在这个文件里找到了什么（或者根本没找到这个文件）；
* `gamepad-state` —— 这一帧的标准输入状态和推给客机的状态。

在 PC 上读（**不要用 `Get-Content`**，UTF-8 中文会乱码）：

```powershell
$p = 'F:\switch\rr3-native-rehost.guest-view.log'
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'gamepad-map|gamepad-state'
```

* `gamepad-map ... file=absent` ⇒ 端口**看不到**这个文件，跑的是内置默认。
  多半是路径不对：必须是 `switch\rr3-native-rehost\rr3-gamepad.txt`。
* `gamepad-map ... parsed=0 error_line=N` ⇒ 这个文件第 N 行有问题，端口**拒绝**了它
  并继续用默认。这是故意的：半张映射表比没有映射表更糟。
* `gamepad-map ... TRUNCATED` ⇒ 文件太大，缓冲区装不下（正常不会发生）。
* `gamepad-state ... buttons=0x... guest_buttons=0x...` ⇒ 端口看到的状态和你推的一致。
  `guest_buttons` 的 bit 0 就是客机的 `BTN_A`，也就是油门。

> **注意**：`rr3-pad.txt`（脚本手柄）在的时候，物理手柄这条路**完全不会被走到**，
> 所以 `gamepad-state` 不会有输出。调试手柄时先把 `rr3-pad.txt` 挪走。
