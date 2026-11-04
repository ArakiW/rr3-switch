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

—— 那三个本该是**游戏的货币图标**。

**实测出来的真实机制**（2026-11-02，从客机自己的日志统计）：客机每轮会向端口
请求 `r3_symbols.ttf` **18 次**，而且**每一次都是单个字符**的绘制。那 18 个字符
不是字母，是**游戏的图标美术**：`U+0029` 是奖杯、`U+002C` 是一摞钞票、
`U+0111` 是游戏自己的 logo、`U+0056` 是减震器。

⇒ 端口以前把 `r3_symbols.ttf` 换成正文字体 `ConduitITCPro-Regular`，
于是**每一个图标都被查成了字母**：`U+00CB` 画成「带重音的 K」、`U+0111` 画成
「带横的 d」、`U+0056` 画成字母 `V`。
**而且同时小了 2.3～5.8 倍**——图标美术宽 1.00～6.22 em，被替换成的字母只有
0.21～0.52 em。⇒ 「缺少字符」和「图标大小不对」是**同一个 bug 的两面**。

⇒ 现在 `r3_symbols.ttf` **用它自己**（放进 NRO 的 romfs）。

**注意有两个同名文件，而且不是同一个字体**：

| 来源 | 大小 | 说明 |
|---|---|---|
| `rr3core:/files/.depot/r3_symbols.ttf` | **125,764 B** | 客机真正映射的那份 ✔ |
| `sdmc:/…/.depot/r3_symbols.ttf` | 72,468 B | 卡上的散落副本，**是另一个字体** ✘ |

两者在若干码位上的字形不一样。所以构建脚本**按大小校验**：只有 125,764 B 那份
才会进 romfs，72,468 B 那份会被拒绝并打印出来——不然错图标看起来就像 UI 坏了，
极难往文件上追。

顺带更正一个上一版写错、这次被实测推翻的结论：

* ❌ 旧结论：「客机用 `r3_symbols.ttf` 排整段英文，所以端口把文字画大了 2.048 倍
  （upem 2048 vs 1000）」。
* ✅ 实测：那 2.048 是 **upem 之比**，跟段落字号无关。客机的段落（tips 段
  `If you want to race in the real world…`）用的是
  `ConduitITCPro-Medium.otf`，**原样打开、一比一排版**；一轮 2,812 次正文绘制
  **没有任何一次被替换**。**正文字号本来就是对的。**
* 旧结论错在哪：那次的判读窗口只有 6 行日志，**越界读到了下一个请求**，
  把下一段的字体安到了上一段头上。

同时修掉的两个「仪器坏了」：

1. `glyph-face` 日志里的 `substituted=` 字段以前**恒为 1**（它比较的是客机的
   卡路径和 romfs 路径，而这两个永远不同）。现在按**文件名**比较，
   客机点名的字体就是 `substituted=0`。
2. 字体缓存 `kFaceSlots` 以前只有 **6** 个槽位，而客机要 14 个字体 + 中文兜底 +
   图标字体 = 16 个。**少一个槽位不会报错**：`openFace()` 返回 `nullptr`，
   文字被悄悄改用 Conduit 画——UI 安静地丢掉客机点名的那个字体。已改成 16。

### 0.4 打开图标字体以后客机卡死了：客机的 `sscanf` 没有界

这不是「图标字体不好」，是**它把一条老坑踩出来了**。

**实测过程**（同一套二进制、同一张卡，只改 `rr3-gfx.conf` 里一个键）：

| 轮次 | 配置 | 结果 |
|---|---|---|
| q123 | `iconfont=1`（开图标字体） | **卡死**：日志停在 1.62 MB，4 帧，120 秒没有新帧 |
| q123b | `iconfont=0`（旧行为） | 正常：3000 帧，20 万行日志，跑满 305 秒 |

