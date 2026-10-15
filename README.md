# Real Racing 3 -> Nintendo Switch

这个目录目前包含 Real Racing 3 14.0.1 的 Android APK 与 ATC、DXT、ETC 三套游戏数据，**不包含游戏源码**。

> 当前目标已改为 **Horizon OS 原生逆向移植**，明确不使用 Android。已生成的 Java 与 ARM64 伪 C 结果、原生工程入口和验证状态见 [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md)。下面的 Switchroot Android 内容仅保留为早期兼容性调查记录，不再是实施路线。

## 当前可行目标

第一阶段采用 **Switchroot Android**：在 Switch 上运行原 Android ARM64 版本，并验证 Joy-Con、OpenGL ES、音频与数据加载。这是兼容部署，不是 Horizon OS 原生 `.nro/.nsp` 移植。

真正的 Horizon OS 原生版本需要满足以下任一条件：

1. 获得 EA / Firemonkeys 的游戏源码、第三方中间件许可（尤其 FMOD）以及 Nintendo Switch SDK；或
2. 从头实现 Android/JNI/EGL/GLES、Java 服务、输入、音频、文件系统和网络兼容层。仅凭现有 APK 无法直接重链接为 Switch 程序。

## 已确认的包信息

- 包名：`com.ea.games.r3_row`
- 版本：`14.0.1`（versionCode `14001`）
- Android：minSdk 26，targetSdk 36
- 原生 ABI：`arm64-v8a`、`armeabi-v7a`
- 图形：OpenGL ES 2.0 / EGL
- 主库：`libRealRacing3.so`，ARM64 ELF，依赖 Android `libandroid.so`、`liblog.so`、EGL/GLES、FMOD、Nimble、ARCore 等
- 启动 Activity：`com.firemint.realracing.UnpackAssetsActivity`
- 数据包：默认推荐 `DXT_960x640+2048x1536数据包-260530-7723.zip`，因为 Switch 的 Tegra GPU 对 DXT/S3TC（BCn）纹理路线最匹配
- APK 能通过 v2/v3 签名结构校验，但证书主体为 `Depot S`，不能据此认定为 EA 官方发布包；部署前应把它视为第三方重签名 APK
- 当前 APK SHA-256：`935FEE3124C8EAEFBF2EC1B1A6BD1A869812D2F0D1FA533E04539293075F455B`

## 工具

所有脚本默认只检查，不会修改 APK 或绕过游戏鉴权。

```powershell
# 1. 检查本地输入和数据包
pwsh -File .\tools\Test-RR3Inputs.ps1

# 2. 把 DXT 数据解压到一个新的空目录（约需 9 GiB）
pwsh -File .\tools\Prepare-SwitchAndroid.ps1 `
  -Destination E:\RR3-SwitchAndroid-Stage `
  -Extract

# 3. Switchroot Android 已启动并开启 USB 调试后，安装 APK
pwsh -File .\tools\Install-SwitchAndroid.ps1 -Launch

# 4. 可选：只有设备的 adbd 明确允许 root 时，写入私有 files 目录
pwsh -File .\tools\Deploy-RR3Data.ps1 `
  -StagePath E:\RR3-SwitchAndroid-Stage `
  -AllowRootAdb

# 5. 启动后收集可验证的运行日志
pwsh -File .\tools\Collect-RuntimeProof.ps1
```

详细边界与验证标准见 [PORTING.md](PORTING.md)。
