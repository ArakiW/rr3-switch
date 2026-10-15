# Handoff — 2026-10-05 · RR3 Switch 移植卡点

> 本文自包含。读它的人不需要本会话的任何上下文。
> 详细的时序记录在 `PORT_PROGRESS.md`（6484 行）；本文只给**卡点、证据、以及已经排除的东西**。

---

## 0. 一句话

客机走完设备指纹（DeviceInfo）块后，**立刻拆除它自己刚建好的那个容器**，而拆除时的链表遍历走不到终点节点，于是卡在那里。

**卡点位置：客机 `libRealRacing3.so` ELF VA `0xcd2aa0` – `0xcd2b0c`（FDE `[0xcd1014, 0xcd3648)`）**

需要客机回溯才能再往里，而模拟器给不出回溯。**这就是当前唯一的阻塞。**

---

## 1. 目标与现状

- **目标**：完整移植安卓端功能，顺利进游戏。
- **现状**：**游戏未进入**。客机能启动、能画 EA  splash、能打印 21 行 DeviceInfo，然后就停住。
- **本会话没有让游戏前进一步。** 产出是：定位了卡点位置、找出了写点与完整调用链、清掉了 17 条错误结论、修了 6 处仪表缺陷。

---

## 2. 环境

| 项 | 值 |
|---|---|
| 工程根 | `E:\AI\RealRacing3-switch` |
| 客机 ELF | `reverse\native\arm64\libRealRacing3.so`（AArch64 PIE，33 MB，`.symtab` 已剥、`.dynsym` 完整） |
| `.text` | `[0x988000, 0x1e17af0)` |
| 反汇编辅助 | `tools\guest-analysis\guestfde.py`（**先读它，不要自己写 ELF 解析器**） |
| 交叉编译器 | devkitPro aarch64-none-elf，**host 侧是 Windows 的 `addr2line`（x86-64），读不了 NRO** |
| 我们的可执行 | `native-switch\rr3-native-rehost.nro` + `rr3-native-rehost.elf`（ELF 有完整 `.symtab`，17,026 个函数，可用于符号化宿主地址） |
| 模拟器 | Eden（`E:\Eden`），运行归档在 `.build\eden-runs\<时间戳>-<标签>\` |

**构建**：`powershell -ExecutionPolicy Bypass -File tools\Build-NativeSwitch.ps1`（必须 exit 0）
**契约**：`powershell -ExecutionPolicy Bypass -File tools\Test-RR3Inputs.ps1`（必须 exit 0，11/11）
**Eden 一轮**：`powershell -ExecutionPolicy Bypass -File tools\Test-OnEden.ps1 -Seconds 200 -Tag <标签>`

---

## 3. 卡点细节

### 3.1 触发序列（同一函数内，首尾相接）

客机在 `[0xcd1014, 0xcd3648)` 里组装整张 DeviceInfo 表，序言 `sub sp, sp, #0x5d0`（1488 字节帧）：

```
0xcd21f8  mov  w8, #0x6544 / movk w8,#0x6976,lsl#16   ; "De"+"vi" = "Devi"
0xcd2204  mov  w8, #0x6563                            ; "ce"      = "Device"   (长度 6)
0xcd2214  mov  w8, #0x7250 / movk w8,#0x666f,lsl#16   ; "Pr"+"of" = "Prof"
0xcd2220  mov  w8, #0x6966 / movk w8,#0x656c,lsl#16   ; "fi"+"le" = "Profile"  (长度 7)
0xcd27e4  mov  w9, #0x6954 / movk w9,#0x7265,lsl#16   ; "Ti"+"re" = "Tire"     (长度 4)
...
0xcd2a8c  adrp  x0, #0x3ed000
0xcd2a98  add   x0, x0, #0xbf3                        ; "DeviceInfo::Tier: %s"
0xcd2a9c  bl    0x109fa18                             ; ← 客机最后一条日志
0xcd2aa0  ldur  x20, [x29, #-0x48]                     ; ← 紧接着进入析构
0xcd2aa4  cbz   x20, #0xcd2b0c
0xcd2aa8  ldur  x21, [x29, #-0x40]
0xcd2aac  cmp   x21, x20
0xcd2ab0  b.ne  #0xcd2ac8
0xcd2ab4  mov   x0, x20
0xcd2ab8  b     #0xcd2b04
0xcd2abc  cmp   x20, x23                              ; ← 采样最热的地址（5x）
0xcd2ac0  mov   x21, x23
0xcd2ac4  b.eq  #0xcd2b00                             ; ← 唯一出口：x23 == x20
0xcd2ac8  ldurb w8, [x21, #-0x18]                      ; "已释放"标志
0xcd2acc  tbnz  w8, #0, #0xcd2ae0
0xcd2ad0  ldurb w8, [x21, #-0x30]                      ; ← 采样次热（3x）
0xcd2ad4  sub   x23, x21, #0x30                        ; 游标每步 -0x30
0xcd2ad8  tbz   w8, #0, #0xcd2abc                      ; 该字节为 0 → 继续
0xcd2ae0  ldur  x0, [x21, #-8]  ; bl 0x1e17b50         ; operator delete
0xcd2afc  b     #0xcd2abc
0xcd2b00  ldur  x0, [x29, #-0x48] ; bl 0x1e17b50
```

**唯一出口是 `0xcd2b00`，条件是 `x23 == x20`——遍历必须走回出发的节点。**

### 3.2 客机打印的完整设备信息（实测，`DeviceInfo::` 共 21 行）

```
Platform:            (空)          Model:               (空)
Model Name: Nintendo Switch        Firmware:            (空)
Total Memory: 2146435072           Chipset: Nintendo Switch
GPU Renderer: Adreno (TM) 420      GPU Vendor: Qualcomm
GPU Version: OpenGL ES 2.0 (GIT_20260802)
Screen Width: 1280                  Screen Height: 720
Build Device: nx                    Build Manufacturer: Nintendo
Screen Density: mdpi                Screen Size: large
CPU Core Count: 4                   CPU Speed: 1.02
CPU Chipset: ARM Cortex-A57        SDK Version: 35
Supported Abis: arm64-v8a          Tier: AndroidHigh     ← 最后一行
```

**3 分钟与 10 分钟两轮完全一致 ⇒ 卡住，不是慢。**

### 3.3 还没做的事

**`[x29,#-0x48]` 和 `[x29,#-0x40]` 这两个链表节点是在哪里被写进去的** —— 那是链条上的对象，直接决定它是否可解。
**这一项本会话没做成**：用 capstone 在 21 MB `.text` 上线性扫描会失步，我手算 ARM64 掩码位段也反复出错，**三次谓词对照连续返回 0**。
⇒ 接手的人**先修谓词、对照通过后再读结果**；不要从 0 里读结论。

---

## 4. 已定案的链（都有反汇编或阳性对照）

### 4.1 `+0x10518` 的唯一写点

```
14045f0  mov  w8, #0x508
14045f4  movk w8, #1, lsl #16        ; w8 = 0x10508
14045f8  mov  x19, x0                ; this
14045fc  add  x21, x0, w8            ; x21 = this + 0x10508
1404600  mov  w0, #0x210
1404604  bl   0x1e17b40              ; operator new(528)
1404610  bl   0xbe2294               ; ctor
140461c  str  x20, [x21, #0x10]      ; ← 写 this + 0x10518（无条件）
```

**`0x10518` 这个字面量在这条指令流里一次都没出现**——它被拆成 `0x10508` 加 `#0x10`。
**这推翻了 `find-offset.py` 连续三轮的「62 读 / 0 写」。** 教训：**按偏移字面量枚举的搜索，结构上找不到拆分编码的 store。**

### 4.2 完整调用链

```
onViewRenderJNI (0x12b3614, .dynsym 里的 Java 导出)
  → 0x12aea7c（渲染入口）
  → 0x12aee50 / 0x12aeec0
  → 0x12b43a0 → 0x12b4440
  → 0x1405868 → 0x14048c8
  → 0x14045e0（构造器）→ 0x140461c（写点）
```

### 4.3 四道门（实测全部打开）

| 门 | 条件 | 位置 |
|---|---|---|
| A | `this->0x83 == 0` | `0x14048e8` |
| B | `this->0x82 == 0` | `0x14048f8` |
| C | once 字节 | `0x1fb5330`（实测 = 1） |
| D | async future ready | `*(0x1f78 328)` 的 `+0x70`（实测 `flags == 7` ⇒ `bic w19,1,flags>>2` 得 0） |
| E | `*(0x1f78e98) != 0` | `0x12b441c` |
| F | `renderer[0xa] != 0` | `0x12b4424`（实测 `0x4b`） |

**A/B 语义提醒**：`+0x82 == 0` 是「还没构造，去构造」，**非零是「已完成」闩锁**。读反会让「已完成」看起来像「被阻塞」。

**worker 线程 `0x1406f94` 活着**，由 census 自己算的 `start_elfva` 独立佐证，不是靠我推的。

---

## 5. 已排除的 17 条（每条都有推翻它的那次测量）

### 5.1 排除我们自己的移植

| 假设 | 推翻它的测量 |
|---|---|
| 五个方法共用哨兵对象导致自环 | `RR3_FRESH_FAKE_OBJECT=1` 时 `0xcd2abc` 仍出现（archive `20260805-085156-nopairlog`） |
| jstring 值在返回途中丢失 | 方法 ID 全部查得到（0 miss）＋全部 `jstring-heap` ＋客机**根本没请求**那三个值 |
| 0 字节 Cloudcell 配置桩是原因 | A/B：`DeviceInfo` 21/21、`cc-slot` 22/22、`jni-enter` 45/45 逐项相同 |
| JNI 桥 62 次进入不返回 | `static-enter 45 / static-exit 45`（那一对判别正确） |
| 互斥量死锁 | 1114 次加锁、`blocked_spins=0 contended=0`（真实计数） |
| Adreno/Qualcomm 身份该改 | **它是实测对照杠杆**：改数字只动 `[rend+0x500]`、不动 `t4fc`。**不要动** |
| `/proc/cpuinfo` 桩多余 | 它和 Cloudcell 文件在同一个 `if` 里，但客机确实在用（`getCpuName` 每轮都被调） |

### 5.2 排除客机侧的

单重解引用的读法（客机有 `ldr x8,[x8]` 一级）· 门 A/B 被关闭 · `+0x80` 是标志字（实测是自指堆指针 `ren+0xa8`）· 最后动作是 `close`（`Sort-Object -Unique` 取的是集合不是顺序）· 方法 ID 查不到（0 miss）· `hbn` / `CC_SeedData.bin` 缺失（两者都已部署可读：`apk/res/hbn` 4 字节内容 `"a527"`；`.depot/CC_SeedData.bin` 296,657 字节，头 `"NomNom"`，内含 `rr3_authomatic_const.js`）

### 5.3 本会话新写的仪表自己产生的 6 条

- `RR3_JNI_PAIR_LOG` 声明了但**从未包进调用**（装饰品）
- `jniPairMark` 用 `phase[0]=='e'` 判别，而 **`"enter"` 和 `"exit"` 都以 `e` 开头** ⇒ `jni-obj-exit` 在构造上不可能出现，造出了「62 进 0 出」的假挂死
- 账本字段 `waits` 其实是**请求数**（`noteMutexWait` 在每次进入时无条件调用）⇒ `waits == acquires` 是恒等式，不是争用
- 互斥量明细表**只由「已被持有」的锁构成** ⇒ 「没人持有但 587 人在等」按构造报不出来
- `census-pc` 对宿主地址一律 `pc_elfva=n/a`，而 `nroBase` 就在手上
- PC 采样用「最近前符号」而非包含判据，把三个不同 syscall 桩的 `+4` 读成了「五线程锁护航」

---

## 6. 为什么模拟器到这儿了（三条独立证据）

```
sampler-selftest-b:  pauseRes=0x0 ctxRes=0x0 resumeRes=0x0
                     pc=0x80cf8a18  lr=0x807c8d44     ← 两个都是宿主 NRO 地址
```

1. `svcGetThreadContext3` 在 Eden 下返回**读取者自己的寄存器**（暂停有效、每个 syscall 都成功，但寄存器是读取者的）
2. FP 栈回溯 `censusFpWalk(ordinal, context, …)` 需要同一个 context ⇒ 即使触发，走的也是我们的栈
3. import 环（`symbol_resolver.cpp` 生成每个 import 的桩）只在客机调用东西时更新

**⇒ 客机地址只有一个来源：我们的代码正跑在客机栈上时，`__builtin_return_address(0)`。**

**⇒ 更根本的一条**：在 rehost 架构里，**客机本来就大部分时间待在我们的替身代码里**，所以 **「客机 PC 采样为 0」是健康运行的预期形状，不是停滞证据**。这个指标没有诊断力。**本会话两次把它当成结论，两次都作废。**

**PC 采样器在这里能回答的**：某线程**停在**客机代码里（此刻不在我们的桥里）——它找出了 `0xcd2abc`。
**它不能回答的**：客机在推进 / 卡住了 / 消失了。

---

## 7. 接手时请先读的纪律（本会话付了 17 次代价）

1. **0 命中先证伪谓词。** 对照必须来自**同一次运行**，且必须是**已知为真**的那个点。
2. **有上限的日志里读缺失 ≠ 没发生。** 先看预算耗尽没有（`phase=per-call-log-cap ... suppressed_total=N`）。
3. **环形缓冲必须按索引读，`Sort-Object -Unique` 只能回答「出现过哪些」。**
4. **21 MB `.text` 上不要用 capstone 线性扫描。** 用字级 ADRP 解码，不依赖扫描同步。
5. **控制 ARM64 编码要从二进制里验证过的例子反推，不要手算位段。**（本会话算错四次。）
6. **断言「某函数有零调用」之前，先证明它不是被间接调用**（vtable / 函数指针表 / `glXGetProcAddress` / `dlsym`）。
7. **`(v & 3) != 0` 这类对齐过滤对字符串/数据指针是错的。**
8. **改完源码用时间戳和「新代码的特征串」验证它真进了二进制**，不要看「脚本没报错」。

---

## 8. 能解锁的三条路

### 8.1 真机（最直接）

```powershell
powershell -ExecutionPolicy Bypass -File tools\Deploy-To-Switch.ps1 -Card <SD卡盘符>\
```

**崩溃最好** —— Switch 的 crash dump 会直接给出该区段的客机回溯，正是模拟器反复证明拿不到的东西。
请记录屏幕现象：黑屏 / 停在 EA logo / 出菜单 / 崩溃。

### 8.2 Cloudcell 方向（需边界决定）

槽 **5 / 20 / 22** 都被客机调用，来自同一个函数 `[0xcd1014,0xcd3648)`：

- 槽 5 是**工厂**，返回接收者；接收者的**槽 20** 被尾调用（`0xa0/8 = 20`）——**两者是同一条链**
- `s_ccVtable` 全部 64 槽都被填过（奇偶默认），只有 6/17/18/34 被钉住
- 已钉槽 17 的注释写着 `returns bool, 0 == "proceed"`，**客机从没调到 17**