⇒ 定位卡在哪：端口的采样器报 `pc_img=host-nro pc_off=0x5ea1c0`，把偏移拿
`nm` 解析到宿主 NRO 的符号表 ⇒ **`__svfscanf_r`**（newlib 的 `sscanf`），
两次采样都在里面。也就是说**客机线程卡在宿主 libc 的 `sscanf` 里**。

⇒ 端口的 `sscanf` 以前是 `RR3_BIND(sscanf)`，**直绑宿主 newlib**，读客机指针
**一点界都没有**。这和 q89 卡在 `_strtoul_l` 是同一个形状（那次已经给
`strtol/strtoul/strtod` 都加了有界副本，唯独漏了 `sscanf`）。

⇒ 加界之后，端口把**输入串和格式串**都按可读性检查拷出来（上限 512 字节），
宿主解析器只看得见副本。日志里 1005 条记录把现场留下来了：

```
phase=guest-scanf-bounded ... input=truncated in_len=512 fmt=ok fmt_len=16
   in_head=4646545F464F4E545F4249474E554D424552533C3139350D0A4646545F ...
   fmt_text=[%[A-Z0-9_],%d]
```

⇒ 那段输入是客机的**字体/着色器定义文件**（`#SEC:MAX_IMAGES:24`、
`FFT_FONT_BIGNUMBERS,195`…），格式是**扫描集** `%[A-Z0-9_]`。
`in_len=512` 说明**那 512 字节里没有 NUL** ⇒ 宿主 newlib 的扫描集在
**没有任何终止符**的内存上一路匹配，直到扫到堆尾——这就是那 120 秒。

⇒ 有界副本让扫描最多走 512 字节，而且**答案不变**：token 本身远短于窗口，
副本解析出的值和原来一致（和 `guest_strto` 的同一条原则：**加界不能改答案**）。

⇒ 另外加了一个 `iconfont=0/1` 开关（`rr3-gfx.conf`），默认 **1**，
需要对照旧行为时随时可以切回去。

### 0.4 你要做的

