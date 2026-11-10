## RR3 Switch Native Port — 2026-11-11：出包改成 NSP，真机崩溃的根因已经在 dump 里了

原生 Horizon OS 移植。这一版只有两件事，但都是你之前卡住的地方：**装包方式换成 NSP**（装上就是主菜单里的一个游戏，不用再开 Homebrew Launcher），以及**真机那次 SPAL 崩溃的根因找到了，而且答案就在你传回来的那份 dump 里**。

### ⭐ 1. 出包改成 NSP

**`rr3-native-rehost.nsp`（5.2 MiB）**：装上以后主菜单里直接出现一个叫 **RR3 Native Rehost** 的标题，点开就进游戏。

NSP 里的东西（就这些，已逐条核对过容器结构）：

| 容器内条目 | 大小 | 是什么 |
|---|---|---|
| `a04cc32b….nca`（Program） | 5,256,192 B | 端口可执行体 + `main.npdm` |
| `9d5377d3….nca`（Control） | 150,528 B | 控制数据：名字 + 图标 |
| **`ba129cfa….cnmt.nca`（Meta）** | 4,096 B | **内容元数据（CNMT）—— 缺它就报「未找到内容元数据」** |

**⇒ ⇒ 第一版给的是只有两个文件的 PFS0，**那不是 NSP**，安装器直接拒绝（未找到内容元数据）。这一版经过官方 SDK 18.3.1 + 你自己的 hacBrewPack（cnmt_digest fork），带齐了 Program / Control / Meta 三个 NCA。

**⇒ NSP 里没有 RomFS。** 这不是漏了，是工具链的边界：`npdmtool` 没有 `icon` / `nacp` / `romfs` 这三个键（每个都试过，出来的 848 字节元数据一模一样），而且它**描述不了 RomFS 段**。

**⇒ 但没有 RomFS 不影响游戏跑。** 端口读自己的资源走 `resolvePortAssetPath()`：先试 `romfs:/X`，**找不到就退到 `sdmc:/switch/rr3-native-rehost/X`** —— 而客机自己的三个 `.so` 和 UI 字体**本来就在卡的那儿**。客机主库也一样：`selectGameLibrary()` 最后会落到 `sdmc:/switch/rr3-native-rehost/libRealRacing3.so`。

**⇒ ⇒ 打包时踩到的两个坑（都记在 §9.44 里了）**：

1. **libnx 自带的 `%.nsp` 规则做出来的是 exefs 镜像，不是 NSP。** 它 `build_pfs0 exefs`，而 `build_pfs0` 只打包输入目录的**第一层**（遇到子目录打印 `Directories aren't supported, skipping...` 就跳过）⇒ 容器里的条目叫 `main` / `main.npdm`。NSP 的条目必须叫 `exefs/main` / `exefs/main.npdm`，目录结构得在容器**里面**。所以容器是 `tools/make-pfs0.py` 自己写的，格式是从 `build_pfs0` 自己的输出里读回来的。
2. **打包完会把容器读回来核对**，`exefs/main` 和 `exefs/main.npdm` 少一个就直接报错 —— 不会出现「构建说成功、包却是坏的」。

### ⭐ 2. 真机那次 SPAL 崩溃：根因就在你传的 dump 里

**MEASURED 2026-11-11，你传回来的 `crash-early.txt`：**

```
esr=0000000092000005        ← 异常类别 0x24 = SPAL（指针认证失败）
pc=0000001abfbb96f4
lr=0000001abfbb96f0
fault_tpidr_el0=000000018eff3e60
sp=000000018eff39b0
```

**⇒ ⇒ ⇒ 关键就在 `fault_tpidr_el0` 这个字段：它比栈指针高 1200 字节，落在栈帧里。**

TPIDR_EL0 是「线程本地区」的指针，**PAC 密钥就是从它派生的**。而端口自己的 `GuestTlsScope`（`tls_probe.cpp`）**在栈上伪造了一块 256 字节的假 TCB，把 TPIDR_EL0 指过去**，而且这段作用域覆盖整个客机构造器窗口（装载器在 `multi_dso_linker.cpp` 的三处调用都在里面）。

**⇒ ⇒ ⇒ ⇒ 而客机的 `libc++_shared.so` 是带指针认证编译的**（1328 条 `paciasp` + 1372 条 `autiasp`）⇒ 客机每次函数进出都在签名/验签 ⇒ **端口伪造 TPIDR_EL0 ⇒ 客机算出来的 PAC 对不上 ⇒ SPAL。**

**⇒ 这是一个端口侧的 bug，而且修法很小：端口不能再乱改 TPIDR_EL0**（把宿主机的值存下来、客机构造器窗口结束后恢复，或者给客机分配一块真正的线程本地区）。

顺带把两件之前读错的事纠正了：
- `esr=0x92000005` 是 **SPAL**，不是「数据访问异常 / 地址翻译失败」（那是 `0x4E000005`）—— 之前的文档读错了，方向整个反了。
- 端口的 NRO **全库只有 2 条 PAC/AUT 指令**（都在 libunwind 里），而崩溃 `pc` 反推出的 NRO 基址**不是页对齐** ⇒ **`pc` 不在端口镜像里，在客机镜像里**。所以「崩溃在端口代码里」这个说法也不成立（§9.41 已按证据改写）。

**⇒ 下一版会把 TPIDR_EL0 的修掉，并附上新仪表**：`loader-diag` 行（每个客机 DSO 映射前的名字/段/偏移/权限，外加每个 DSO 的 **PAC 指令条数**）、以及崩溃 dump 里的 **`ec=`（解出来的异常类别）** 和 **`pc_attribution=` / `lr_attribution=`（出错的 pc 属于哪个 DSO、偏移多少）**。装载器的诊断行现在**逐行立即落盘**（日志平时 64 行批量落盘，崩溃时会把装载器自己的证据一起带走 —— 这正是上一次什么都没留下的原因）。