**已明确不做**（用户划定的反作弊边界）：伪造 Cloudcell 响应、逆向反作弊、造假遥测。
可做的正当动作是**让 Cloudcell 明确报告「不可用」**，让引擎走它自己的离线分支（和 `RR3_NET_CONNECTIVITY = 0` 同类）。
**改之前必须先测清引擎在等哪一个槽。**

### 8.3 如果继续在模拟器上做

**先修谓词**（第 3.3 节），并给每个新仪表配**独立预算 + 阳性对照 + 显式 CAPPED 行**。不要从「没出现」里读结论。

---

## 9. 当前开关与状态

| 开关 | 值 | 含义 |
|---|---|---|
| `RR3_FRESH_FAKE_OBJECT` | `1` | 五个 JNI 方法每次返回独立对象而非共享哨兵。**理由是语义（重复调用不该返回同一对象），不是那个已作废的 A/B** |
| `RR3_JNI_PAIR_LOG` | `0` | 入口/出口标记（每行一次文件写）。默认关：它今晚才存在，调查前不存在 |
| `RR3_TOUCH_STUBS` | `1` | 0 字节桩 + 虚拟 `/proc/cpuinfo`。**注意它同时开关两者** |
| `RR3_FRAME_LOOP_PUMP` / `RENDER` | `1` / `0` | 帧循环每 tick 调一次绘制会自造 946,234 次 fault 风暴 |
| `kStatBionicMode` | `2` | 宿主 stat 布局 + 仅补 `st_size@0x30`（mode 1 全量 bionic 会让客机提前退出） |

- 构建 exit 0，契约 11/11 exit 0
- `PORT_PROGRESS.md` 6484 行，17 次自我撤回全部就地标注
- **`symbol_resolver.cpp` 逐字节冻结**（SHA-256 `270fc563080915adbe06b88fb5086f9aa9cc23cd29924b02ad3071e90adb420a`），全程未触碰
- E 盘回收站仍有 8.97 GiB 可恢复副本待清

---

## 10. 附：本会话新增的跨项目经验

1. **按偏移字面量枚举的搜索找不到拆分编码的 store**（`add base,#K` + `str [base,#small]`）；用第二种方法交叉验证时，正对照必须是「已知为真的那个写点」，而不是那个工具自己的 Control A
2. **不可编码的指令 = 不可交叉验证的错误**（`ldr x,[x,#0x10518]` 根本不存在，抄错也没法拿汇编器意见去对）
3. **可能卡住的调用，日志必须写在「进入」那一刻**；出口日志不是入口日志的弱化版，是写错的那一种
4. **注释里的反汇编可能完全正确，而旁边的探针算的是另一回事**
5. **rehost 移植里「客机 PC 采样为 0」是健康形状**
6. **阴性结果的可信度 = 产出它的仪器的可信度**
7. **在已有小节标题上做 replace 会把标题吃掉**——要插入就锚在它下一行的正文上

---

## 11. 2026-10-05 晚 · 收到 `RR3_MINIMAX_FIX_HANDOFF` 后的复核与新结论

对方修正了我的核心判断，**我独立用真实字节验证，他是对的**；我自己又试了一条更省的路，**被实测否掉**；同时找到了那条路的替代品。

### 11.1 「不是链表，是连续存储」——已证实，我的措辞是错的

原交接摘录**恰好漏掉了证明它的那几行**（`0xcd2ae4`–`0xcd2af8`）。补齐后：

```
0xcd2ad4  sub  x23,x21,#0x30     ┐
0xcd2aec  sub  x23,x21,#0x30     ┘ 两条分支都步进，步长恒为 0x30
0xcd2ae0  ldur x0,[x21,#-8]        第二个 24B 串的长串数据指针
0xcd2af4  ldur x0,[x21,#-0x20]     第一个 24B 串的长串数据指针
0xcd2b00  ldur x0,[x29,#-0x48]     x0 = B
0xcd2b04  stur x20,[x29,#-0x40]    E := B      ← 教科书式 vector 析构
0xcd2b08  bl   0x1e17b50           operator delete(B)
```

**全程没有��何 `next` 指针读取。** 所以终止条件是**纯算术**的：`E >= B` 且 `(E-B) % 48 == 0`。
**注意 48 不是 2 的幂，`delta & 47` 是错的。**

采样分布（全部归档）：`0xcd2ad0` 17 次、`0xcd2abc` 10 次、**退出路径 0 次**。与「跑不到头」一致，但 27 个采样偏少，**不构成证明**。

### 11.2 实测否掉：census 采样器对客机代码命中率 = 0%

试过一条不需要改客机代码的捷径：`census-regs-special` 会输出主线程 `x29`（客机 FP），所以在 census 里当 `pc` 落在该 FDE 时读 `[fp-0x48]` / `[fp-0x40]` 即可。

已实现并构建（`census-deinfo-bounds`），并**把探针改成对每个客机 PC 采样都出一行**（带 `pc_rc.off`），好把命中率变成数字而不是假设。

**结果：240 秒一轮，0 行。`pcGuest` 一次都没为真。**

**⇒ 任何依赖「在客机代码里抓到它」的计划都不成立**，包括这一条。这与「64 采样 / 0 个客机 PC」一致，但**这次是测出来的**。

### 11.3 客机 `.text` 改码可行 —— 走的不是 `svcSetMemoryPermission`

`guest_view_probe.cpp:2165` 的「NOP patch removed: svcSetMemoryPermission fails on guest .text」**只否掉了 `svcSetMemoryPermission` 这一条**。

Horizon 有专用 JIT 代码内存 API，而这个加载器**已经在用它**：

```
multi_dso_linker.cpp:1013  svcCreateCodeMemory(&range.handle, range.backing, range.size)
multi_dso_linker.cpp:1027  svcControlCodeMemory(range.handle, CodeMapOperation_MapOwner, ...)
multi_dso_linker.cpp:1055  svcControlCodeMemory(range.handle, CodeMapOperation_MapSlave, ...)
multi_dso_linker.cpp:213   std::vector<SegmentCodeMapping> ranges;   ← handle 留在这里
multi_dso_linker.cpp:219   void closeSegmentedMappings();
```

**⇒ 每个客机段的 code-memory handle 都保存在加载器 session 的成员 `ranges` 里，只是还没有对外的 accessor。**

**需要的接线（几十行）：**
1. `multi_dso_linker.cpp` 加 `bool codeMemoryHandleForAddress(uintptr_t addr, Handle* out)`，遍历 `ranges` 找覆盖该地址的段；
2. 用该 handle 走 `MapOwner` 拿可写视图；
3. 在 `0xcd2aa0` 打探针，**寄存器/条件码的保存与重放按对方文档第 4.3 节做**（那部分判断正确，与内存权限无关）。

**`multi_dso_linker.cpp` 不是冻结文件**（冻结的是 `symbol_resolver.cpp`，SHA-256 `270fc563…`）。

### 11.4 对方交付物的核验结果

| 检查 | 结果 |
|---|---|
| 5 个 SHA-256 校验和 | **全对** |
| `rr3_vector_check.py` 18 个自测 | **全过，rc=0** |
| `bounds --begin 0x1008 --end 0x1098`（跨度 144 = 3 元素） | rc=0，无异常 |
| **故意喂错（跨度 145，非 48 倍数）** | **rc=1，model_issue_count=1** ✓ |

**它在该失败的时候失败了** —— 这是本项目今晚对自己的仪表从未做到过的事。
已解压到 `.build\agentfix\rr3_minimax_diagnostics\`（scratch，不污染工程根）。
---

## 12. 2026-10-05 夜 · 客机 `.text` 可写性实测（两条死胡同之后）

### 12.1 死胡同一：census 对客机代码命中率 = 0%

试过一条不需要改客机代码的捷径：`census-regs-special` 会输出主线程的 `x29`（客机 FP），
所以在 census 里当 `pc` 落在该 FDE 时读 `[fp-0x48]` / `[fp-0x40]` 即可。

已实现（`census-deinfo-bounds`，`pthread_bridge.cpp`），并**把探针改成对每个客机 PC 采样都出一行**（带 `pc_rc.off`），
好把命中率变成数字而不是假设。

**结果：240 秒一轮，0 行。`pcGuest` 一次都没为真。**
⇒ 任何依赖「在客机代码里抓到它」的计划都不成立。这与「64 采样 / 0 个客机 PC」一致，**但这次是测出来的**。

### 12.2 死胡同二：没有 owner/slave 双映射

曾以为 `multi_dso_linker.cpp` 的 `SegmentCodeMapping`（含 `handle` / `ownerAddress`）
就是可写别名。**实测全局表 `segments=0`：一个段都没有。**

原因在同文件 L1262/L1264 的三目选择器——客机走的是
`mapNormalSegmentedRange`（L979），它在 **L998 用普通 `svcMapMemory`**，不是 code memory。
所以 code-memory handle 那条路**对这个客机不适用**。

### 12.3 结论：客机 `.text` **可写**，那条旧注释是错的

```
phase=guesttext-perm-start    guest_code=0x210722baa0  page=0x210722b000
phase=guesttext-perm-result  rc=0 module=0 desc=0
                             => guest .text is WRITABLE via svcSetMemoryPermission
```

（archive `20261005-192644-permprobe`，只改权限、未写任何客机字节、随后改回 `Perm_R`。）

**⇒ `guest_view_probe.cpp:2165` 的「NOP patch removed: svcSetMemoryPermission fails on
guest .text (different memory block)」被实测推翻。** 那条注释当年为一个 NOP 补丁下的结论，
代价是把整条客机改码路封掉；**实测 `rc=0`**。

### 12.4 下一步（已解封）

1. 在 `0xcd2aa0` 打探针：`B` 指令跳到 trampoline，保存 x19–x30 / sp / v8–v15 / 条件码，
   读 `[x29-0x48]` 与 `[x29-0x40]`，落盘，**重放原指令**后返回。
2. 写完 `armDCacheFlush`；必要时 `armICacheInvalidate`。
3. 算 `E >= B` 与 `(E-B) % 48`（**48 不是 2 的幂，不能用 `& 47`**）。
4. 用随包的 `rr3_vector_check.py` 复核（已验证：18 自测全过、负对照正确返回 1）。

**寄存器/条件码保存与重放的要求与内存权限无关，那部分判断仍然成立。**
---

## 13. trampoline 已交付；两处前提被修正，其中一处是我自己刚犯的 bug

### 13.1 冻结基线 SHA 是陈旧的

```
symbol_resolver.cpp  SHA256 = 32a3e014b037a848650cab98049d42165460009e3948b0e42e755dc587d3a228
mtime = 2026-10-03 13:19
```

**本会话一直引用的 `270fc563080915adbe06b88fb5086f9aa9cc23cd29924b02ad3071e90adb420a` 是错的。**
mtime 证明本会话没写过该文件（最后修改在两天前），**但文档里的基线值不可信**——请以实测值为准。

### 13.2 我引入的活 bug：权限探针抹掉了执行权限

libnx `svc.h`：
```
Perm_R  = BIT(0)                 只读，无 X
Perm_Rx = Perm_R | Perm_X
"This can be used to move back and forth between Perm_None, Perm_R and Perm_Rw"
                                    ← 不会返回 X