**如果卡上已经有数据卷**：下 `rr3-switch-app-20261102.zip`（45.3 MiB，一个文件），
解包，把 `switch\` 里的东西覆盖到卡根即可。**不要删 `switch/rr3-gfx.conf`。**

**如果是新卡**：用下面的分卷整包。

### 0.5 19:45 那次真机崩溃，和它牵出来的两个老坑

**崩溃现场**（`rr3-native-rehost.crash-000001a77b06589b.txt`）：

```
far=0x28   esr=0x92000005   (data abort, translation fault, level 1)
pc=0x1837ed7370   ← 在客机镜像里
lr=0x87d40de54    ← 宿主：deliverServerTime
```

⇒ **客机解引用了 NULL+0x28**；宿主这边用 `addr2line` 解析 `lr` 落在
`cc_http_request.cpp` 的 `deliverServerTime` —— 也就是**把 `/time` 应答交给客机**的那一步。
客机自己的日志最后一行是 `phase=cc-probe … worker unreadable`：
**端口明知 worker 读不出来，还是把指针交给了客机。**

**坑一：端口的可读性判定把真机的客机堆判成了不可读。**

| | 真机 | Eden |
|---|---|---|
| 客机镜像 bias | 0x18361F3000 | 0x2106880000 |
| 客机 worker 对象 | 0x562EA7C4C0 | 0x2113D42280 |
| 距镜像上方 | **259 GiB** | 122 MB |

`guestReadable()` 原来只接受 `[bias − 4 GiB, bias + 4 GiB)` ⇒ 真机上**完全合法的客机对象**
被判成「不可读」。而宿主自己的地址在镜像**下方**（0x87e1147b0 / 0x6d20958940）、
客机堆在**上方** ⇒ 对称窗口分不开两者，**必须改成非对称**：
`[bias − 4 GiB, bias + 512 GiB)`。

**坑二：客机的 `sscanf` 没有长度界。** 端口把客机的 `sscanf` 直绑到宿主 newlib
（`RR3_BIND(sscanf)`），而客机把**字体/着色器定义文件**交给一个**扫描集**格式
`%[A-Z0-9_]`，那段内存**前 512 字节没有 NUL** ⇒ 扫描集一路匹配到堆尾。
这和 q89 卡在 `_strtoul_l` 是**同一个形状**，只是换了个函数。
现在输入串和格式串都先拷出来（可读性检查 + 上限），宿主解析器只看得见副本：
**答案不变，只是扫描有界**；而且日志行写在**解析之前**（挂在里面的那次调用必须先落日志）。

**另外**：7 处「把 `req->callback` 交给客机」的地方现在都先过 `guestWorkerAlive()`，
客机已经释放的 worker 不会再被交回去 —— 这种情况会记成 `cc-worker-dead`（不再崩）。

**这次还加了三样仪器**，前四次真机崩溃都是它们找出来的：
`boot-step`（开机步骤面包屑）、`elf-dry-step`（ELF 干加载面包屑）、
`sdmc:/switch/rr3-native-rehost.crash-early.txt`（早期崩溃的寄存器转储）。
详见 `docs/HANDOFF-2026-11-03.md`。

**这次的构建 `249c9f02…`**，模拟器实测 q132：306 秒 / 1560 帧 / 28 万行日志 / 0 崩溃，
`Server time returned` ×1、`cc-worker-dead` ×0、`glyph-tex-fail` ×0。

**你需要做的**：装新 NRO（`dist/rr3-switch-app-20261103.zip`，45.4 MiB）→ 开机 →
回传 `guest-view.log`、`crash-early.txt`（如果有）、以及屏幕上看到什么。
**务必留下和 NRO 同一批的 `.elf`** —— 崩溃的 PC 只能在**完全相同的那次构建**上解析。

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

- **这一版把车交给你开了（2026-11-04）。** 之前每一轮测试都是**端口自己的自动驾驶**在开车。
  你说「你们从来没让我实际接手操控」——查下来比这更糟，有三个缺陷叠在一起：
  1. `autopilot = 0` **只管「物理手柄」那条路**；而脚本手柄那条路被端口的闭环**无条件覆写**
     （`scripted.leftX = drive.steer`），所以**那个开关根本没切到东西**。
     实测（q148 对照轮：卡上设成 `autopilot = 0`，代码还是旧的）：自动驾驶照样开车，
     201/208 个采样都在驱动，只跑到一圈的 8.96%。
  2. **出货包带着实验用的 `rr3-pad.txt`**，而脚本一旦激活，**物理手柄整条路完全不会被走到**
     ⇒ **在主机上你的摇杆是死的**；而且脚本从 frame 600 起一直按着油门。
  3. 脚本那条路**一个仪表都没有**，所以「输入到底到没到车」这个问题从日志里根本答不出来。
  ⇒ 三条都修了：模式统一管两条路；**脚本在真人动杆时自动让位**；脚本路补上自己的仪表
  （`scripted-state`）。实测（q149，24 个连续采样全部一致）：
  `lx_script == lx_guest`、`loop_steered=0`，车速 1750→6464，步长在 1.5 m / 30 m 之间交替
  ⇒ **你的输入端到端到达了车**。
  ⇒ **出货默认现在是 `autopilot = 0`（你开）**，`rr3-pad.txt` 不再有任何事件行
  （它按住油门，会把油门从你手里抢走）。
- **进比赛**：之前「车一直不动」有三个原因，都已定位并修掉：
  1. 客机自己的**比赛内教程对话框**不吃端口的合成触摸（客机确实收到了，坐标也对，但它不响应）。
     ⇒ 现在 `rr3-pad.txt` 在 frame 600 起**按住客机的确认键 BTN_A**，把它关掉。
     实测：frame 1000 抓到的画面从「选择赛车」变成了**比赛发车格**。
     ⇒ **2026-11-04 起这条改由你自己按 A 完成**：出货的 `rr3-pad.txt` 不再有任何事件行。
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
  **别指望自动驾驶能自己跑完圈**（2026-11-04 实测：最好跑到一圈的 10.35%）：
  它跟的是**赛道中心线**，而这条赛道自己的赛车线（`ai_spline`）比中心线短 67 m/圈、
  差 2.92~8.25 m ⇒ **车在弯道外侧跑**，每场撞护栏约 10 次。
  ⇒ **这是自动驾驶（attract mode）的质量问题，不是移植缺陷**：你自己开，圈数就会正常计。
- **圈数在你手里是正常的**（客机自己的圈数成员之前一直是 −1，只因为那时一直是自动驾驶在开）。

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
| `rres=` | 车离赛道**自己的赛车线**（`ai_spline`）有多远（米） | 开着玩时应该不大 |
| `pur=` | 纯追踪是否接管 | `1`（`0` = 你在开） |
| `perr=` | 转向误差（弧度） | 非 0 |
| `laps=` | 圈数 | **你开 ⇒ 最终 ≥ 1** |

### 7.1 黑屏归因：`guest-frame-census` 行（**这一版新增**）

端口每帧往主日志写一行 `phase=guest-frame-census`，这一行现在带着**这一帧的时间账**：

| 字段 | 含义 |
|---|---|
| `tt=` | 整帧耗时（毫秒） |
| `tr=` | 其中**客机自己渲染**占了多少 |
| `tp=` | 其中**端口自身**占了多少 |
| `ts=` | 其中 `eglSwapBuffers`（呈现）占了多少 |

⇒ **Eden 上的测量结论**（2026-11-04）：端口自身只占 **0.1%**（菜单）/ **1.0%**（比赛），
其余全是客机自己的 ARM64 代码在**被模拟执行**。所以模拟器上的帧率**不代表主机**，
主机的黑屏更可能是**启动阶段**（端口能控的那部分）。

⇒ **在主机上直接读这三行，就能知道你的黑屏是客机自己的活还是端口的**：

```powershell
$p = 'F:\switch\rr3-native-rehost.guest-view.log'
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'guest-frame-census' | Select-Object -First 20
```

如果某一帧的 `tt` 是几十秒而 `tp` 还是 0~1 毫秒 ⇒ **是客机自己在加载/解析资源**，
端口没拖后腿；如果 `tp` 也是几十秒 ⇒ **端口在拖后腿**，那就有的查了。

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
autopilot = 0        你开。端口完全不碰油门、刹车、转向（**出货默认**）
autopilot = 1        端口自己开（attract mode；它自己跑不完一圈，见 §6）
autopilot = assist   你开，端口只在你没碰的那个控制上补位
```

> **出货默认是 `autopilot = 0`，也就是车是你的。**
> 以前默认是 1，而且**那个开关当时根本没切到东西**：它只管「物理手柄」那条路，
> 而脚本手柄那条路被端口的闭环无条件覆写（2026-11-04 实测，见 §6）。
> 现在两条路都由这个模式管，而且**脚本一旦检测到你在动手柄就自动让位**。

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

> **2026-11-04 起不用再挪 `rr3-pad.txt` 了。**
> 以前脚本手柄一旦激活，物理手柄那条路**完全不会被走到**（所以 `gamepad-state` 没输出），
> 调试手柄得先把 `rr3-pad.txt` 挪走。**现在脚本会让位给真人**：
> 你一动杆或一按键，脚本立刻停手，物理手柄接管。
> 而出货的 `rr3-pad.txt` **没有任何事件行**，所以它根本不会激活。
> ⇒ **看到 `gamepad-state` 就说明手柄被认出来了；它上面那行 `guest_buttons`/`guest_lx`
> 就是客机真正收到的值。**