### 📊 3. 60 帧：端口侧已经没水分了，真机才是判据

**MEASURED 2026-11-10，10 轮归档的帧预算（渲染段，按阶段分开）：**

| 字段 | 含义 | 中位数 |
|---|---|---|
| `tt` | 整帧 | 40~66 ms（Eden） |
| `tr` | 客机自己的渲染 | **37~40 ms** |
| `ts` | 交换/呈现 | 0 ms |
| **`tp`** | **端口自己在帧循环里干的活** | **0 ms（每一轮、每一阶段都是 0）** |
| `jni` | 端口经 JNI 伺候客机 | 7~11 ms |

**⇒ 端口自己的开销是 0 ms，帧成本全在客机的栅格化上。** 所以 60 帧这件事**端口侧已经做完了**，剩下的是客机的画质档 —— 而那只能在真机上走：Eden 上实测过三个客机侧杠杆（关 AA、关 12 个高开销画质项、分辨率减半），**三个全落在轮间噪声里**（关 AA 反而更慢 72→83 ms），说明 Eden 的帧时间被模拟器本身主导。

**⇒ 真机上的判据还是同一个：census 行的 `tt`。** 中位数降到 16~17 ms 就是 60 fps。旋钮清单见[安装指南 10.10](https://github.com/ArakiW/rr3-switch/blob/master/tools/README-hardware-test.md)。

### 🎮 4. 游戏其实已经在比赛里了（画面实测）

这一轮的画面帧证明的事情比计数器多得多：

- **事件选择页**：三张每日比赛卡（保时捷 911 GT3 / 奥迪 R8 / 日产 GT-R）**贴图全对、零洋红**，中文标签正常。
- **3D 选车展厅**：F1 赛车完整渲染（涂装、尾翼、Real Racing 标识都对），右侧 `R / MR / PR` 级别、车手等级 110.3、四条性能条、中文提示框全对。
- **赛前发车格**：玩家的 NISMO GT-R（白红涂装、灯组正确），旁边**二十辆 AI 赛车**——维斯塔潘 1、佩雷兹 11、塞恩斯 55、勒克莱尔 16、科拉平托 43、周冠宇 24、奥孔 31、马格努森 20、斯特罗尔 18、皮亚斯特里 21、诺里斯 4、许尔肯贝格 27、角田毅 22、拉塞尔 63、阿尔本 23、加斯利 10、汉密尔顿 44、阿隆索 14 —— **涂装、号码、名字全对**，右侧排行榜也在画。

**⇒ 端口把客机开到「发车格前」这一整条路是通的。**

### 文件 / Files

| 文件 | 大小 | 说明 |
|---|---|---|
| `rr3-native-rehost.nsp` | 5.0 MiB | **装这个**。NSP 标题，主菜单直接点开 |
| `rr3-switch-app-20261111.zip` | 49.3 MiB | 应用包（NSP + NRO + RomFS + 9 份控制脚本 + 配置 + 安装脚本 + 中文指南） |

zip sha256：见下方 release 资产
NSP sha256：`F4B549EEF05B474C90014D6D0E0EA6F28100603A908FD0B42B72EBFAF75F1BA1`

**游戏数据卷（七个卷 + `.depot`，约 11.8 GiB）不在本发布内**，仍需按[安装指南](https://github.com/ArakiW/rr3-switch/blob/master/tools/README-hardware-test.md)准备。

### 部署 / Deployment

1. **装 NSP**：用 NSP 安装器（Tinfoil / Awoo / NSz），或 hbloader + sigpatches。装完主菜单出现 **RR3 Native Rehost**。
2. **拷数据**：七个数据卷和 `.depot` 拷到卡的 `sdmc:/switch/rr3-native-rehost/`。
3. **拷控制脚本**：9 份 `rr3-*.txt` 放同一目录，`rr3-gfx.conf` 放 `sdmc:/switch/`。
4. **客机的 .so 和字体**：如果之前没拷过，从你自己的应用副本取出放到 `sdmc:/switch/rr3-native-rehost/`（NSP 里没有这些，端口会自己去那儿找）。

### 回传日志 / Send logs back

跑完请回传：
- `sdmc:/switch/rr3-native-rehost.guest-view.log` —— 现在会多出 `phase=loader-diag` 行（含 `pac-census`），它直接说出是哪个客机 DSO 带了多少条指针认证指令。
- `sdmc:/switch/rr3-native-rehost.crash-early.txt` —— 如果还崩，现在多了 `ec=` 和 `pc_attribution=` / `lr_attribution=`，**一行就能说清崩的是端口还是客机**。

### 文档 / Documentation

- [主交接文档 docs/OPEN-ISSUES.md](https://github.com/ArakiW/rr3-switch/blob/master/docs/OPEN-ISSUES.md)（§9.41 改写、§9.42~9.44 新增）
- [主机安装指南（中文）](https://github.com/ArakiW/rr3-switch/blob/master/tools/README-hardware-test.md)（新增 10.12：NSP 怎么装、NSP 里没有什么）
- [逐轮时间线 PORT_PROGRESS.md](https://github.com/ArakiW/rr3-switch/blob/master/PORT_PROGRESS.md)
- [授权与合规说明 NOTICE.md](https://github.com/ArakiW/rr3-switch/blob/master/NOTICE.md)

---

*This project is for learning and research. It does not bypass DRM or forge accounts. The game binary and data in this build come from your own copy of the application — see [NOTICE.md](https://github.com/ArakiW/rr3-switch/blob/master/NOTICE.md).*
