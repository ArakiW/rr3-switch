# RR3 Native Rehost 0.22.4 — 实机（Nintendo Switch）完整测试包

《真实赛车 3》安卓客机 → Nintendo Switch 原生移植（libnx + Mesa / nouveau / NVN）。
对应仓库提交 **850a46d5**（`origin/master` 已同步）。

**这个包是自包含的**：应用、控制脚本、7 个数据卷、散落素材树 `.depot`、端口的实机配置
`sdmc:/switch/rr3-gfx.conf`，全都在里面，不需要再去别的卡上找。

---

## 1. 装到卡上

### 方式 A：分卷 zip（推荐，脚本全自动）

下载**全部 4 个分卷** `rr3-switch-fulltest-20261102.zip.001` … `.004`，加上
`Join-And-Extract.ps1`，放进**同一个文件夹**，然后：

```powershell
powershell -ExecutionPolicy Bypass -File Join-And-Extract.ps1 -Card F:
```

它会：把分卷拼成一个 zip → 解包到 `.\rr3-package` → 调用包里的 `Copy-To-SwitchCard.ps1`
把整棵树装到 SD 卡。`F:` 换成你的卡。

> 分卷必须**一个不缺**。脚本会把找到的分卷按文件名排序拼接，少一个就会得到一个坏 zip。

### 方式 B：直接拷目录

把 `switch\` 整个目录复制到 SD 卡根目录：

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
    ├── data/                               7 个 *.romfs 数据卷
    ├── .depot/                             散落素材树
    └── cache/ apk/ doc/ proc/ save/
```

启动器标题栏显示的就是 `switch\rr3-native-rehost` 这个路径。

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

- **比赛现在能开始了（2026-11-02 实测）。** 之前「车一直不动」有两个原因，都已定位并修掉：
  1. 客机自己的**比赛内教程对话框**不吃端口的合成触摸（客机确实收到了，坐标也对，但它不响应）。
     ⇒ 现在 `rr3-pad.txt` 在 frame 600 起**按住客机的确认键 BTN_A**，把它关掉。
     实测：frame 1000 抓到的画面从「选择赛车」变成了**比赛发车格**。
  2. 油门在 frame 6000，而客机在 300 秒里只跑到 frame ~1100 ⇒ **油门永远踩不到**。
     ⇒ 现在油门在 frame 950（比赛真实位置）。
  实测（q111）：`mres=0x3f`（六个成员首次全部解析）、`posok=1`、**`lapd=1593344`（圈距为正 ⇒ 车真的在动）**，
  抓到的帧是：1200 发车格、1400 赛道上的角色教学（背景是模糊的赛道）、**1800 比赛开场动画**。
- **但方向还是没人管。** 端口的纯追踪控制器要等「观察到进度」才接管（`sawprog=1` ⇒ `drive=1` ⇒ `pur=1`），
  这一版还没点亮 ⇒ **车是自己在直着走（油门+确认键），没有转向**。别指望它能自己跑完圈。
- **转向手性仍未实测。** 纯追踪的轴向手性（`rr3-drive.txt` 里的 `steersign`）**从未实测过**——
  端口读到的车坐标 Y 是取反的，所以大概率要 `steersign = -1`。
  改这个值**不需要重打包**，改卡上的 `rr3-drive.txt` 一行即可。
- **圈数可能仍是 0。** 端口刚把「读到活着的 metrics 对象」这一层修好，链还没完全打通。

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
| `rr3-drive.txt` | `steersign=1\|-1` | 纯追踪的轴向手性 ⇒ 方向反了改 `-1` |
| `rr3-drive.txt` | `enable=1\|0` | 转向控制器总开关（`0` = 完全交给客机自己开） |
| `rr3-screen.txt` | `platform= / ui= / view=` | 屏幕尺寸，见下 |
| `rr3-settings.txt` | `AA_LEVEL = 4` | 抗锯齿档位（`2` = 2x，`0` = 关） |
| `../rr3-gfx.conf` | `nogo=1\|0` | 不起 socket 栈（默认 1）。真机若在联网状态卡在启动，把 Wi-Fi 关掉再试 |

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