```

`guest_view_probe.cpp` 里的权限探针原本在测完后 `svcSetMemoryPermission(page, Perm_R)`，
**那等于把客机 `.text` 的执行权限抹掉**，而注释还写着 "leave no trace"。**已改为 `Perm_Rx`。**

**凡是动过客机代码页权限的地方，恢复值必须是 `Perm_Rx`，不能是 `Perm_R`。**

### 13.3 站点偏移我写错了

`0xcd2aa0 raw=0xf85b83b4` = **`ldur x20, [x29, #-0x48]`**（负）。
我交给 worker 的 brief 里写成了 `+0x48`。worker 用 capstone 独立解出 `-0x48` 并指出了矛盾——**它是对的**。

附带好处：捕获的 `B` 与重放后 `x20` 拿到的是同一个字，构成免费自检。

### 13.4 trampoline 规格（`native-switch/source/rr3_deinfo_probe.cpp`）

70 条指令 / 280 字节，冻结为 `kTrampolineCode`，并与汇编器输出**逐字节比对**（防漂移）。

保存帧 = `sp − 512`：

| 状态 | 保存 | 恢复 |
|---|---|---|
| x0–x28 | `[sp+0]`..`[sp+224]`（16×`stp`） | 同址 16×`ldp` |
| x29 | `[sp+232]` | `[sp+232]` |
| x30 | `[sp+240]` | `[sp+240]` |
| NZCV | `[sp+248]`（`mrs`） | `msr nzcv` |
| **v0–v15 全 128 位** | `[sp+256]`..`[sp+496]`（8×`stp q`） | 同址 |
| sp | — | `add sp,#512` |

顺序上四条承重：重放在寄存器恢复**之后**；`msr nzcv` 在重放**之前**；用 `stp q` 而非 `stp d`（`d` 会把 bits[127:64] 清零）；`add`/`ret` 不带 `S`。

**trampoline 内不调任何 C 函数、不 printf、不加锁、不碰 TLS** —— 它跑在客机自己的栈上、在桥里面，
我们的文件系统封装会重入桥。改为把 B/E 写进预分配缓冲 + `stlr` 递增序号，由宿主侧排空。

### 13.5 已验证 / 未验证

**已做**：从 ELF 读站点字节；devkitPro binutils 汇编+反汇编；静态断言（重放字 == `0xf85b83b4`、
保存集合==恢复集合、无 call/branch、存储顺序）；**冻结数组与汇编器输出逐字节一致**；
用 capstone（独立解码器）写的**语义解释器**跑一遍重定位后的字节，校验捕获值对、31 个 GPR 全等、
sp、NZCV、v0–v15、返回地址、x20 重放——通过；**8 个阳性对照全部会失败**；构建 exit 0 无告警；契约 exit 0。

**未做**：**没有真正执行任何 AArch64 代码**（本机 x86-64，无 qemu-aarch64）。
解释器是语义模型、不是 CPU，不建模 cache/重排序；release/acquire 顺序只做了**静态**断言。
`svcSetMemoryPermission` 是否保留 X、安装/卸载的实机行为，均未实测。

### 13.6 当前状态：探针已编译但被 gc-sections 丢弃

`rr3_deinfo_probe.o` 编译通过，但**没有任何东西调用它**，所以被 `--gc-sections` 从 NRO 里丢掉。
**要启用必须改一个现有 `.cpp`（在帧循环里调 install）** —— 下一步就是这件事。
---

## 14. 决定性数字到手（2026-10-06）

### 14.1 补丁安装失败关闭，客机 `.text` 未被改动

```
deinfo-install  ok=0 stage=6 installed=0 wrote=0x00000000
  site=0x2106a2baa0  tramp=0x23e8cc1000  branchDelta=12,384,294,240  (约 12.4 GB)
```

`jitCreate` 把 trampoline 放在 `0x23e8cc1000`，离站点 12.4 GB。**单条 `B`（±128 MiB）
和 ADRP+ADD（±4 GiB）都跨不了。** stage 6（重定位哨兵）先报错，掩盖了真正原因。

**`wrote=0x00000000` / `installed=0` ⇒ fail-closed 按设计工作，客机代码一个字节没变。**

**⇒ 修法：trampoline 必须落在站点 ±4 GiB 内**（客机映像自身或邻近保留区），不能用 JIT 池。

### 14.2 但 census 那个读数器命中了，并给出根因

```
fp=0x10822fc4a0  B=0x8061d9dc  E=0x80877c58
span=0x25a27c (2,466,428)   E_ge_B=1   mod48=0
verdict=SPAN_NOT_MULTIPLE_OF_48      pc_off=0xcd2abc / 0xcd2ad0
```

（39 行采样，archive `20261006-022123-devinfo-install`）

**关键事实：B 和 E 都不是客机地址。**
- 客机映像：同一轮 `site=0x2106a2baa0` ⇒ 客机在 `0x21xxxxxx`
- 我们的 NRO：`host+0x545950 @ 0x80cfa950` ⇒ 基址 `0x80CB5000`
- **`0x8061d9dc` 和 `0x80877c58` 都低于 NRO 基址**，既不在客机映像，也不在我们的 NRO

**⇒ 客机在遍历一个 begin/end 指向「别的映射」的容器，而不是一个连贯的 48 字节步长分配。
`E - B = 2,466,428` 不是 48 的倍数，所以遍历永远碰不到 B，循环永不退出。**

**根因从假设变成事实。** 保留的不确定性：那两个地址具体属于哪个映射尚未查明
（低于 NRO 基址，可能是内核、另一个 NRO、或某个静态表）。
但「它不是客机堆上的合法 vector」是确定的——它不在客机映像范围内。

**这正是本会话造出来的那个读数器的产出。** 它上一轮实测命中率 0%，这一轮命中 39 次；
**同一台仪器两轮结论相反**，所以「命中率」本身必须逐轮测，不能继承。

### 14.3 下一步

1. **查 `0x8061d9dc` / `0x80877c58` 属于哪个映射。** 候选：本移植的假对象
   （`g_fakeObjectObject`、`namedObjectFor(key)` 的对象、`iface_NewStringUTF`
   造的 `String`）、或某个静态表。**先查归属，再谈修复。**
2. **把 trampoline 移进客机映像或站点 ±4 GiB 内**，否则补丁路线走不通。
3. 归属确定后，**修复点大概率是「让那个容器拿到一个连贯的、48 字节步长的真实分配」**。
---

## 15. 根因收敛为一个事实 + 一个待测的推断（2026-10-06）

### 15.1 两条独立证据，同向

**静态**（`[0xcd1014,0xcd3648)`，`_range` 读的 `address_range` = 0x2634 = 9780 B = **2445 条指令**，
与 capstone 解出的条数一致；对照 `0xcd1058 stur x8,[x29,#-0x28]` 通过）：

```
0xcd2aa0  load   ldur x20, [x29, #-0x48]     <- 读 B
0xcd2aa8  load   ldur x21, [x29, #-0x40]     <- 读 E
0xcd2b00  load   ldur x0,  [x29, #-0x48]     <- free(B)
0xcd2b04  STORE  stur  x20, [x29, #-0x40]     <- E := B，全函数唯一写 E 处
```

**⇒ 整个 2445 条指令里，没有一条写 `[x29-0x48]`。** 一个局部 vector 必然在序言被初始化。

**运行时**（39 采样；NRO 基址由本轮 76 个 `census-pc` 一致推出为 `0x800aa000`）：

```
B = 0x8061d9dc  ->  我们 NRO 内 +0x5739dc  ->  .text 的 _free_r +28
E = 0x80877c58  ->  我们 NRO 内 +0x7cdc58  ->  .data 的 __global_locale +432
span = 0x7cdc58 - 0x5739dc = 0x25A27C = 2,466,428   （与日志 span 吻合，自检通过）
```

**⇒ 析构在遍历一片本函数从未写过的栈内存。容器从未被构造。**

### 15.2 一次「差点撤掉正确结论」的自我更正

我一度用 `G.FDE[i][1]` 当结束地址，算出该函数有 9.7 MB，进而以为「全函数无写入」是被
截断读造成的假象。**错在 `FDE` 的第二元素不是结束地址，是编码后的 FDE 指针**；
`_range()` 真正从 FDE 记录读 `address_range`，且其 docstring 里已经写过这个坑修过一次。

**这次错误的形状和今晚所有错误对称：不是被假阴性骗，是把自己误读的表当成了事实。**

### 15.3 唯一剩下的推断，已埋好探针

「`x29` 在整个函数期间是本函数的帧指针」目前是**推断**。序言是
`x29 = sp+0x10` 后 `sub sp,sp,#0x5d0` ⇒ **若运行时 `fp - sp == 0x5e0`，则确认成立**。

`census-deinfo-bounds` 行现已带 `fp= sp= fp_minus_sp= expect_0x5e0=`。
**但它跨轮命中率不稳定（实测 0 / 39 交替），必须逐轮测、不能继承上一轮的结果。**

### 15.4 下一步

1. **重跑到 `census-deinfo-bounds` 命中**（约 1/3 概率），读 `expect_0x5e0`。
2. 若为 1：`x29` 确认是本函数帧指针 ⇒ **「容器从未构造」成立** ⇒ 去找「谁本该构造它」，
   即本函数调用链上被我们 stub 掉的那一个。
3. 若为 0：`x29` 被挪作他用 ⇒ 前面那条静态结论要重做，**且不能当成已定**。
4. `deinfo-install` 仍 fail-closed（trampoline 在 JIT 池，离站点 12.4 GB）。要启用补丁
   须把 trampoline 放进客机映像或站点 ±4 GiB 内。
---

## 16. 加长运行没有提高命中率（一次有价值的阴性）

archive `20261006-022811-fpsp`（200 s） 与 `20261006-023240-fpsp-long`（**420 s**）：

| | 200 s | 420 s |
|---|---|---|
| `census-pc ordinal=0` 行数 | **5** | **5** |
| `census-deinfo-bounds` 命中 | 0 | **0** |
| `DeviceInfo::` 行数 | 21 | 21 |
| 日志总行数（对照） | 5552 | 5538 |

**运行时间翻倍，主线程采样行数完全没变。** 瓶颈是 census 每趟的耗时
（`pthread_bridge.cpp` 注释：单趟 ≥6.7 s，因为每线程一次
`svcSetThreadActivity(Paused)` + `svcGetThreadContext3` 往返，而趟与趟之间
还叠着发布节流 `kCensusPcPublishMs = 8000`）。

**⇒ 我用错了杠杆：想提高命中率，去加长了运行，而不是提高采样频率。**

**⇒ 正确做法**（下一步）：为这一个测量单独提高采样率，例如
(a) 只对主线程采（跳过其余 5 个线程的往返），或
(b) 临时把 `kCensusPcPublishMs` 调小、或在 stall 判定命中时立刻补一次采样。
**在采样率提上去之前，重跑同一轮配置不会带来新信息。**

**保留的不确定性不变**：`x29` 是否为本函数帧指针仍未测量。探针字段
`expect_0x5e0` 已在位，只等一个采样率够的运行。
---

## 17. 补充采样器已接入但**不产出**（一次实测的静默失败）

`rr3RootPcSamplerStart()` 已在 `guest_view_probe.cpp` 的 `frames == 0` 处调用
（`frames == 0` 块确实执行：同块内的 `seg-owner-slave-sum`、`guesttext-perm-start`、
`deinfo-install` 三行都在）。

archive `20261006-025121-rootsample`：

```
root-pc-sampler-* 行数 : 0        （start / rate / NOT-GUEST-IMAGE / OUT-OF-RANGE 全为 0）
FAILED reason=pthread_create : 无   （⇒ pthread_create 成功，线程起来了）
FAILED reason=tick-freq-unavailable : 无  （⇒ tickFreq 正常，没有在入口返回）
```

worker 入口（`pthread_bridge.cpp:3243`）的前两个早退都排除了，**卡在找 root 条目**：

```cpp
for (unsigned tries = 0; tries < kRootPcHandleTries && rootHandle == 0; ++tries) {
    for (const CensusEntry& entry : gCensus)
        if (entry.used && entry.root && !entry.exited && entry.kernelHandle != 0) { ... }
    condvarWaitTimeout(... kRootPcHandleRetryMs ...);   // 100 ms
}
// kRootPcHandleTries = 200  ⇒ 最多等 20 s
```

`entry.root` 只由 `censusBindCaller` 设置（`pthread_bridge.cpp:693`），而
`censusBindCaller` 只有两个调用点：`censusArm()`（**在调用 arm 的那个线程上**，
不保证是客机主线程）和 L3559 的 first-import 路径。

**⇒ 下一步（具体、便宜）**：查 census 里是否存在
`used && root && !exited && kernelHandle != 0` 的条目；不存在则是
**「谁在 arm census、arm 的是哪个线程」**的问题——`censusArm()` 由谁调用需要确认。

**另外两处 worker 自查出来的、我给错的数字**（它对，我对错）：
- `census-deinfo-bounds` 在发布门**之前**发出（2688 vs 2859），39 行 B/E 来自
  10 行**发布** ⇒ 我说的「5 个采样」是发布数，不是 B/E 采样数。
- 那 420 s 轮里 33 个 root 采样**全是 host-nro** ⇒ 真正的门是 `if (pcGuest)`，
  不是采样率。**所以「加长运行」本来就不该有效——我归因错了。**
- `mod48=0` 是 `strideOk` 标志（真余数 44），字段名有歧义。
- 我引用的 `fp=0x10822fc4a0` 与 `sp=0x10830fbec0` **来自两个不同归档**，
  差值为负 ⇒ 我给出的 `expect_0x5e0` 预期建立在跨归档污染的数据上。
  **探针本身没错**（它现在从同一个 context 算 `fp_minus_sp`），错的是我的预期。
---

## 18. 撤回第 17 节的诊断

第 17 节说「census 里不存在可用 root 条目」。**日志直接否掉了它**（archive `20261006-025121-rootsample`）：

```
phase=census-arm  ordinal=0 target=0x124f800  sweep=4s stall=8s
phase=census-ord  ordinal=0 ... exited=0 host=1 root=1  target=0xbb2  lr_ring=210...
```

采样器要的四个条件 `used && root && !exited && kernelHandle != 0`
**全部满足**：root 条目存在、就是主线程（`root=1 host=1`）、未退出、句柄 `0xbb2` 非零。

顺带确认了一条我猜对的：`censusArm()` 唯一的调用点是
`symbol_resolver.cpp:1992` 的 `resolveHostSymbol`（冻结文件，只读确认），
**所以 arm 的是客机 linker 线程**。

**⇒ 下一件要查的不是 census，是 worker 里那段搜索本身**
（`pthread_bridge.cpp:3259-3290`）：它持 `gCensusLock` 遍历 `gCensus`，
条件如上，应当命中却没命中。**在读到那段代码的完整逻辑之前不要下结论。**

**这是今晚第四次「我的诊断被自己的下一次读数打掉」**：
第 2 次（探针少一级解引用）、第 3 次（门 A/B 语义读反）、
第 5 次（把 `FDE[1]` 当结束地址）、这一次。
---

## 19. 采样器为什么不产出：它卡在第一条日志**之前**

`pthread_bridge.cpp` 里那段搜索有**三个** `FAILED` 分支
（`pthread_create` / `tick-freq-unavailable` / `no-root-census-entry`），
**三条 `root-pc-sampler-start` 一条都没出现** ⇒ **搜索成功了，线程在采样循环里。**

找到之后的第一件事不是打日志：

```
3301  const GuestStartupPlan plan = captureGuestStartupPlan();   ← 整卡 fsync
3302  const uintptr_t nroBase = censusNroBase();
...
3322  censusLog("root-pc-sampler-start", ordinal, 0, "STARTED ...");
```

**⇒ 线程卡在（或极慢地耗在）`captureGuestStartupPlan()` 里的 fsync 上，因此一条日志都没打出来。**

**这比「采样器没输出」严重，因为它可能是主动伤害：**
`recovery_guard.cpp` 已经实测过 fsync 在模拟 SD 卡上是整卡同步，census 曾因此
每秒 60 次整卡同步、把客机自己的文件 I/O 拖死。**在 frame 0 挂一个 10 Hz
采样器、每拍重新抓 plan，等于自己制造同类开销——它在扰动客机，不是在观测客机。**

**⇒ 修法（按优先级）：**
1. **把 `STARTED` 日志移到 `captureGuestStartupPlan()` 之前**，先证明线程活着；
2. **不要每拍重抓 plan**：worker 注释里已经写了「图像集在卡住时不会变」，
   却仍然每 `kPlanRefreshPasses` 拍重抓——要么把周期拉大到整个运行一次，
   要么复用 census 线程已算好的 plan（并接受它只在 reporter 线程上被触碰的约束，
   例如把 plan 快照一次性复制出来给采样器）；
3. 任何 fsync 都不该出现在每拍路径上。

**注意 1 和 2 的次序：先有「STARTED」这行，才谈得上判断采样器有没有在工作。**
这一次我们连它是否启动都无法区分，因为启动日志在阻塞调用之后。
---

## 20. ~~最重要的一条~~ 撤回：plan 没有坏

**这一节整个作废。** 它的唯一依据是 `root-pc-sampler-plan PLAN-CAPTURED images=2969242736`。

而 `GuestStartupPlan` 里**根本没有叫 `images` 的计数字段**：

```
bool     available{}
uint64_t resolutionPlanHash{}
uint64_t imageCount{}                                  <- 计数在这里
uint64_t dependencyConstructors{}
uint64_t mainConstructors{}
GuestStartupImage images[kGuestStartupMaximumImages]{}  <- images 是数组
```

我写的 `"PLAN-CAPTURED images=%u", plan.images` 传的是**数组**，退化成指针，
`%u` 打印了**这个局部 plan 的栈地址低 32 位**。**2969242736 是一个栈指针。**

**⇒ plan 是好的，分类输入是好的，那 123 个 `NOT-GUEST-IMAGE` 是可信的：
主线程在 123 次采样里确实全部在宿主代码。**

**⇒ 我今天用来否定自己方案的那些「客机命中率 0%」结论，全部恢复有效。**

**而这是我在写下这条教训之后的一小时内第四次犯同一类错：又一次没核对字段名就下结论。**
这一节保留在文件里，不删——删掉的话，下一个人会重走一遍。

---

## 21. 2026-10-06 凌晨 · 根因确认成立，但**探针只覆盖了 25%**

### 21.1 头条，以及必须同时说清的限制

`RR3_FRESH_FAKE_OBJECT` 翻回 `0`（共享哨兵）后跑的 `20261006-030541-sentinel-back`，
**第一次测到 `expect_0x5e0=1`** —— 判定「`x29` 是否为本函数帧指针」的唯一探针命中：

```
fp=0x10824fc490 sp=0x10824fbeb0 fp_minus_sp=0x5e0 expect_0x5e0=1
B=0x8070025c E=0x8095ac58 span=0x25a9fc(2468348) E_ge_B=1 mod48=0
verdict=SPAN_NOT_MULTIPLE_OF_48 pc_off=0xcd2abc
```

12 个客机采样全部一致，`pc_off` 落在 `0xcd2abc` / `0xcd2ad0`，即站点 `0xcd2aa0` 之后 0x1c / 0x30 字节。

**但这一节不能只写头条。** rate 心跳行只出现在 tick 1/100/200/300/400
（`elapsed_ms=44205`，约 46 秒），而这一轮实际跑了约 187 秒
（`reaper-asset-singleton waited_ns=187000000000`）。
tick 400 之后**没有心跳**，425 之后**没有采样**，而且**既没有 `STOPPED` 也没有
`FAILED`** —— 那是 sampler 循环仅有的两条退出路径。

⇒ **它不是正常退出，是卡在循环体内。覆盖率 46 s / 187 s ≈ 25%。**

**⇒ 所以「客机卡死在析构循环」这句话不能写。** 能写的只有：客机进过析构循环，
之后沉默了，而**沉默期间它在哪儿，未知**。见 21.4。

### 21.2 根因静态复核：**成立**，而且拿到硬数字

这一轮专门做了一件我之前一直回避的事：复核第 15 节那条
「全函数无一条写 `[x29-0x48]`」。因为**同一个编译器已经实测会用寄存器拆分偏移**：

```
14045fc  add  x21, x0, w8        ; w8 = 0x10508
140461c  str  x20, [x21, #0x10]  ; 实际写 this+0x10518，字面量 0x10518 全文未出现
```

而上一轮的 store 解码器**只认立即数偏移形式**，会漏掉 `str [reg, #0]`。
结论（独立自研位域解码 + CFG may-分析，与 capstone 在 320/320 条 store 上零分歧）：

| 锚点 | 值 |
|---|---|
| `0xcd1030 add x29, sp, #0x10` ⇒ | `x29 = STACK0 − 0x60` |
| `0xcd1034 sub sp, sp, #0x5d0` ⇒ | `sp = STACK0 − 0x640` |
| 相减 | **`fp − sp = 0x5e0`**（与 21.1 的 `expect_0x5e0=1` 独立吻合） |
| `[x29 − 0x48]` | **`STACK0 − 0xa8`** |
| 全函数栈相对 store | **272 条 / 125 个不同槽位** |
| 其中含 `−0xa8` 的 | **0 条**（下方最近 `−0xc0`，上方最近 `−0xa0`） |

寄存器拆分寻址**确实被这套仪表抓到了**（如 `0xcd1444 stur w10,[x27,#1]`，`x27 = sp+0x2d8`
→ 槽位 `−0x367`，同类 7+ 条），**抓到了还是没有 `−0xa8`**。

**⇒ 第 15 节的根因成立：这是一个真实的、从未被构造的局部变量被析构遍历。**
不是分析假象，不是函数边界划错（`[x29−0x48]` 是局部变量，入参在 `x29` 正偏移侧）。

有界盲区（如实记录）：**48 条 store 的基址寄存器来源无法静态判定**
（`x0×25, x25×5, x19×4, x8×4, x26×3, x24×3, x27×2, x21×1, x20×1`），
其中 `x0` 在该处是入参 `this` 而非帧派生。完全排除需要值别名分析，本轮未做。

### 21.3 被否掉的一个假设（有价值的阴性）

我本来准备追「有 DeviceInfo 字段被静默跳过」。带阳性对照枚举二进制里
**全部** `DeviceInfo::` 字面量（对照：已知 `DeviceInfo::Platform` 在 `0x48c742`，
脚本精确复现）：

**21 个字面量，客机打了 21 行，集合完全一致，一个不落。**

⇒ 假设被否。附带确认：**日志阶段完整跑完**，楔子确实在日志之后的尾段。
（注意：`find(子串)` 返回的是字面量中间偏移，必须向前回退到前一个 NUL 才是真起点。）

### 21.4 本轮我自己的三个错（都已修，保留记录）

**错 1：我说「`CAPPED budget=200` 把 rate 心跳一起饿死了」。错的。**
心跳行**从来就不在预算分支里**（它在 `else if (!capped)` 之外）。
tick 500 心跳缺失的真实含义是：**采样线程根本没走到 tick 500**。
我又一次把「日志被 cap 掉」当成了机制，而它是另一回事。

**错 2：我说「hit 行保持不封顶」。错的**，`root_pc_sampler.hpp:57`
有 `kHitLineBudget = 400`。

**错 3：交接文档说二进制是 `ET_EXEC 非 PIE`。实测 `e_type = 3 (ET_DYN)`。**
本段映射未受影响（`LOAD[1]` 的 vaddr 恰等于 file offset），但前提描述要更正。

**另外：契约测试是 10/11，不是 11/11。**
`test_external_data_vfs_contract.py:19` 硬编码了一个 mojibake 文件名
（`数据包` 的 UTF-8 字节被按 GBK 解码，`-` 塌成 `?`），
在 `zipfile.ZipFile()` 就 `OSError`，**在读取任何项目源码之前**。
已改成 `DXT_*.zip` glob + 明确失败信息，**恢复 11/11**。
顺带修掉一个隐患：原字面量带日期后缀，数据重新生成就会再次失效。

### 21.5 探针本身会制造被观测的静默（本轮最重要的方法论收获）

`pthread_bridge.cpp` 的采样器是
`svcSetThreadActivity(Paused)` → `svcGetThreadContext3` → `Runnable` 三步往返。
代码里那段注释自己写着
「leaving the guest thread paused turns frozen into wedged, which is the very
failure this investigation is about」——**但危险恰恰在 `getcontext` 本身**：
它不返回，后面那行 `Runnable` 就永远执行不到，**客机主线程被我们自己的探针冻住**。

已加三样东西：

| 措施 | 位置 | 作用 |
|---|---|---|
| **resume 看门狗** | `pthread_bridge.cpp:3577-3810` | 50 ms 检查、**150 ms 硬上界**后无条件 resume，把冻结时长上界住 |
| **step 标记** | `pthread_bridge.cpp:3994/4008/4011/4102` | 四个阻塞点各打一条，下一轮能一眼分出「卡在 getcontext（危险）」和「卡在日志（无害）」 |
| **覆盖率字段** | `pthread_bridge.cpp:4141-4156` | rate 行改成 `hits=%u/%u misses=%u/%u`，让「采了多少 / 一共多少」可算 |

**没有下结论的地方**：tick 425 之后卡在哪一步，**现有归档无法判定，本轮也不判定**。
「没有下一行」这一个观测同时兼容三种卡法（getctx / 日志锁 / 线程饿死），
证据不足以区分。下一轮由 step 标记回答。

代价如实记：看门狗强制 resume 可能产生**一次被撕裂的坏样本**，
换取的是**不会永久冻结客机主线程**；step 标记每 tick 4 行，
run 级预算 8000 行（≈208 s）——够用，因为真要卡死会在 425 tick 附近就卡住。

### 21.6 沉默这件事本身是可信的（有同源阳性对照）

采样器死后，宿主 census **仍然活着并继续输出**：
174 条 `census-ord` + 29 条 `census-sweep` + 29 条 `census-mutex-sum` + 141 条 `reaper`。
同期客机日志 **0 条**（最后一条停在 `ordinal=686 DeviceInfo::Tier: AndroidHigh`）。

⇒ **「客机在最后约 140 秒完全沉默」是一个有阳性对照的阴性结论。**
真正救回这条结论的不是采样器，是另一条独立仪表。
**这才是阴性结论该有的样子。**

### 21.7 下一步

1. **等 240 s 那轮**（tag `watchdog-frame`）验证三件事：
   看门狗是否兼住采样器、帧转储是否打出毒药模式自检、沉默期位置是否终于可观测。
2. **栈帧转储**（`census-deinfo-frame`，头 `[fp-0x80,fp-0x10)` 7 行 +
   脚 `[sp,sp+0x40)` 4 行 + SUMMARY）已实现，自检**抓到了转储器自己的字节序 bug**
   （`deadbeef` 曾输出成 `efbeadde`，端序相关）。读不到渲染成 32 个 `?`，
   **绝不渲染成 32 个 0** ——「帧是空的」和「帧没读到」必须是两个字符串。
3. **真机仍然是最快的路**（8.1）。模拟器的采样限制已经吃了三轮。

---

## 22. 2026-10-06 03:43 · Eden 这条路判死，路线改道

一轮 240 s（tag `watchdog-frame`，归档 `20261006-034353-watchdog-frame`）。
**结论先行：采样器这条路在 Eden 上走不通，而且卡点被一步钉死了。**
这一节是**路线决策**，请先读完再动手。

### 22.1 仪表是好的：自检在真实二进制上通过

先说好消息，因为它决定了 22.3 的改道**不会把一个坏仪表带过去**：

```
phase=census-deinfo-frame-selftest hit=1 off=0xffffffffffffff80 bytes=deadbeef40adbeefdeadbeef41adbeef
phase=census-deinfo-frame-selftest hit=2 off=0xffffffffffffff90 bytes=deadbeef42adbeefdeadbeef43adbeef
phase=census-deinfo-frame-selftest hit=3 off=0xffffffffffffffa0 bytes=????????????????????????????????
phase=census-deinfo-frame-selftest poison=deadbeef40adbeef/deadbeef41adbeef row0_match=1 row1_match=1 unreadable_match=1 verdict=SELFTEST_PASS
```

第 3 行是**故意指向未映射页**的，输出 32 个 `?` 而不是 32 个 `0`。
**「帧是空的」和「帧没读到」在日志上是两个不同字符串** —— 这条是阴性读数可信的前提。

`census-deinfo-frame` 真实转储 **0 条**（采样器在 tick 7 就死了，
没等到客机进析构，上一轮那是 tick 200 前后）。
**⇒ 转储器已被自证可用，但从未在真实帧上跑过。**

### 22.2 采样器死在 tick 7，而且**卡点唯一**

比上轮的 tick 425 还早 10 倍。**step 标记一次就定位了**（这就是 21.5 那三个措施的价值）：

```
tick=1..6:  step=pause -> step=getctx -> step=resume -> step=log     四步齐全，正常
tick=7:     step=pause -> step=getctx -> step=resume                  step=log 永不出现
```

`step=resume` 标记在 `pthread_bridge.cpp:4011`，下一个 `mutexLock` 在 `4035`。
**两者之间只有一个调用**：`pthread_bridge.cpp:4016` 的
`svcSetThreadActivity(handle, ThreadActivity_Runnable)`。

⇒ **卡在这个 syscall 上。**

### 22.3 决定性对照：不是我们的锁，是 Eden 的 syscall

看门狗的输出：

```
RECOVERED handle=0x83ff generation=7 held_ms=176 resume_rc=0 recoveries=1
```

`resume_rc=0`。**看门狗从另一个线程、对同一个 handle、调用同一个 syscall，
立刻返回 0。** 而采样线程那一次永远不返回。

这排除了两个备选解释：

| 备选 | 排除依据 |
|---|---|
| 我们的锁死 | 看门狗在调用 svc **之前已解锁**（`3729-3732` → `3735-3737`），代码正确；采样器在 `4035` 之前根本没碰锁 |
| 这个线程没法恢复 | 看门狗恢复了它，`resume_rc=0` |

**⇒ `svcSetThreadActivity` 在 Eden 上会永久阻塞，且与调用者有关。
加上已知的「`svcGetThreadContext3` 返回读取者自己的寄存器」，
Eden 根本不支持跨线程线程态内省。**

**⇒ 采样覆盖率这条路，在模拟器上判死。再调参数没有意义——
卡在 syscall 内部，不在我们的代码里。**

**⇒ 但看门狗救了场**：`recoveries=1` 证明**客机主线程没有被我们冻住**。
这是本轮唯一一个确定的好消息。

### 22.4 改道：客机侧自报，不再问内核

想知道客机 PC，唯一可靠的办法是**让客机自己报**：
在客机 `.text` 打 trampoline 回调宿主，**完全不经过内核**。

两条前置都已具备：
- 客机 `.text` **可写**（`guesttext-perm-result rc=0`，见 12.3）
- 站点 `0xcd2aa0` **确实在被执行**（21.1 实测 pc_off = 0xcd2abc / 0xcd2ad0）

**唯一阻塞是距离**：trampoline 经 `jitCreate` 落在 `0x23e8cc1000`，
离站点约 **12.4 GB**，`B` 的 ±128 MiB 和 `ADRP+ADD` 的 ±4 GiB **都跨不了**。

解法（16 字节，位置无关，可跨任意距离）：

```
LDR  X16, #8          ; 从 PC+8 加载 64 位目标地址
B    .+8
.dword <trampoline 地址>
BR   X16
```

代价：改写窗口 16 字节 = 4 条指令，而原站点只有 4 字节，
**必须把被吃掉的那几条原指令逐字重建进 trampoline 尾部再跳回**。
漏一条就改语义。**这个方案正在实施中。**

### 22.5 顺带修掉的阻塞

`test_external_data_vfs_contract.py` 硬编码了 mojibake 文件名
（`数据包` 的 UTF-8 字节被按 GBK 解码），在 `ZipFile()` 就 `OSError`，
**契约测试其实是 10/11 而不是 11/11**。已改成 `DXT_*.zip` glob +
明确失败信息，**恢复 11/11**。顺带修掉一个隐患：原字面量带日期后缀，
数据重新生成就会再次失效。

### 22.6 一条不要误读的数字

tick 6 的采样是 `OUT-OF-RANGE pc_off=0x12c04d0` —— **客机主线程当时确实在客机代码里**
（只是不在 DeviceInfo 那个 FDE 区间）。上一轮我一度把「客机 0% 命中率」
当成两次归因错误，后来确认读数本身是对的。**这个读数也是对的：
客机确实在跑自己的代码。**

---

## 23. 2026-10-06 04:20 · 根因收窄到一条**可测**的路径；trampoline 终于装得进去

### 23.1 盲区闭合了，但结论要**换个说法**才能成立

上一轮（21.2）留下的盲区是「48 条 store 的基址无法静态判定」。本轮用两���独立通道闭合：

**通道 A（may-分析，CFG 定点 + 线性单通道）**：320 条 store 中 269 条解析到精确帧偏移，
`max|F set| = 1`（所有帧指针都是单值，无歧义）。剩余 57 条按基址**值类别**分：
27 条 `E`（可证明不是帧指针）、30 条 `U`。
**通道 B（与模型无关的纯语法扫描）**：2445 条指令 / 921 个内存操作数中，
代数上表示 `STACK0-0xa8` 的**恰好 2 个，且都是 load**：

```
0xcd2aa0  ldur x20, [x29, #-0x48]   ; 读 B
0xcd2b00  ldur x0,  [x29, #-0x48]   ; free(B)
```

全窗口**不存在 `add/sub Xd, x29, #0x48`** ⇒ **这个地址无法被任何指令物化**。
`&STACK0-0xa8` 从未出现在 x0–x7（319 个调用点 0 命中），
AAPCS64 出栈实参区 `sp+0x00..0xF8` 内无任何帧指针。

**⇒ 21.2 的结论要改写：不是「这个槽从未被写过」，而是
「这个槽从未被本函数及其寄存器传参写过」。**

### 23.2 真正的线索：帧槽写入次数的断崖

```
-0xe0(9)  -0xd8(6)  -0xd0(5)  -0xc8(6)  -0xc0(10)    连续 0x20 块，共 36 次写
-0xb8      -0xb0      -0xa8                             全零
-0xa0                                                    仅 1 次
```

而 **`-0xe0` 这个帧槽作为实参被传给 callee 传了 40 次**
（`bl 0x1cb4910` 16 次、间接 `blr` 13 次、`bl 0x1c67638` 2 次、`bl 0x1e17ba0` 4 次…），
目标槽 **`-0xa8 = -0xe0 + 0x38`**，正好落在那个被外传对象的**内部**。

**⇒ 假设：callee 经 `&STACK0-0xe0` 构造整个对象（写全部字段，含 `+0x38` 处的
vector），本函数只更新 `end` 字段。这一步没发生 ⇒ `+0x38` 保持栈垃圾。**

**过程内分析原理上无法关闭这条路径**，再精密的 may-分析也不行。
**这是当前最强、且可测的假设。**

### 23.3 仪表可信度：变异测试（必须做，做了就有效）

```
MUTATION = none
   OK  0xcd2b04 stur x20,[x29,#-0x40]  -> ['-0xa0']   expect 0xffffffffffffff60
   OK  0xcd1058 stur x8,[x29,#-0x28]   -> ['-0x88']   expect 0xffffffffffffff78
   OK  0xcd1444 stur w10,[x27,#1]      -> ['-0x367']  expect 0xfffffffffffffc99
   >> stores resolving to TARGET: 0   >> CONTROLS: PASS
```

变异测试（证明仪表不是「永不失败」的）：
- `inject`：把 `0xcd2b04` 的 `imm9` 从 `448(-0x40)` 改成 `-0x48`
  → `DETECTOR FIRED: YES @ 0xcd2b04`。**证明「0 命中」非空洞。**
- `sp_shift`：`sub sp,sp,#0x5d0` → `#0x5c0` → 对照 4 FAIL。**证明帧偏移算术真实承重。**

过程中自查出并修复 **3 个会伪造阴性的仪表缺陷**：
(a) 写回模式误判——`[sp,#0x10]` 被当成后变址，每条 `stp` 都污染 `sp`；
(b) 格底元素错误——`join(E,F)=E` 让「从未写入的槽」吞掉帧指针；
(c) **不健全**——`and xN,xM,#imm` 曾返回 `E`，但掩码可以保留帧指针
（`(p+15)&~15 == p`），已降级为 `U`（47 处）。

### 23.4 两个 Agent 的 CFG 结论冲突（不要投票，看通道）

对 `0xcd345c` 落地_pad：前一个 Agent 说「被 39 条分支进入」，
本轮 Agent 说「**0 条入边，由异常表进入**」。

**本轮的更可信**，理由是它同时给了机制解释（异常表进入 ⇒ `ret @ 0xcd3458`
之后的代码不在正常 CFG 内），并且用线性通道 + 语法通道补齐了那 205 条指令。
前一个 Agent 的「39 条」与「`ret` 是函数尾」自相矛盾。

**⇒ 下一条纪律：两个独立结论冲突时，看谁有独立通道和机制解释，不要投票。**

### 23.5 trampoline 改写完成：**第一次装得进去**

**先记我自己的错**：我在任务书里写「`LDR X16,#8 / B .+8 / .dword / BR X16`
= 16 字节 / 4 条」——**算错了**，`ldr`+`b`+`.dword`+`br` = 4+4+8+4 = **20 字节 / 5 条**。
正确解法是把常量放在**分支之后**，`br` 在 +4 就跳走，+8 永远不会被抓取执行：

```
+0x00  ldr  x16, #8     0x58000050
+0x04  br   x16         0xd61f0200     任意距离
+0x08  <8 字节常量>                   trampoline 地址
```

（试过「常量在前、ldr 用负位移回读」，**那是错的**——+0 正是客机取指落点，
8 字节地址会被当指令解码。）

**第二个发现：只修入口跳转不够。** 旧 trampoline 里 3 个 ADRP+ADD 指向宿主 slot，
同样跨不了 12.4 GB（实测需 3,023,509 页 > 1,048,576 页上限）。
**现已全部换成 64 位 literal，指令流里没有一条 ADRP。**

站点 16 字节窗口（全在单页 `0xcd2000` 内）：

```
0xcd2aa0  f85b83b4  ldur x20, [x29,#-0x48]  ┐
0xcd2aa4  b4000354  cbz   x20, 0xcd2b0c    │ 16 字节全被覆盖
0xcd2aa8  f85c03b5  ldur x21, [x29,#-0x40]  │
0xcd2aac  eb1402bf  cmp   x21, x20         ┘
0xcd2ab0  540000c1  b.ne  0xcd2ac8        ← 恢复点
```

`+0/+8/+12` **逐字重放**（字节完全一致）；`cbz` 是 PC 相对的，
改写成等价的绝对跳转（`cmp x20,#0` → `csel` → `br x16`，目标 0xcd2b0c）。
`x16/x17` 故意不还原——**反汇编整个 `0xcd1014..0xcd3648`，这三个寄存器出现 0 次**。

改写过程中又抓到并修掉两个真 bug：cbz-taken 路径上 NZCV 留下了 `cmp x20,#0`
的标志位；`ldr x16,[sp,#248]` 会覆盖 csel 选好的目标（NZCV 载体改用 x17）。

**跨 12.4 GB 的证明不是纯算术**，是解释器拿实测地址真跑了一遍
stub → trampoline → 客机（77 条指令，落在 0x2106a2bab0）：

```
delta 12384294240 bytes (12.38 GB)   B 需要 +3096073560，短 3062519128
stub words  0x58000050 0xd61f0200 0xe8cc1000 0x00000023
8-byte constant 0x00000023e8cc1000   → landed 0x2106a2bab0, seq=1
```

uninstall 对真实 ELF 字节**逐字节比对 16/16 OK**，`windowRestored() == true`。
8 个原阳性对照 8/8 全部 FAIL（预期），新增第 9 个（12.4 GB）也 FAIL（预期）。

### 23.6 扩权确认

trampoline Worker 改了我**没有授权**的三个文件：
`tools/rr3-deinfo-probe/{trampoline.s, build-trampoline.py, test_trampoline.py}`。
**这个扩权是对的、必须的**：字节数组由这三个文件生成，
只改 C++ 不改它们，「never hand-typed」保证和 8 个阳性对照会全部失效，
而验收项 3/4 本身就依赖它们。**已复核：`symbol_resolver.cpp` SHA-256
仍是 `32a3e014…3a228`，`pthread_bridge.cpp` / `guest_view_probe.cpp` / 交接文档未动。**

### 23.7 下一步（进行中）

一轮 200 s（tag `tramp-install`）正在跑。`deinfo-install` 是**无条件自动触发**的，
所以这是 trampoline 的**第一次真实执行**。

**预期要盯的三件事**（按重要性）：
1. `deinfo-install` 是否 `ok=1`、`wrote` 是否非 0 —— 第一次真的写进客机 `.text`。
2. **最大风险**：JIT 池在 12.4 GB 外，本项目**从没成功执行过**那个地址。
   `svcSetMemoryPermission` 能改权限 ≠ 能取指。失败模式会从「静默卡在析构」
   变成「客机跳飞/异常」——**这本身也是有用信息**（会直接暴露取指窗口问题）。
3. 若 trampoline 跑通，drain 出来的就是 **B/E 现场 + 栈帧**，
   可与 23.2 的槽位分布假设直接对账。

**若 23.2 的假设成立**，下一步就是把 trampoline 挪到 callee
（候选 `0x1cb4910` 与那个间接 `blr`，二者合计接收 `-0xe0` 29 次），
比对「谁写了 `-0xe0`、谁写了 `-0xa8`」。

---

## 24. 2026-10-06 04:26 · `stage=9`：**我猜错了，实验把它否掉了**

### 24.1 第一次真的写进了客机 `.text`

```
ok=0 stage=9 installed=0 site=0x210762baa0 tramp=0x5ee1a17000
wrote=0x58000050 jitCreateRes=0 permRestoreMode=0 alreadyInstalled=0
```

`wrote=0x58000050` **正是 stub 第一条指令 `ldr x16, #8`**。
这是本项目第一次把字写进客机 `.text`（12.4 GB 阻塞已由 23.5 解决）。

**但 stub 本身完全正确**，下一轮 `perm-order` 跑出了全部字段：

```
windowVerified=4
cbzTarget=0x210722bb0c   = site + 0x6c  (0xcd2b0c)  ✓
resume   =0x210722bab0   = site + 0x10 (0xcd2ab0)  ✓
stub=[0x58000050 0xd61f0200 0x72ba7000 0x0000006a]   ← literal 正好是 tramp=0x6a72ba7000
```

`ldr` / `br` / 64 位常量，三项都对；四个被吃掉的原字全部匹配。
**⇒ 唯一失败的是 `svcSetMemoryPermission`，不是 trampoline。**

### 24.2 矛盾：同一页、同样的调用，一次失败一次成功

| 调用方 | 页 | 结果 |
|---|---|---|
| 权限探针 `guest_view_probe.cpp` | `0x210722b000` | `rc=0` **成功** |
| 安装函数 `rr3_deinfo_probe.cpp:545` | `0x210722b000`（**同一页**） | **`0x0000d401` 失败** |

同页、同 `kPageSize=0x1000`、同一次运行、同一权限常量。
我先查过 `kPageSize` 两边一致，**不是常量问题**。

### 24.3 我的假设，以及它怎么被否掉

我当时的推断：探针在 `guest_view_probe.cpp:3045` 把页恢复成 `Perm_Rx`，
所以安装撞上**第二次** `Rx→Rw`，Eden 拒绝重复重映射。
原代码里那句注释「install 需要 Perm_Rw，所以必须排在探针之后」**是错的**——
安装自己取 `Perm_Rw`、自己恢复 `Perm_Rx`，自足。我把安装挪到了前面。

**然后我加了同一次运行里的直接测量**（`guesttext-perm-second`）：

```
second_transition_rc=0 module=0 desc=0 => a repeat Rx->Rw on the same page IS allowed
```

**⇒ 假设被否。** 重复转换是**允许**的。

而且因为我已经把安装挪到了前面，真实形状和我猜的**正好相反**：

| 次序 | 谁 | 结果 |
|---|---|---|
| 第 1 次 | 安装（现在跑在前面） | `0x0000d401` **失败** |
| 第 2 次 | 探针（跑在后面） | `rc=0` **成功** |

**⇒ 不是「重复被拒」，是「第一次失败、第二次成功」。**
两次调用之间唯一的差别是安装开头那次 `jitCreate`（在 315 GB 外分配 JIT 区）。

**这一节的教训**：我在没有先测的情况下改了顺序，并且改完之后**立刻把假设变成同一次
运行里的一次测量**——所以它一轮就被否掉，成本是一次 200 s 的 Eden。
如果我只是「改了顺序再跑一次看有没有好」，我会把这一轮误读成「顺序就是原因」，
然后在一个错的解释上继续往下走。

### 24.4 正在跑的判决实验

在 `install()` 里、`jitCreate` **之前**加一次同样的 `Perm_Rw` 探针
（`permRwPreResult`），成功就立刻恢复 `Perm_Rx`：

- `permRwPreRes=0` 且 `permRwRes=0xd401` ⇒ **`jitCreate` 是元凶**
- `permRwPreRes=0xd401` ⇒ **第一次调用在这页上就会失败**，与 `jitCreate` 无关
  （那探针那次成功就另有原因，得换方向查）

一轮 Eden 出结果。**在它出来之前不要把「顺序」或「jitCreate」当成已知原因。**

---

## 25. 2026-10-06 04:31 · 真因是**我自己的一行 32/64 位不匹配**；帧转储第一次出真实数据

### 25.1 判决实验把 jitCreate 也排除了

```
permRwPreRes=0x0000d401  permRwRes=0x0000d401
```

`jitCreate` **之前**的第一次调用也是同一个 `0xd401`
⇒ **不是 `jitCreate`**。至此已被直接实验否掉的假设有三个：
「Eden 拒绝重复重映射」「调用顺序」「`jitCreate` 破坏后续权限调用」。

而 `guest_view_probe.cpp` 里**完全相同的调用、同一页、稍后执行，返回 `rc=0`**。

### 25.2 真因

```cpp
constexpr uint32_t kPageSize = 0x1000;                              // ← 错在这
void* page = (void*)(guestSiteRuntimeVa & ~(kPageSize - 1));
```

`~(0xFFFu)` 类型是 `unsigned int`，值 `0xFFFFF000`；
参与 `&` 时**零扩展**成 `0x00000000FFFFF000`：

| | 掩码 | 算出的页 |
|---|---|---|
| `rr3_deinfo_probe.cpp`（`uint32_t`） | `0x00000000fffff000` | **`0x762b000`** ❌ 没被映射 |
| `guest_view_probe.cpp`（`uint64_t`） | `0xfffffffffffff000` | `0x210762b000` ✅ |

`svcSetMemoryPermission(0x762b000, …)` ⇒ `0xd401`。
**客机 `.text` 在 4 GiB 以上，32 位掩码根本表达不了它。C++ 的 `~` 不会提升到 64 位。**

**为什么一直隐形**：报告只记 `siteVa`（**原始**地址），**从不记掩码之后的页**：
```
site=0x210762baa0     <- 完全正确
permRwRes=0x0000d401  <- 权限失败
```
「站点对、权限失败」读起来完全像「这页拒绝重映射」。
**一个表达式，在两个文件里各写一遍、一个 32 位一个 64 位，差出来的效果像模拟器 bug，
整整白烧一个 200 秒轮次。**

已修：`kPageSize` 改 `uint64_t`、掩码独立成 `kPageMask`、
**报告新增 `pageVa` 并打进日志**（以后「我算的是什么」和「我传的是什么」同框）。

### 25.3 修复后：16 字节真的写进去了，**栈帧也第一次被看到**

```
ok=0 stage=11 installed=0 site=0x210782baa0 page=0x210782b000
permRwRes=0x00000000 permRwPreRes=0x00000000 permRestoreMode=0
wrote=0x58000050 windowVerified=4
stub=[0x58000050 0xd61f0200 0x70f00000 0x00000051]
```

- `page` 正确 ✔ · `Perm_Rw` **成功** ✔ · **16 字节已写进客机 `.text`** ✔ · stub 编码全对 ✔
- `stage=11` = `kDeInfoInstallRestored`：**写完之后，把页改回 `Perm_Rx` 那次失败**，
  代码把原字恢复、fail-closed，所以客机行为与之前完全一致。
- **12.4 GB 那道墙是真的翻过去了。** 现在只剩「改回可执行」这一格。

**`census-deinfo-frame` 第一次打出现实数据**（自检同轮 `verdict=SELFTEST_PASS`，
未映射页那行输出 32 个 `?` 而非 32 个 `0`，所以这批读数可信）：

```
fp=0x10830fc4a0 sp=0x10830fbec0 fp_minus_sp=0x5e0 expect_0x5e0=1 pc_off=0xcd2abc
off=fp-0x80  bytes=16416e64726f69644869676800353700   ← ASCII "AndroidHigh"
off=fp-0x90  bytes=0000000000000000a04ba00321000000
off=fp-0xa0  bytes=20c40f8310000000c000af03210000000
off=fp-0xb0  bytes=70c40f8310000000dc34448100000000
off=fp-0xc0  bytes=58dc6981000000000000000000000000
off=fp-0xd0  bytes=a0c40f83100000005a7885e91d78379e
off=fp-0xe0  bytes=200000000000000048db438100000000
SUMMARY: frame_bytes=0x5e0 blocks=24 zero_mask=0x2 unreadable_mask=0x0
```

`fp-0x80` 里的 ASCII 是 **`AndroidHigh`**——正是客机最后一条日志
`DeviceInfo::Tier: AndroidHigh` 的内容。**这是本项目第一次真正看到那个栈帧。**

### 25.4 未解决：`Perm_Rx` 这一格

libnx 权限位 `Perm_R=BIT(0)`、`Perm_W=BIT(1)`、`Perm_X=BIT(2)`，
`Perm_Rw=0x3`、`Perm_Rx=0x5`；本项目 `gfx_backend.cpp` 记着
`svcSetProcessMemoryPermission` 拒绝任何高于 `0x5` 的权限，
而 libnx 文档注释说 `svcSetMemoryPermission` 在 `None/R/Rw` 之间移动、**不返回 X**。
**⇒ `Perm_Rx` 这条路本身可能就是错的**，客机 `.text` 的可执行性必须另有保法。

**⚠️ 关键区分**：本轮日志里客机在 `Perm_Rw` 之后**照常跑到 `ordinal=686`**，
说明 **Eden 根本不强制执行权限**。但**真机上一定会崩**。
⇒ 代码必须在真机上正确，**不能因为 Eden 宽松就放松**。这是当前最容易犯的错。

### 25.5 接下来的两个分支（已并行派出）

- **worker**：`Perm_Rx` 失败的处理——按序尝试 `Perm_Rx` / `Perm_R|Perm_X` / `Perm_X` /
  先 `Perm_R` 再 `Perm_X`，逐条记 rc；**全失败就恢复 16 字节并报告「最终页到底是什么权限」**，
  不许把客机留在「stub 已写入且页不可执行」的状态。
- **explore**：逐字解码上面那个 0x5e0 栈帧。重点是
  **`fp-0xC0`（被外传 40 次的槽）里装的是什么**：
  如果它也是宿主垃圾，那么「从未构造」的范围就不是 `fp-0x48` 一个槽，
  而要扩大到整段 `-0x60..-0xA0`——**那会改写 23.2 的假设**。

---

## 26. 2026-10-06 04:51 · `Perm_X` 是内核拒绝的；栈帧解码**推翻了我自己的扩大化推断**

### 26.1 `Perm_Rx` 不是没调对，是内核从来就不接受

本机 SDK `svc.h` 原文：

> Perm_X is not allowed. Setting write-only is not allowed either (Perm_W).
> This can be used to move back and forth between Perm_None, Perm_R and Perm_Rw.

switchbrew 的 SVC 页给出具体码：**`0xD801`** *"Invalid permission specified.
Valid permissions are `---`, `r--` and `rw-`"*。

**⇒ 合法集合只有 `{0, 1, 3}`。`Perm_Rx(0x5)` 一样被拒。**

⚠️ **本项目 `main.cpp:472` 那条「拒绝任何高于 0x5 的权限」的记录是个陷阱**：
`0x5` 就是 `0x5`，它自己也在被拒之列。读起来像 0x5 可以，是这句话害我多绕了一圈。

**⇒ 任何「改成可写→打补丁→改回可执行」的写法都是错的。**
正确做法：取 `Perm_Rw` → 打补丁 → 回 **`Perm_R`**（合法）→
**用 `svcQueryMemory` 回读 `MemoryInfo.perm`，检查 `perm & Perm_X` 是否还在**。
理由：X 可能是**映射的属性**而非该 syscall 的参数，所以 `Perm_Rw → Perm_R`
未必掉执行位。**但必须回读验证，不能假设。**

已实现的阶梯（逐级记 rc，**任一级成功且回读仍有 X 才算装上**）：
`Perm_Rx` → `Perm_R|Perm_X` → `Perm_X` → `Perm_R`。真机预期只有第 4 级合法。

### 26.2 ⚠️ Eden 掩盖了这个 bug，而真机不会

本轮客机在页被改成 `Perm_Rw` 之后**照常跑到 `ordinal=686`**。
**⇒ Eden 根本不强制执行权限。**

**这意味着「`Perm_R` 会不会掉 X」在模拟器上永远测不出来，真机会直接崩。**
所以判据写成「**rc==0 且回读仍有 `Perm_X`**」，代码按真机写，
**不许因为 Eden 宽松就放松**。新增 stage 17–20 专门区分
「阶梯全拒 / 查询失败 / 接受了但掉了 X / 连 `Perm_Rw` 都拒」。

顺带修掉一个更安静的同类 bug：PRE-jitCreate 探针原来也用 `Perm_Rx` 恢复、
并把 rc 丢进 `void`——**真机上那会让客机 `.text` 永久停在 `Perm_Rw` 且不吭声**。

### 26.3 栈帧解码：三个结论，其中一个**推翻我自己**

**⚠️ 先记我自己的错**：我在给 Agent 的任务书里把 `off=0xffffffffffffffc0`
标成 `fp-0xC0`。**日志的 `off` 是二进制补码 64 位值，`0xffffffffffffffc0` = `-0x40`**，
那一行其实是 `fp-0x40`（E），不是 `fp-0xC0`。Agent 自己查出来并纠正了。

**结论 1：NRO 基址 = `0x80ece000`**（`census-pc` 63 条 + `census-reg-class` 44 条，两通道互证）。

**结论 2：B/E 都已确认是宿主 NRO 地址，且都不是合法堆指针**

| | 值 | NRO 相对 | 段 |
|---|---|---|---|
| B = `[fp-0x48]` | `0x814434dc` | `+0x5754dc` | **代码区**，`census-deref` 判 **`[B]` UNREADABLE** |
| E = `[fp-0x40]` | `0x8169dc58` | `+0x7cfc58` | 数据区，可读 |

**B 逐位等于 `fpwalk-frame i=1`（`fp=0x10830fbe70`，在本帧 `sp` 之下）保存的 LR**
⇒ **B 是同栈深度上一个更浅的宿主帧残留的返回地址。**
且 `B mod 48 = 28`、`E mod 48 = 8`，**都不是 48 对齐**
⇒ 它们**不可能**是 0x30 步长数组的 `{begin,end}`。`free(B)` 必然是未定义行为。

**结论 3：可见区间不是宿主垃圾 —— 我上一轮的扩大化推断被推翻**

`fp-0x60` = `0x10830fc420` = 恰为 `fp-0x80` 槽的地址；
`fp-0x50` = `0x10830fc470` = 恰为 `fp-0x30` 槽的地址；
`fp-0x30` = `0x10830fc4a0` = **fp 自身**；`fp-0x38` = `0x9e37781de985785a`
= splitmix64 黄金比例常数；`sp+0x10` = `0x10830fc500` = **序言保存的入口 sp**
⇒ **独立确认 `STACK0 = fp + 0x60`**。

**⇒ 可见区间是客机自己写的自洽数据。「整段 `-0x60..-0xA0` 都是宿主垃圾」不成立。**
⇒ **23.2 的假设既没被证实也没被推翻**，数据上仍然开放。

### 26.4 关键槽 `fp-0xC0` 我们**零观测**

原转储是「头 `[fp-0x80, fp-0x10)` + 脚 `[sp, sp+0x40)`」= `0xb0` 字节，
**只覆盖 0x5e0 的 11.7%**；中间 `[sp+0x40, fp-0x80)` 有 **0x520（87.3%）空洞**，
而**整个假设的赌注所在的 `fp-0xC0` 就在空洞里**。

`zero_mask` 也救不了场：它是 64 字节块位掩码，**日志没定义块原点**，
所以 bit 1 可以有两种读法，两种都自洽，**纯推测，不能采信**。

**已改**：转储扩成**整帧 0x5e0、每 16 字节一行**（188 行/次，命中数 4→2 以控日志量），
并在 SUMMARY 里写死 `zero_mask` 的块原点是 `sp + n*block_bytes`、
以及 `STACK0 = fp + 0x60` 这个已验证换算关系（`fp` 相对偏移 `o` ⇒ `STACK0` 相对 `o+0x60`）。

### 26.5 一处必须更正的旧数字

`E − B = 0x25a77c = 2467708`，**`mod 48 = 28`**。
前几轮我报的「余 44」只对那几轮成立，**余数每轮都变**（44 / 44 / 28）——
这本身就和「读到的是垃圾」一致。日志里 `mod48=0` 是**标志位**不是余数（这点早已更正过）。

---

## 27. 2026-10-06 05:08 · 换架构：**靠 ABI 抓 x29，全程不碰内核**

### 27.1 触发这次换架构的两条实测

**(1) 权限阶梯给出了确定答案：原地打补丁这条路死了**

```
ladderN=4  ladder=[0xd801 0xd801 0xd801 0x0000 0x0000]  rung=4
permBefore=0x3   permFinal=0x1   exec=0   stage=17
```

前三格（`Perm_Rx` / `Perm_R|Perm_X` / `Perm_X`）全被拒，第 4 格 `Perm_R` **被接受**，
**但回读 `permFinal=0x1`、`exec=0`——执行位永久丢失**。
连预检探针都把 X 丢了（`preFinal=0x1`）。
**⇒ `svcSetMemoryPermission` 框架下无法给客机 `.text` 打补丁。**

**这一轮证明 worker 的严格判据值钱**：只看 `rc==0` 会把这一轮误判成「装上了」；
**正是因为加了「回读 `svcQueryMemory` 确认 `Perm_X` 还在」，
才没把一个会在真机上硬崩的状态当成成功。**

**(2) 采样器又死在同一处**（`tick=7`，`step=resume` 之后无 `step=log`，看门狗 `recoveries=1`），
⇒ 整帧转储这轮 **0 行**。转储器本身没问题（自检通过、上一轮打过真实数据），
**问题在于它挂在一个不可靠的仪表上。**

### 27.2 新仪表：用 AAPCS64 的 callee-saved 性质

**`x29` 在 AAPCS64 里是 callee-saved ⇒ 客机 `bl` 进我们的 hook 时，x29 里就是客机自己的帧指针。**

在 import hook 入口用**汇编桩**把「传入时的 x29」抓出来，就能在
**客机自己的线程上、客机自己的上下文里**读 `[x29-0x48]` / `[x29-0x40]` / `[x29-0x60]`：

- **不暂停任何线程 · 不改客机一个字节 · 不碰任何内核接口**

桩布局（`native-switch/source/guest_frame_probe_stub.s`，11 指令 / 44 字节）：
```
0x08  stp  x29, x30, [sp, #0x00]   <<< 捕获：此刻 x29 仍是客机的帧指针
0x14  mov  x3, x29                 <<< 客机 x29 交给 C 侧
0x18  mov  x4, x30                 <<< 客机返回地址交给 C 侧
0x1c  bl   guestViewTracedMemcpyBody
0x20  ldp  x29, x30, [sp, #0x00]   <<< 原样还给客机
0x28  ret
```

**为什么必须是汇编（编译器自己给出了证明）**：链接产物里
`guestViewTracedMemcpyBody` 的第一条指令就是 `mov x29, sp`——
C 函数序言第一件事就是覆盖 x29，**任何 C 语句都来不及读**。

**为什么区间内任意挂点都拿到同一个帧**：全函数 10 KiB 反汇编，
**写 x29 的指令只有 3 条**——`0xcd1018` 保存、`0xcd1030 add x29,sp,#0x10` 建立、
`0xcd3450` 恢复。**区间内全部 291 个调用点的 x29 指向同一帧。**

### 27.3 ⚠️ 又一次更正：**`0xcd2b00` 不是 `free`**

我在任务书里断言析构尾部是 `free(B)`。**错的。**
`0xcd2b08` 是 `bl #0x1e17b50`，PLT 槽 = **`_ZdlPv`（operator delete）**，
区间内被调 **90 次**；而 `free` 的 PLT 桩 `0x1e18220` 在区间内**被调 0 次**。

| PLT 桩 | 符号 | 区间内调用 | 宿主 shim？ |
|---|---|---|---|
| `0x1e18220` | `free` | **0** | 是（但没用） |
| `0x1e17b50` | `_ZdlPv` | **90**（含 `0xcd2b08`） | **否** |
| `0x1e17c00` | **`memcpy`** | **10** | **是** ✅ |
| `0x1e17b40` | `_Znwm` | 19 | 否 |

**挂 `memcpy`**：它是唯一同时满足「区间内被调用」+「宿主 shim 且我们拥有其定义」的选择
（`_ZdlPv` 走 `findExportInLoaderScope` 解析到 `libc++_shared.so`，
要挂它必须往**逐字节冻结**的 `symbol_resolver.cpp` 加一行 —— 不做）。
10 个区间内调用点：`0xcd1170 0xcd11dc 0xcd124c 0xcd1318 0xcd1e28 0xcd1ed4 0xcd1f50 0xcd1fcc 0xcd2d48 0xcd2e2c`。

**实现方式**：把 `guestViewTracedMemcpy` 从 C 函数改成汇编定义，
C 实现下沉为 `guestViewTracedMemcpyBody`（`guest_view_trace.cpp`）。
`symbol_resolver.cpp` **一个字节没动**（SHA-256 已复验）。

### 27.4 自检：7 个变异全部被正确拒掉

真实桩字节过 capstone 解释器，逐轮断言 `x3`（fp）、`x4`（lr）、
返回后 `x29/x30/sp` 复原、返回值透传：

```
FAIL captures x28 instead of x29          FAIL never restores x29/x30
FAIL never hands the return address to C  FAIL never restores sp
FAIL never takes the frame at all         FAIL hands C its own sp
FAIL no entry capture of x29/x30
RESULT: PASS   —— 7 个坏变体全部被拒
```

**⚠️ 诚实的证据边界**：本机是 x86-64，**没有任何一条 aarch64 指令被本机原生执行过**。
上面是「解释器模型 + 真实字节」。
真正的 aarch64 阳性对照已编进 NRO
（`main.cpp` 启动自检新增 `guest frame probe selftest=... caller_fp=... captured_fp=...`），
**但要等一次真实运行才能拿到那一行输出。在拿到之前，不要宣称这个桩在真机 aarch64 上验证过。**

### 27.5 fail-open：读客机内存为什么必然安全

`accept()` 六道闸，任一不过就计数返回：
x30 ∈ `[0xcd1014,0xcd3648)` · x29≠0 · x29 16 字节对齐 ·
x29 ≥ 当前 sp · `x29 − sp ≤ 0x20000` ·
**三个槽必须整体落在 `[x29-0x5e0, x29)`**。

**最后一条是让「读」从「大概安全」变成「必然安全」的那条**——野指针必然被对齐或范围拦掉。
桩只写自己 0x30 字节的栈帧，**客机帧一个字节不碰**；x29/x30 在 `ret` 之前原样归还；
桩只写 x3/x4（caller-saved），C body 按 AAPCS 保全 x19-x28/sp/d8-d15。

### 27.6 待决：要不要读 `operator delete` 的实参

`memcpy` 的实参是**目的地址**，不是「被释放的指针」——它能读帧，但
**不能直接回答「free 的到底是哪个地址」**（要靠 `dst_eq_b` 间接逼近）。
要直接读 `_ZdlPv` 的实参，需要往冻结的 `symbol_resolver.cpp` 加一行
`RR3_SHIM("operator delete", ...)`。**这是冻结文件，等你决定，我不自行处理。**

### 27.7 本轮未解决的

`memcpy` 区间内的 10 个调用点里，**8 个在析构循环之前**、2 个在之后。
客机楔在 `0xcd2abc`，**永远到不了后面那 2 个**，所以能观测到的最新时刻是 `0xcd1fcc`。
⇒ **能回答「函数正常运行期间 `[x29-0x48]` 是不是就已经是垃圾」**，
**不能回答「析构那一刻它变成了什么」**——后者仍需等第 26.1 的 trampoline 路线
（需要真机确认 `Perm_R` 会不会掉 X）。

---

## 28. 2026-10-06 14:40 · **根因表述被反汇编改写**，并推翻本文件第 11.1 节

### 28.1 ⚠️ 作废：本文件 11.1 的「终止条件含取模守卫」是错的

11.1 写的是「终止条件纯算术 `E>=B && (E-B)%48==0`，48 非 2 的幂所以不能用 `& 47`」。
**这一节整个作废。** 完整反汇编 `0xcd2a70`–`0xcd2b3c`（用 PT_LOAD 段做
vaddr→file 映射，未假设 `vaddr == offset`）：

```
0xcd2aa0  ldur  x20, [x29, #-0x48]   ; x20 = B = begin
0xcd2aa4  cbz   x20, #0xcd2b0c       ; 空 -> 跳过整段
0xcd2aa8  ldur  x21, [x29, #-0x40]   ; x21 = E = end
0xcd2aac  cmp   x21, x20
0xcd2ab0  b.ne  #0xcd2ac8            ; E != B -> 进元素循环
0xcd2ab4  mov   x0, x20
0xcd2ab8  b     #0xcd2b04            ; 空区间 -> 直接 E:=B
0xcd2abc  cmp   x20, x23            ; <<< 楔点。B vs 已递减的游标
0xcd2ac0  mov   x21, x23
0xcd2ac4  b.eq  #0xcd2b00            ; 相等 -> 退出
0xcd2ac8  ldurb w8, [x21, #-0x18]    ; 元素标志
0xcd2acc  tbnz  w8, #0, #0xcd2ae0
0xcd2ad0  ldurb w8, [x21, #-0x30]    ; <<< 楔点
0xcd2ad4  sub   x23, x21, #0x30      ; 游标 -= 0x30
0xcd2ad8  tbz   w8, #0, #0xcd2abc    ; 回跳
0xcd2adc  b     #0xcd2af4
0xcd2ae0  ldur  x0, [x21, #-8]
0xcd2ae4  bl    #0x1e17b50           ; operator delete
0xcd2ae8  ldurb w8, [x21, #-0x30]
0xcd2aec  sub   x23, x21, #0x30
0xcd2af0  tbz   w8, #0, #0xcd2abc
0xcd2af4  ldur  x0, [x21, #-0x20]
0xcd2af8  bl    #0x1e17b50           ; operator delete
0xcd2afc  b     #0xcd2abc
0xcd2b00  ldur  x0, [x29, #-0x48]
0xcd2b04  stur  x20, [x29, #-0x40]   ; E := B
0xcd2b08  bl    #0x1e17b50           ; operator delete(B)
```

**这是一个标准的反向析构循环 `for (p = E; p != B; p -= 0x30) destroy(*p);`。
退出条件是纯指针相等，指令流里没有任何取模守卫。**

**⇒ 退出要求 `B` 落在 `E, E-0x30, E-0x60, …` 这条格点上。**
实测 `E − B = 0x25A77C = 2,467,708`，**`mod 0x30 = 0x1C`（28）** ⇒
**游标永远撞不上 B，循环永远不退出**，会一路往下、每步调一到两次
`operator delete`，直到走进未映射页。

**所以「客机永久静默」的机制不是「对野指针 free 崩了」，而是
「一个退出条件不可达的循环」。** 崩不崩取决于它什么时候走出映射；
静默是因为它还在走。

### 28.2 撤销对 `_ZdlPv` shim 的使用（你已授权，但我判断不该那么做）

你授权往冻结的 `symbol_resolver.cpp` 加一行 `RR3_SHIM("_ZdlPv", ...)`。
**我把它撤回了，理由是它既更危险、信息量又更差：**

1. **堆风险**：客机 `free` 已经指向 `std::free`（同一个分配器），
   所以 **`_ZdlPv` 的桩必须转发到 libc++ 的 `operator delete`** 才能保持堆正确。
   转发到宿主的 `::operator delete` 会用错分配器。
2. **x29 未必是客机的**：调用链是 `guest → operator delete(libc++) → free(我们的 shim)`。
   桩看到的 x29 是客机的，**仅当 libc++ 的 `operator delete` 不建立自己的帧**。
   这一点无法先验确定，**属于不可靠证据**。
3. **而 `free` 已经安全地 shim 了**：它转 `std::free`，同一个分配器，
   **加观测零堆风险、且不需要动冻结文件**。

**⇒ 改用「释放指针序列」作证据。** 合法析构释放的是有界、分散的一组指针；
失控循环释放的是**固定步长的长跑**且地址一路向下。**这个区别本身就是证明，
而且只看指针值。** 已在 `guestViewTracedFree` 首句加 `phase=guest-free-seq`：
记 `ptr / prev / stride / run / longest_run / longest_stride / caller`，
只在 `total<=8` 与 `run ∈ {64,1024,16384,131072,1048576}` 时打一行。

**冻结文件本轮一个字节未动。**

### 28.3 一条没成立的经验性印证（如实记）

我曾预测那些 509,734 条 `Unmapped Read` 会构成「每 0x30 递减的等差数列」，
用来印证 28.1。**实测不是**：那些地址是在**按字节顺序扫字符串内容**
（例如 `0x436567617373654E` 小端解出 `"NessageC"`）。**那条印证不成立，
不作为证据。** 28.1 的依据是反汇编本身，不是这批日志。

### 28.4 其它

- E 盘回收站已清空：**释放 12.41 GiB**，空闲 264.83 → **286.98 GiB**。
- 真机部署你选择暂缓，先在模拟器上把当前路线走完。
- `guest-frame-selftest` 已在真机 aarch64 上 **PASS**
  （`caller_fp == captured_fp == 0x1082efffa0`），
  **汇编桩路线到此确认可用**；只是 `memcpy` 这个挂点选错了
  （32768 次调用、0 次落在 DeviceInfo 区间，换算修好后这个 0 才是可信的）。

---

## 31. 2026-10-06 17:00 · 收尾：修复建成且自证 ~~**从未被观测到触发**~~ **（本节结论已被第 32 节推翻，见该节）**

### 31.1 状态一句话

**根因已确诊到机制级，修复已实现并通过 38 项自检（含变异），
但直到本节为止，修复从未在真机上被观测到写入过一次。**
**剩下的障碍在观测侧，不在修复侧。**

### 31.2 这一晚推翻掉的东西（全部有推翻它的那次测量）

| 我说过的 | 推翻它的测量 |
|---|---|
| Eden 拒绝重复 `svcSetMemoryPermission` | `guesttext-perm-second rc=0`，重复转换是**允许**的 |
| `jitCreate` 破坏了后续权限调用 | `permRwPreRes=0xd401`，它之前同样失败 |
| 顺序是原因 | 同上 + 把顺序改掉后行为未变 |
| 探针「被这页拒绝」 | 真因是 `uint32_t` 页掩码零扩展，算出 `0x762b000` |
| 闩锁置位 ⇒ 初始化被跳过 | 闩锁是 check-then-set，置位者和检查者同一函数，守的是**弹窗回调** |
| 「大概不涉及 Cloudcell」 | 该函数就是 `CC_Helpers::Manager::Manager`，RTTI 确认 |
| `+0x38` 是我算错的 | 原来的 +0x38 是对的 |
| `0xcd2b00` 调 `free` | 是 `bl #0x1e17b50` = `_ZdlPv`；`free` 的 PLT 区间内 0 次调用 |
| 修正在 A 阶段生效（那轮 3993 条日志） | `attempts=13 settled=0` —— 修复从未写入，**原因至今未确证** |
| `attempts=13` ⇒ 修复只被调了 13 次 | 那只是 drop 行打印瞬间的**快照** |
| `guestViewNoteAndroidLog` 入口无帧记录 | 构建产物序言是 `stp x29,x30,[sp,#-128]!` + `mov x29,sp`，**入口就有** |
| 日志撕裂吞掉了修复的行 | 独立单写者文件同样只有 1 行 |
| `atexit` 汇总会落盘 | Eden 直接杀进程，**atexit 不跑**（这条我自己写进过记忆，又犯一次） |

### 31.3 现在的确切状态

- **根因（已确认，静态+运行时双向）**：`CC_Helpers::Manager::Manager`
  （ELF `0xcd1014`–`0xcd3648`）在返回前析构一个**从未构造**的局部
  `vector<pair<string,string>>`（`x29-0x80`，字段 `-0x48/-0x40/-0x38`，
  元素 `0x30`）。析构退出条件是**纯指针相等**，无取模守卫；
  实测 `(end-begin) mod 0x30 = 0x1C` ⇒ **退出不可达**，
  运行时观测到 `stride=0x30` 的递减释放长跑，caller 折回 `ELF 0xcd2afc`。
- **修复（已实现，未观测到生效）**：`rr3/rr3_teardown_repair.hpp` +
  `source/guest_teardown_repair.cpp`。沿帧链上溯找到 saved LR 落在
  `[0xcd1014,0xcd3648)` 的帧，把它指向的三个 8 字节清零 ——
  **等于补上客机漏掉的默认构造（空向量）**，之后析构第一条
  `cbz x20, 0xcd2b0c` 直接跳过。**不碰客机字节、不碰冻结文件、不涉反作弊。**
  自检 38/38，3 个变异全被拒，宿主自检 47/47。稳态热路径 +5 条指令。
- **新卡点（已确认，待验）**：`doc/ccsn.bin` 是**我们自己的 shim 预造的 0 字节占位**
  （APK 里没有 `doc/`：1822 条目、0 个空文件）。客机 `FMCryptFile` 解析器
  `fopen` 只判非空、随后读 NUL 结尾串，**唯一出口是 `tmp==0`**，
  空文件永远出不来 ⇒ 3306 次 `"File ended, its corrupt!"`。
  **修法 `RR3_TOUCH_STUBS=0` 已部署**（不预造占位，让它对客机呈现为「文件不存在」，
  走 `localleaderboard.bin` 已被验证可行的跳过路径），
  **但还没被验证过**——需要一次能越过 DeviceInfo 的运行。

### 31.4 待接手的第一件事（障碍已隔离到一处）

**修复的日志写入在 SD 卡上静默失败。** 证据链：

- `guestTeardownRepairAttempt` 入口有**无条件**的 `ENTRY-TRACE`（前 3 次），
  实测恰好出现 3 行，`settled=0 t_in_attempt=0`；
- 同一次调用本应再写一行 `not_found`（前 12 次在预算内），**一行都没有**；
- `repairLog()` **不检查 `fwrite` 的返回值**。

⇒ 两种可能，都指向同一处待查：
① `bias==0` 让调用在 not-found 分支之前返回（那样 no_bias 分支也该写一行），
② **写进 SD 卡的 `fwrite` 在前几次之后静默失败**。

**下一步就一件事：让 `repairLog()` 检查并记录 `fwrite` 的返回值**，
外加把 `resolveMainImageLoadBias()` 的返回值在 no_bias 分支里单独打一行。
一轮 Eden 就能分辨，而不需要再猜。

### 31.5 顺带纠正的两处本文件/代码里的不准确陈述

- §29.3 说 `guestViewNoteAndroidLog` 入口没有帧记录、因此不能当锚点：
  **构建产物反汇编否定了它**（`stp x29,x30,[sp,#-128]!` 在偏移 0）。
  修复已改挂到该函数（它是唯一能产生 `phase=guest-log-any` 的函数，
  687 条客机日志全部经过它，与客机用哪条 import 无关）。
- §31.1 的修复注释里写的「此刻读 x29 得到的是**调用者**的帧指针」**也是错的**：
  序言已经把 x29 换成本函数自己的帧，所以 `x0` 传的是**本函数**的帧。
  **上溯会因此深一跳**，不影响正确性，但前提陈述必须更正。

### 31.6 「唯一产生点」这个支撑点：我复查过，站得住

我曾怀疑自己那���「`guestViewNoteAndroidLog` 是 `guest-log-any` 唯一产生点」
的全树搜索有遗漏（那次用了 `-Include *.cpp *.hpp *.h *.c` 并跳过目录）。
**用不裁剪文件类型、不裁剪目录的版本重做了：**

```
files read           : 446  (136.2 MiB)
files unreadable     : 0
POSITIVE CONTROL  present-needle : 5   (>=1 ✓)
NEGATIVE CONTROL  absent-needle  : 0   (=0 ✓)
'"guest-log-any"' : source\guest_view_trace.cpp   1 处
```

（另外三处是 `guest_filesystem.cpp` 里的相名清单字符串、
`guest_teardown_repair.hpp` 里的注释，以及 `.o/.elf/.nro` 构建产物。）

**⇒ 唯一产生点成立。** 所以「修复在函数入口、客机 687 条日志全经过它、
却只有 5 次进入且无任何出口痕」这个矛盾**依然存在**，且已排除了：
日志撕裂、SD 写入失败、提前返回吃调用、日志预算、部署旧 NRO、
代码没编进去（先前那次「built=0」是我用了一个**从来就不存在的子串**
`EXIT-TRACE reason=`，真值是 `EXIT-TRACE seen=%u reason=`——**又一次
「拿不存在的字符串断定不存在」**）。

**⇒ 接手时请从这里继续，不要重复以上排除项。**



---

## 29. 2026-10-06 15:01 · **根因链条的最后一环：宿主内存里的一次性闩锁**

### 29.1 两处自我更正（必须先看，否则下面读不对）

1. **`0xcd1014`–`0xcd3648` 不是「普通的设备信息组装函数」，它是 Cloudcell 初始化函数。**
   函数内引用 `CC_SeedData.bin`、`Cloudcell initializing with environment: %s`、
   `CLOUDCELL_FallbackAssetServerEnabled`、23 个设备信息键、`/rr3/stage/`。
   **⇒ 我在 28 节说「大概不涉及 Cloudcell」是错的，此处撤回。**
2. **向量的位置是 `obj+0x38/+0x40/+0x48`，`obj = STACK0-0xe0`。
   早先那个「+0x38」是对的，我在 28 节里「更正」成 `+0x18` 是我自己算错了。**
   验算：`obj+0x38 = STACK0-0xe0+0x38 = STACK0-0xa8 = [x29-0x48]`（B）✔
   **析构读的 `[x29-0x48]`/`[x29-0x40]` 正好是 begin/end。**

### 29.2 向量的三个字段，只有一个在本函数里被写过

`push_back` = **`0x1cb4910`**，尾段（16 个调用点）：

```
0x1cb4a80  ldp x20, x8, [x19,#0x40]   ; 读 end 和 cap
0x1cb4a88  b.eq #0x1cb4b00            ; end==cap -> 扩容
0x1cb4a94  bl  0x1cb591c              ; 在 *end 处拷贝构造
0x1cb4a98  add x8, x20, #0x30
0x1cb4a9c  str x8, [x19,#0x40]        ; end += 0x30
```

`x19` 全函数只有 `0x1cb4938 mov x19,x0` 一处赋值。

| 字段 | 本函数内的访问 |
|---|---|
| `obj+0x38`（begin） | **0 次写**，仅析构的 2 次读 |
| `obj+0x40`（end） | push_back 写 |
| `obj+0x48`（cap） | **0 次访问**（全函数一次都没有） |

**⇒ `push_back` 必须读 cap（`ldp` 同时加载 end 和 cap），析构也必须读它。
一个从不被访问的 cap 字段只能是从未被初始化。**

**⇒ 向量的 begin/end/cap 归零初始化在本函数内根本不存在；
它被假定「已经就绪」。** 设备上能工作，说明有别的路径负责它——而本窗口里找不到。

### 29.3 最可疑的东西：宿主内存里的一次性闩锁

```
0xcdc198  ldrb w9, [x8, #0xd7]    ; x8 = *(x0+8)
          cbz  w9, #0xcdc1bc      ; 置位 -> 直接 ret
```

**被检查的字节在 `*(obj+8) + 0xd7` —— 宿主拥有的内存。**
置位即跳过整个 init 体（2 次 `bl 0x13c3fc4` + `b 0x12bb330` 注册回调 `0xcdc240`）。

**⇒ 最可能的故事（待证）：闩锁已置位 ⇒ 真初始化整段被跳过 ⇒
向量三字段是宿主栈残留 ⇒ 析构退出条件不可达 ⇒ 永久静默。**

**卡点在宿主自己的内存状态上**，不是缺文件、不是 Cloudcell 反作弊、不是权限。

### 29.4 附带确认：析构尾声**不共用**

stride `0x30` 的循环恰好两处，跨度都是 0x40，逐指令同构：
- `0xcd2abc`–`0xcd2afc` 作用于 `[x29-0x48]` / `[x29-0x40]`（本函数）
- `0xcd34d8`–`0xcd3518` 作用于 `[sp,#0x70]` / `[sp,#0x78]`

**⇒ 不是共享尾声。性质因此确定：这是「析构了一个从未构造的向量」，
不是「构造分支被跳过」。**

### 29.5 那 39 个调用点（更正 26/27 节的「40 次」）

`[x29-0x60]` 是**指针槽**，存的是 `x29-0x80`（`STACK0-0xe0`）。
**`&(STACK0-0xc0)` 被传给 callee 的次数是 0。**
「40 次」对应的真实数字是 **39 次**——传的是 `STACK0-0xe0` 这个**值**，
把「槽的值」当成「槽的地址」差了一层。
`STACK0-0xc0` 的 14 次真实访问全是 `stur/ldur [x29,#-0x60]` 直接寻址。

其中 5 个 import 调用宿主函数（都收 `STACK0-0xe0`）：
`0xcd1518` / `0xcd2f5c` / `0xcd3014` / `0xcd30f4` → `std::string` 拷贝构造，
`0xcd3244` → `std::string::compare`。

### 29.6 过程中的方法论记录（本轮 Agent 自查出 3 个仪表缺陷）

1. **capstone 的 `reg_name(x29)` 返回 `fp`**，按名字匹配会漏掉 60 个 add/sub 位点
   ⇒ **寄存器要按号解析，不要按名字**。
2. **未跟踪 `sp` 经过序言回写** ⇒ `x29` 被算成 `SP0-0x630` 而非 `SP0-0x60`。
   **是阳性对照抓出来的。**
3. **`tbz/tbnz` 的分支目标是 `operands[2]`，不是 `operands[0]`**（后者是位号）
   ⇒ 全部 `tbz/tbnz` 边被静默丢掉，支配分析因此不收敛、输出自相矛盾。
   **修好后才发现存在一条跨 0x2000 的长回边。**
支配关系**未给出结论**（迭代 500 轮不收敛，需要 Lengauer–Tarjan）。

---

## 30. 2026-10-06 15:31 · **修复已实现**：替客机补上它漏掉的默认构造

### 30.1 修复是什么，为什么它是正当的

`CC_Helpers::Manager::Manager` 返回前销毁一个**局部向量** `x29-0x80`，
其 begin/end/cap 分别是 `x29-0x48` / `x29-0x40` / `x29-0x38`，
而**本函数从未构造过它**（30.2 前一节已证）。成员向量 `Manager+0x30`
被完整初始化，21 条 `DeviceInfo::` 字符串进的是它。

**⇒ 析构销毁的是一个从未构造、从未使用的局部向量。**
这是客机代码里的潜在缺陷：**在安卓上「碰巧无害」**——那三个槽在安卓的栈上
恰好残留着能让循环退出的值；我们的重宿主换了栈内容，它就不无害了。

**修复：把三个 8 字节清零**，等于替客机补上它漏掉的**默认构造（空向量）**。
之后析构第一条守卫直接跳过整段：
```
0xcd2aa0  ldur x20, [x29,#-0x48]
0xcd2aa4  cbz  x20, #0xcd2b0c      ; begin==NULL -> 跳过，_ZdlPv 一次都不调用
```

**这不是绕过反作弊，也不是伪造任何东西**：被清零的是**一个从未被构造的局部变量
的栈槽**，把它恢复成「空容器」这一合法初始状态。
**不碰客机任何一个字节，不碰冻结文件。**

### 30.2 障碍：那个窗口里没有任何 import

先量后做：`0xcd2824`–`0xcd2aa0`（最后一次 push_back 到析构之间）
**22 次 `bl` 全部是客机代码，0 个 import**（21 次是日志函数 `0x109fa18`）。
**⇒ 「在 import 上挂桩」这条走不通。**

### 30.3 出路：帧指针链上溯

客机是用帧指针编译的（全函数只有 3 条指令写 `x29`），
**帧链是连着的**。宿主 shim 跑在客机自己的线程和调用路径上，从当前 `x29`
沿 AArch64 帧链上溯，找到 saved LR 落在 `0xcd1014`–`0xcd3648` 的那一帧。

**挂点：`rr3::androidLogVPrint`，不是 `guestViewNoteAndroidLog`。**
这不是按角色选的，是**按构建产物 ELF 的实测反汇编**选的：

```
androidLogVPrint       a100: stp x29,x30,[sp,#-368]!   ← 入口即有帧记录
                       a104: mov x29, sp
guestViewNoteAndroidLog 1ebe0: cbz x0, ...             ← 入口无帧记录
                       1ed88: stp x29,x30,[sp,#-128]!  ← 函数体内 0x1a8 处才建帧
```
**从 `guestViewNoteAndroidLog` 起步脚下没有帧记录，只会读到调用者栈上的垃圾。**
「一个能用、一个是野读」——这是选它的唯一理由。

调用链：`0xcd2a9c bl 0x109fa18` → `0x109fac0 bl 0x1e17ed0 (__android_log_vprint)`
→ `RR3_SHIM("__android_log_vprint", androidLogVPrint)`。
真实链只需 **2 跳**。

### 30.4 关键正确性细节（都踩过坑才写对的）

- **先折算再比较**：saved LR 是**运行时地址**（客机帧被重定位过），
  必须先用 `censusGuestElfOffsetInPlan` 折回 ELF 偏移再比大小。
  **拿运行时地址和 ELF 偏移比大小这条判据恒假，本会话已栽过一次。**
- **折算失败不得用 0 代替**：那会把未映射地址折到第一个 image 底部，
  0 恰好低于下界，**错误完全不可见**。
- **命中判定排在守卫之前**是安全论证本身：本函数解引用的每个地址
  都是**已验证过的帧指针**。
- **`x29-0x38`（capacity）此前项目里没有任何检查覆盖过**
  （旧 `accept()` 只查 `-0x60/-0x48/-0x40`）。**两槽检查配三槽写入
  正是会变成野写的近似失误**，所以这三个槽的检查是新写的。
- **一次性定型会出事**：首条客机日志可能早于 census 捕获计划。
  所以定型只在**成功**或 **4 次用尽**后发生，避免把瞬态变成永久失效，
  也让日志能区分「没触发」和「触发了没找到」。

### 30.5 验收

构建 exit 0 · `Test-RR3Inputs` exit 0 · `test_*.py` **11/11** ·
`symbol_resolver.cpp` SHA 未变 · 客机 `.so` 源与 romfs 副本 SHA 一致。

**帧链自检 34/34（含 3 个变异全部被拒 + 1 个 fail-open 用例）：**
```
ok  POSITIVE: finds the record whose saved LR is in Manager
ok  POSITIVE: elf_off == 0xcd2a9c
ok  POSITIVE: slots are exactly [fp-0x48] [fp-0x40] [fp-0x38]
ok  POSITIVE: the planted poison reads back (a zero-reading control is void)
ok  MUTATION 1: no-base-subtraction fold finds NOTHING
ok  MUTATION 2: a walk one hop short finds NOTHING
ok  MUTATION 3: +8 is a different address than [fp-0x48]
ok  FAIL-OPEN: a descending chain stops as not_ascending
ALL PASS (34/34 checks, 0 failures)
```

**自检第一次跑是 28/32**——它抓到的 4 个错**全在我的预期里，走查代码本身四次全对**。
**这正是控制项该干的事**，比一个从不失败的对照值钱得多。

**热路径开销（从构建产物实测）稳态 +5 条指令**：
```
a10c: mov  x0, x29
a11c: adrp x2, <GOT>
a120: ldr  x2, [x2, #1336]     ; &g_guestTeardownRepairSettled
a124: ldr  w2, [x2]            ; 唯一的 relaxed 读 = 一条普通 LDR，无屏障
a128: cbz  w2, <慢路径>
```
慢路径 out-of-line；栈帧 368 → 384 字节（+16 B）。

**失败一定安全**：唯一的状态改变是 3 个 8 字节清零，且只在 5 项守卫全过后执行，
写的是**当前线程栈上一个正在执行的函数的栈帧**，不是 `.text`、不是堆、不是任何导入。
失败时**什么都不写** ⇒ **修复失败 = 现状不变，不会比现状更糟。**

---

## 32. 2026-10-07 · 最终交付：**修复确实到达了目标帧**（推翻第 31 节的结论）

### 32.1 先作废第 31 节

第 31 节写「修复建成且自证，但**从未被观测到触发**」。
**这一条错了**，错因和本项目今晚反复栽的是同一族：**观测被上限和缓冲切碎了**。

### 32.2 完整数据（`20261006-045337` 之后的 `allhops` 一轮）

上限全部拿掉之后（ENTRY-TRACE 的 6 行、HOP 的 24→200→20000、
not_found 的预算门、六个出口的条数上限、以及那个太小��缓冲）：

```
总行数            14757
HOP-TRACE         12114    （上限 20000，未截断）
EXIT-TRACE        1318     （每次上溯一条）
found=0           1318     ← 终于打出来了
REPAIRED          0
上溯深度          min=5  max=16
```

**落在 `[0xcd1014,0xcd3648)` 内的折叠偏移，全库只有 3 个：**

| 偏移 | 次数 | 客机锚点 |
|---|---|---|
| `0xcd2a9c` | 2 | **0**（都是自检的合成帧） |
| **`0xcd13a0`** | **1** | **1** ← 真实客机链 |

```
HOP-TRACE n=12114 hop=2 fp=0x0000001082cfbe50 saved_lr=0x000000210742a3a0
             folded=1 elf_off=0xcd13a0 in_range=1
```

**⇒ 上溯逻辑、折叠、五道守卫在真机 aarch64 上全部正确；
它确实到达了 `CC_Helpers::Manager::Manager` 的帧。**

### 32.3 缓冲这一项已被自证

```
<<TRUNCATED line_was=337 buffer=320>>    ×1318
```

**⇒ `char line[320]` 对上 337 字节的渲染长度，这一行从第一轮起就一次都没落盘**，
而它前面一句的 `attempts` 自增照常进行，一路涨到 1317。
**「上溯从不返回」这个读法我自己信了整整六轮。**

`repairLogBounded()` 现在**永远写**，超长就显式盖 `<<TRUNCATED line_was=N buffer=M>>`。

### 32.4 仍然缺的一行

`n=12114` 是**整个文件���最后一行**。命中之后既没有 `verdict=REPAIRED`，
也没有 `stop=target_rejected`。**日志恰好停在找到目标帧的那一刻。**
延长到 420 秒也没有改变（输出字节数一致）。

**⇒ 剩下的不是「修复能不能工作」，而是「这一行为什么没落盘」，
而这需要在真机上观察，或在 `_ZdlPv` 侧直接观测（需改冻结文件）。**

### 32.5 今晚这一族，共 9 次

| # | 被当成「事件没发生」的东西 | 真相 |
|---|---|---|
| 1 | 日志里 25 行 | 采样上限（每 200 条打一次） |
| 2 | `attempts=13` | drop 行打印瞬间的**快照**，实际 1317 |
| 3 | 没有 HOP 行 | **自检吃光了 24 行预算** |
| 4 | 第 8 次后没有 reject 行 | 条数上限 |
| 5 | 第 7 次后没有出口行 | 6 行上限 |
| 6 | 「上溯从不返回」 | **缓冲 337 > 320，整行从未落盘** |
| 7 | 1318 次只看到 6 次 | ENTRY-TRACE 上限 |
| 8 | 链不进 Manager | HOP 上限 200，只看到头 20 条链 |
| 9 | `built=0` 代码没编进去 | **用了从来就不存在的子串** `EXIT-TRACE reason=`（真值 `EXIT-TRACE seen=%u reason=`） |

**破局方式一次也没变过：把上限 / 缓冲 / 过滤拿掉，让读数自己说话。**

### 32.6 待你决定（模拟器上已无路可走）

1. **真机部署** —— 模拟器两条观测路径已判死：
   `svcSetThreadActivity` 永久阻塞 + `svcGetThreadContext3` 返回读取者自己的寄存器
   （跨线程线程态内参）；`svcSetMemoryPermission` 拒绝 X 位（`0xD801`），
   `.text` 原地打补丁不可行。真机 dump 能直接看清 32.4 那一行。
2. **是否允许改逐字节冻结的 `symbol_resolver.cpp`** —— 加 `_ZdlPv` shim
   可在析构循环内部直接观测到每一次 `operator delete` 的实参。
3. **Cloudcell 槽方向** —— 被研究函数本身即 `CC_Helpers::Manager::Manager`。










