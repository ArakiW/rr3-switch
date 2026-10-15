# 移植路线与验证标准

> 本文的 Switchroot Android 路线已被用户明确排除，仅作为早期可行性记录保留。当前实施路线是 [Horizon OS 原生逆向移植](REVERSE_ENGINEERING.md)。

## 路线 A：Switchroot Android（当前推荐）

现有游戏核心是 AArch64，但它是 Android ELF 动态库，不是 Nintendo Horizon OS 可执行文件。Switchroot Android 能同时提供 Android framework、Bionic C 库、JNI、EGL/OpenGL ES、音频和 Java 服务，因此它是现有二进制最短的运行路径。

建议设备环境：

- 当前维护的 Switchroot Android / LineageOS；
- 至少 32 GiB 的 Android 数据分区，避免解压和首次运行时空间不足；
- 可用的 Google Play 服务环境（此 APK 声明了 Play Games、Billing、Firebase 等组件）；
- USB 调试，用于安装与收集日志；
- 首轮测试不超频，先建立稳定基线。

数据包优先级：

1. **DXT**：Tegra GPU 的首选试验包；
2. **ETC**：若日志显示 DXT/S3TC 扩展未暴露或游戏主动选择 ETC，再测试；
3. **ATC**：主要面向 Qualcomm/Adreno 路线，不适合作为 Switch 首选。

数据 ZIP 的根目录为 `files/`，形态与 Android 私有应用目录 `/data/user/0/com.ea.games.r3_row/files` 相符。Android 的应用隔离不允许普通 `adb push` 直接写入该目录，所以本项目只在设备明确支持 `adb root` 且用户显式传入 `-AllowRootAdb` 时部署离线数据。否则先只安装 APK，让应用自行创建/下载数据，或由用户提供合法、可访问的数据导入方式。

### 完成标准

只有以下项目都通过，才可称为“Switch Android 版可用”：

- APK 成功安装且 package manager 识别 `arm64-v8a`；
- `UnpackAssetsActivity` 能进入主界面，无持续崩溃循环；
- DXT 数据被识别，非反复下载或报资源校验失败；
- Joy-Con 左右摇杆、A/B/X/Y、L/R/ZL/ZR 能在比赛中工作；
- EGL 上下文建立，赛道画面没有黑屏、纹理错乱或 shader 编译失败；
- 音乐、引擎声和比赛音效正常；
- 至少完整跑完一场离线比赛，并记录帧率/温度/崩溃日志。

当前工作区没有连接 Switch，因此这里只能完成静态检查和部署工具验证，不能把设备运行结果标记为已完成。

## 路线 B：Horizon OS 原生移植

### 有源码时

这是正规的产品移植路线：在获得 Nintendo 开发者权限和游戏权利方授权后，用官方 SDK 重新编译 C/C++ 游戏核心，并逐层替换：

- Android Activity/JNI -> Switch 应用生命周期；
- EGL/GLES -> Nintendo 图形 API 或获许可的跨平台渲染后端；
- Android 输入/传感器 -> Npad/Joy-Con；
- FMOD Android 库 -> 已获 Switch 平台许可的 FMOD 构建；
- Java 网络、账号、支付、广告、通知 -> Switch 平台服务或删除不适用模块；
- 私有 Android 文件布局 -> Switch save-data/content 布局；
- 触控 UI -> 手柄焦点和电视安全区；
- 动态下载内容 -> 合规的应用内容/补丁/DLC 分发。

### 无源码时

需要的是兼容层或 clean-room 重实现，而不是普通“换编译目标”。现有主库直接依赖 Android/Bionic、JNI、EGL/GLES、FMOD、Nimble 和 ARCore；此外 APK 内还有大量 Java/Kotlin SDK。即使 AArch64 指令集相同，也不能在 Horizon 上直接装载。

在没有权利方授权、官方 SDK和中间件许可的情况下，本项目不生成 `.nsp`，也不修改签名、DRM、账号或服务器鉴权。

## 下一轮设备测试需要的信息

- Switch 型号：Erista（初代）、Mariko/OLED、Lite；
- 当前是否已经能启动 Switchroot Android；
- Android 版本与 build fingerprint；
- 是否能使用 `adb devices` 看到设备；
- 是否接受只做 Android 兼容部署，还是持有源码与正式 Nintendo 开发资质、要走原生商业移植。
