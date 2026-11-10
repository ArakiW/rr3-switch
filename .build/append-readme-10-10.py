"""Append the frame-rate KNOB section to tools/README-hardware-test.md (UTF-8, no BOM).

WHY: the port's own overhead is now about a millisecond a frame; what is left
of the frame is the guest's own rasterisation, and the guest asks for that
through ITS OWN settings file.  So the knob the user can turn is in that file,
and it belongs in the README next to the measurement.
"""
import io
import sys

SECTION = """
## 10.10 60 帧：现在能动帧时间的是**游戏自己的画质档**

**先说结论（2026-11-10 实测）**：端口自己每帧只花约 1~2 ms（帧循环 0.4 ms、
资源解析 0 ms、JNI 记账 8 ms 且其中大部分其实是客机自己的 Java 方法体）。
**剩下的 60~170 ms 是客机自己的栅格化** ⇒ 想再快，动的是**客机的画质**，不是端口。

**卡上现在的设置是 `AA_LEVEL = 4`（4 倍抗锯齿）**：

```
sdmc:/switch/rr3-native-rehost/rr3-settings.txt
AA_LEVEL = 4        ← 改成 0 就是关掉抗锯齿
```

**⇒ 抗锯齿是每像素 4 次采样 ⇒ 它直接乘在客机每帧的栅格化开销上。** 这是唯一一个
「改一行就可能把帧时间砍掉一大截」的旋钮。

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
powershell -ExecutionPolicy Bypass -File .build\set-guest-setting.ps1 -Key AA_LEVEL -Value 0
powershell -ExecutionPolicy Bypass -File .build\set-guest-setting.ps1 -Key calidad -Value medio
```

**怎么确认有没有变快**（真机上的量法，仪器已经在卡上了）：比赛段的 `tt` 就是整帧时间。
`tt` 的中位数从 68 ms 降到 33 ms 就是 30 fps，降到 17 ms 就是 60 fps。
在 `rr3-native-rehost.guest-view.log` 里：

```powershell
$p = 'F:\switch\rr3-native-rehost.guest-view.log'
[System.IO.File]::ReadAllLines($p,[System.Text.Encoding]::UTF8) |
    Select-String 'guest-frame-census' |
    Select-Object -Last 5
```

那一行里 `tt=` 是整帧、`jni=` 是端口经 JNI 的开销、`fs=` 是资源解析、`gui=`/`twosq=`
是前端普查的**端口自带计数器**（不要再用正则去数日志里的字符串了 —— 那是上一版的做法，
会让「改日志」变成「改测量」）。

**Eden 上的天花板**：模拟器的 GL 路径本身就慢，真机走 NVN 会快不少。
**⇒ 60 帧是「真机 + 关抗锯齿（或降一档画质）」的目标，Eden 上量到的数字只是参考。**
"""

def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        text = handle.read()
    if "## 10.10" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", errors="surrogateescape", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
