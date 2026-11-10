"""Rewrite README-hardware-test.md sections 10.9 and 10.10, and add 10.11.

WHY: the old 10.9/10.10 carried numbers from an earlier round and one claim
that has since been MEASURED FALSE:

    "资源解析缓存：客机反复要同一个资源，现在直接命中缓存（fs=0 ms）"

The cache's counters are 0 hits and 0 misses in every archived round while
the filesystem service burned 99-120 seconds of a 400-second round, so the
cache is on the boot-time list builder and not on the hot path.  A document
that says the cache works is a document that says a lie, and this one is the
one the user reads.

Also: the census line no longer has the phantom `fhit=` field.
"""
import io
import sys

MARK = "## 10.9 帧率：怎么在真机上量，以及现在卡在哪"

NEW = """## 10.9 帧率：怎么在真机上量，以及现在卡在哪

**先说结论（2026-11-10，Eden 实测，10 轮归档）**：**端口自己几乎不占帧时间**。
帧成本几乎全在客机自己的渲染上。

帧的三个成本，普查行里都有（`tt` = 整帧，`tr` = 客机自己的渲染调用，
`ts` = 交换/呈现，`tp` = 端口自己的开销 = `tt - tr - ts`）：

| 字段 | 含义 | Eden 实测（中位数） |
|---|---|---|
| `tt` | 整帧 | 渲染段 **40~66 ms**；加载段 168~183 ms |
| `tr` | 客机自己的渲染调用 | **37~40 ms**（占大头） |
| `ts` | 交换/呈现 | 0 ms |
| `tp` | **端口自己在帧循环里干的活** | **0 ms（每一轮、每一个阶段都是 0）** |
| `jni` | 端口经 JNI 伺候客机的时间 | 7~11 ms（其中大部分是客机自己的 Java 方法体） |
| `fs` | 端口做资源解析/文件服务的时间 | 渲染段 0 ms；加载段 **229~350 ms** |
| `fsr` | 其中属于**路径解析**的部分（新） | 见日志，用来分辨「路由慢」还是「I/O 慢」 |
| `fshit` / `fsmiss` | 资源解析缓存的命中/未命中 | 见日志（注意：**只有这两个**，没有 `fhit=`） |

**⇒ 端口侧的 60 帧工作已经做完了**：`tp` 的中位数是 0 ms，端口没有可再省的地方。
⇒ 剩下的全是客机自己的栅格化 ⇒ 想更快，动的是**客机的画质**，不是端口（见 10.10）。

**⇒ 加载阶段另有一笔账**：整个文件系统服务在 400 秒的一轮里花了
**99~120 秒**，而资源解析缓存的计数器**一次都没动过**（命中 0 / 未命中 0）。
缓存挂在开机时用的清单构建函数上，而客机真正走的是另一条路径。
⇒ 所以普查行加了 `fsr=`，下一轮就能分辨这 99~120 秒到底是**路由**还是**读盘**。

**在真机上怎么量**（跟第 7 节同一条命令）：

```powershell
$p = 'F:\\switch\\rr3-native-rehost.guest-view.log'
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'guest-frame-census' | Select-Object -First 20
```

**只读比赛段**（前 2500 帧是加载/前端，帧时间是几十到几百毫秒，混进来会误导）：

```powershell
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'guest-frame-census' |
    Where-Object { $_ -match 'frame=(\\d+)' -and [int]$Matches[1] -gt 2500 } |
    Select-Object -First 20
```

判读：

- `tt` 中位数 ≈ 17 ms ⇒ 60 fps；≈ 40 ms ⇒ 25 fps。
- **`tp` 大** ⇒ 端口在拖后腿（**这条线才是要修的**）；**`tp` = 0 而 `tr` 大** ⇒ 客机自己慢，
  端口无能为力，只能动画质（10.10）。
- `tt` 里有几十秒的巨帧 ⇒ 客机自己的云端/登录等待，**不是端口的问题**。

**新开关（`rr3-gfx.conf`）**：

| 键 | 默认 | 说明 |
|---|---|---|
| `percall=1\\|0` | **0（关）** | 逐调用日志。**这是诊断开关，出货默认就是关的**。开着时每帧往卡上写几十行，测的是日志开销而不是游戏本身 |

**帧率改动已经落地的两条**：

1. **配速**：原来干完活固定睡 16.67 ms（周期 = 活儿 + 整帧，活儿被罚两遍），
   现在只睡预算剩余 ⇒ 周期 = max(活儿, 16.67 ms)。
2. **每帧显式让出 2 ms**：日志写卡在等的时候 CPU 是闲的，客机的其他线程就靠这段
   空窗跑 ⇒ 把让出变成显式行为，日志就可以批量落盘（每 64 行一次）。

## 10.10 60 帧：现在能动帧时间的是**游戏自己的画质档**

**先说结论（2026-11-10 实测）**：端口自己的开销 `tp` 中位数是 **0 ms**，
帧成本全在客机自己的栅格化（`tr` = 37~40 ms）。
**⇒ 想再快，动的是客机的画质，不是端口。**

**⇒ 而且在 Eden 上，客机侧的三根杠杆都量过，全都不成立**（同一个二进制、
同一张卡、一次只改一个变量）：

| 变量 | 结果 |
|---|---|
| `AA_LEVEL` 4 → 0 | 72 → 83 ms（**反了**：关掉抗锯齿反而更慢 ⇒ 这是噪声，不是效果） |
| 12 个客机高开销画质项全关 | 72 → 70 ms（3 %，在轮间方差以内） |
| 分辨率 1280×720 → 960×640 | 72 → 77 ms（同样反了 ⇒ 也是噪声） |

**⇒ ⇒ 这三个结果说明：Eden 上的帧时间被模拟器本身主导，客机画画的代价在噪声底下。
⇒ ⇒ ⇒ 所以画质档这条梯子只能在真机上走，不是在 Eden 上走。**

**卡上现在的设置是 `AA_LEVEL = 4`（4 倍抗锯齿）**：

```
sdmc:/switch/rr3-native-rehost/rr3-settings.txt
AA_LEVEL = 4        ← 改成 0 就是关掉抗锯齿
```

**⇒ 抗锯齿是每像素 4 次采样 ⇒ 它直接乘在客机每帧的栅格化开销上。**
真机上这是唯一一个「改一行就可能把帧时间砍掉一大截」的旋钮。

**画质档（整档）**：`rr3-settings.txt` 里 `calidad = ` 可选：

| 值 | 含义 |
|---|---|
| `bajo` | AndroidLow |
| `medio` | AndroidMid |
| `medioalto` | AndroidMid2 |
| `alto` | AndroidHigh（玩家默认那档） |
| `xtralto` | AndroidXtraHigh（HD 档：2048x1536 图集 + AA + 字体页 2048） |
| `personal` | 保留玩家自己的档案，只应用下面逐项写的覆盖 |

**逐项覆盖**（可以只改一项）：`AA_LEVEL`、图集（`GUI_LAYOUT_EXTENSION` /
`EXPECTED_SCREEN`）、字体页大小（`FONT_PAGE_SIZE`）、`MIPMAP_BIAS`、各类阴影开关。

**怎么改**（不要用记事本另存，编码会坏）：用 PowerShell 脚本，只改那一行：

```powershell
powershell -ExecutionPolicy Bypass -File .build\\set-guest-setting.ps1 -Key AA_LEVEL -Value 0
powershell -ExecutionPolicy Bypass -File .build\\set-guest-setting.ps1 -Key calidad -Value medio
```

**怎么确认有没有变快**（真机上的量法，仪器已经在卡上了）：比赛段的 `tt` 就是整帧时间。
`tt` 的中位数降到 16~17 ms 就是 60 fps。在 `rr3-native-rehost.guest-view.log` 里：

```powershell
$p = 'F:\\switch\\rr3-native-rehost.guest-view.log'
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'guest-frame-census' |
    Select-Object -Last 5
```

那一行里 `tt=` 是整帧、`tp=` 是**端口自己的开销**、`jni=` 是端口经 JNI 的开销、
`fs=`/`fsr=` 是文件服务/其中的路径解析、`gui=`/`twosq=` 是前端普查的**端口自带计数器**
（不要再用正则去数日志里的字符串了 —— 那是上一版的做法，会让「改日志」变成「改测量」）。

**Eden 上的天花板**：模拟器的 GL 路径本身就慢，真机走 NVN 会快不少。
**⇒ 60 帧是「真机 + 关抗锯齿（或降一档画质）」的目标，Eden 上量到的数字只是参考。**

## 10.11 真机崩溃：现在日志里会多出三样东西，请一起传回来

**2026-11-10 的真机崩溃（`esr=0x92000005`）是一次 SPAL —— 指针认证失败。**
上一版文档把它记成「数据访问异常」，那是错的：异常类别在 ESR 的高位，
`(esr >> 26) & 0x3f` = **0x24 = SPAL**。

**⇒ 已经装了三个新仪器，下一次真机跑完请把这两个文件传回来：**

| 文件 | 里面会多出什么 |
|---|---|
| `rr3-native-rehost.guest-view.log` | `phase=loader-diag ...` 行：每个客机 DSO 映射前的名字/段/偏移/权限，外加 **`pac-census` 行**（每个 DSO 的 PAC/AUT 指令条数） |
| `rr3-native-rehost.crash-early.txt` | **`ec=0x..`**（解出来的异常类别）和 **`pc_attribution=` / `lr_attribution=`**（出错的 pc 属于哪个 DSO、偏移多少，或者是 `port-nro+0x...`、或者是 `unmapped`） |

**⇒ 这三行合起来就能一句话说清**：崩溃在端口的代码里，还是在客机的代码里；
客机的 DSO 到底带不带指针认证。

**为什么这次要改口径**：上一版说「崩溃在端口自己的代码里、在七 DSO 装载器里」。
**这个结论被推翻了**：端口的 NRO 全库只有 **2 条** PAC/AUT 指令（都在 libunwind 里），
而崩溃 pc 反推出来的 NRO 基址**不是页对齐**（NRO 基址必须是页对齐），
所以 **pc 不在端口镜像里**；反过来，客机的 `libc++_shared.so` 带着
**1328 条 `paciasp` + 1372 条 `autiasp`**，是客机自己的代码在做指针认证。
**⇒ ⇒ 「日志最后一行是 `gl-ctx-early`，下一句是 `loadLinkElfSet()`」也推不出结论**，
因为日志是**追加**写的，最后一行可能是上一轮的尾巴。

**⇒ ⇒ 装载器的诊断现在每一行都**立即落盘**（日志平时是 64 行批量落盘，
崩溃时会把装载器自己的证据一起带走 —— 这正是上一次什么都没留下的原因）。
"""


def main():
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    index = text.find(MARK)
    if index < 0:
        print("10.9 not found")
        return 1
    head = text[:index].rstrip("\n")
    with io.open(path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(head + "\n" + NEW)
    print("rewrote 10.9 / 10.10 and added 10.11")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
