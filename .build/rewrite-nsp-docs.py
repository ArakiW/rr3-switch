"""Rewrite the NSP sections: README-hardware-test.md 10.12 and OPEN-ISSUES.md 9.44.

WHY: the first version of 10.12 described a PFS0 with two files in it and
told the user it was an NSP.  It is not: an installer refuses it with "no
content meta found" (未找到内容元数据), because the content metadata -- the
Meta NCA -- is missing.  The package now goes through the official SDK and
the user's own CFW packer, and carries Program + Control + Meta.
"""
import io
import sys

README_MARK = "## 10.12 现在出的是 NSP 了：怎么装"

README_NEW = """## 10.12 现在出的是 NSP 了：怎么装

**2026-11-11 起，出包改成 NSP。** NSP 装上去就是一个「游戏」，从主菜单直接点开；
NRO 每次都得先用 hbmenu 之类的启动器把它打开。

### 这个 NSP 里有什么（这一节是上一版写错的地方）

**⇒ 第一版我给的是一个只有两个文件的 PFS0（`exefs/main` + `exefs/main.npdm`），
那不是 NSP，是 exefs 镜像 —— 任何安装器都会拒绝它，报「未找到内容元数据」。**

**⇒ ⇒ 一个能装的 NSP 里有三个 NCA：**

| 容器内条目 | 是什么 |
|---|---|
| `a04cc32b….nca`（Program） | 端口可执行体 + `main.npdm` |
| `9d5377d3….nca`（Control） | 控制数据：`control.nacp`（名字）+ `icon_AmericanEnglish.dat`（图标） |
| **`ba129cfa….cnmt.nca`（Meta）** | **内容元数据（CNMT）—— 就是它，缺它就报「未找到内容元数据」** |

**⇒ Meta NCA 里的 CNMT 末尾 32 字节必须是它前面 160 字节的 SHA256。**
上游 hacBrewPack 只算了 CNMT 的一部分，所以 digest 对不上，安装器就认为
内容元数据不存在 —— **你用的那个 `cfw_packer_cnmt_digest_fork` 修的正是这一处**
（`cnmt.c`：hash 的是 CNMT body：header + extended header + content records）。

### 出包的两级流程（`tools/Pack-NativeSwitchNsp.ps1`）

| 级别 | 用谁 | 出什么 |
|---|---|---|
| 1 | **官方 Nintendo SDK 18.3.1**（`elf2nso` / `MakeMeta.exe` / `AuthoringTool.exe creatensp`） | `exefs/main`、`exefs/main.npdm`、控制数据，以及一个签了名的开发包（`checkintegrity` 通过） |
| 2 | **你的 hacBrewPack**（`cfw_packer_cnmt_digest_fork`，`--keyset` 指向你自己的 keyset） | 最终的 unsigned homebrew NSP |

**⇒ 两级都要，因为：官方的 MakeMeta 出的 NPDM 带 ACID，控制数据（名字和图标）
也是官方从 `.nmeta` 生成的；而最终那个 CNMT 的 digest 是你的 fork 算对的。**

**⇒ `.nmeta`（`native-switch/rr3.nmeta`）是这份 NSP 的身份**：title id
`0x0100ac50f00d0001`、名字「RR3 Native Rehost」、版本 0.22.9。
图标路径在仓库里是占位符 `@SDK_SAMPLE_ICON@`，打包时**只**在 stage 副本里解析到
本机 SDK 自带的样例图标（不重新分发图标）。

**⇒ hacbrewpack 的 stdout/stderr 全部丢掉，只取退出码** —— 它的报错信息里会带 key 派生值。

### 怎么装

1. **装 NSP**：用 NSP 安装器（Tinfoil / Awoo / NSz），或 hbloader + sigpatches。
   装完主菜单出现 **RR3 Native Rehost**。
2. **拷数据**：七个数据卷和 `.depot` 拷到卡的 `sdmc:/switch/rr3-native-rehost/`。
3. **拷控制脚本**：9 份 `rr3-*.txt` 放同一目录，`rr3-gfx.conf` 放 `sdmc:/switch/`。
4. **客机的 .so 和字体**：NSP 里没有这些（在卡上），端口会自己去
   `sdmc:/switch/rr3-native-rehost/` 找。

### 怎么自己验一眼包对不对

`tools/Pack-NativeSwitchNsp.ps1` 每次都会**把容器读回来**断言：条目必须正好是三个，
且其中必须有一个 `.cnmt.nca` —— 少了就报错，**不会出现「构建说成功、包却装不上」**。
"""

ISSUE_MARK = "### 9.44 The NSP package, the TPIDR_EL0 reading, and what the controlled round settled"

ISSUE_NEW = """
### 9.44 The NSP package, the TPIDR_EL0 reading, and what the controlled round settled

#### 9.44.1 THE NSP PACKAGE, AND WHY THE FIRST ONE WAS NOT AN NSP

**THE FIRST PACKAGE WAS WRONG, AND THE USER FOUND IT BY INSTALLING IT.**

**MEASURED 2026-11-11: the installer refused the first NSP with "no content
meta found" (Chinese: 未找到内容元数据).**

**=> ⇒ ⇒ AND THE FIRST PACKAGE WAS NOT AN NSP.**  It was a PFS0 with two
entries, `exefs/main` (the NSO) and `exefs/main.npdm` (the meta).  That is
an EXEFS IMAGE.  **An NSP that an installer accepts carries THREE NCAs:**

    a04cc32b....nca          Program  (the executable and its meta)
    9d5377d3....nca          Control  (control.nacp: the name; icon: the icon)
    ba129cfa....cnmt.nca     Meta     (THE CONTENT META)

and the third one is exactly what the installer says it cannot find.

**=> ⇒ ⇒ ⇒ AND WHY THE FIRST PACKAGE HAD NO Meta NCA IS A FACTORY RULE, NOT
A BUG:** `npdmtool` can only produce the NPDM, `elf2nso` only the NSO, and
neither of them can produce an NCA.  The tool that does is the packer, and it
needs a keyset.

**=> ⇒ ⇒ ⇒ ⇒ SO THE PACKAGE NOW GOES THROUGH TWO STAGES, BOTH OF THEM
ALREADY ON THIS MACHINE.**

*Stage 1 -- the official Nintendo SDK 18.3.1 (E:\\AI\\Nintendo SDK
18.3(1).1\\installed-sdk-18.3.1\\NintendoSDK):*

    elf2nso                      -> exefs/main
    MakeMeta.exe                  -> exefs/main.npdm
    AuthoringTool.exe creatensp   -> the signed development NSP
    AuthoringTool.exe extract     -> control.nacp + icon_AmericanEnglish.dat

The control is taken back OUT of the development NSP, because the SDK
generates it from the .nmeta and it carries the title's name and icon.
The development NSP passes `checkintegrity`, and the SDK reports the content
meta's digest: `08f4edbaa1ee42b3c310469e6f5ad633b6fb8fb02dbdd3e84f07dcb3855c319c`.

*Stage 2 -- the user's own CFW packer, the CNMT-digest fork:*

    hacbrewpack --keyset <the user's own keyset> --keygeneration 1 ...

**=> ⇒ ⇒ ⇒ ⇒ ⇒ AND THE FORK'S ONE-LINE FIX IS EXACTLY THIS BUG.**  The
fork's `cnmt.c` computes the content meta's digest over the CNMT BODY -- the
header, the extended header and the content records -- so the last 32 bytes
are SHA256 of the first 160.  Upstream hashes only part of it, the digest
does not match what the installer recomputes, and the installer reports the
content metadata as missing.  **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ THE ERROR THE USER HIT WAS THE
DIGEST, NOT THE MISSING FILE.**

**=> ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ AND THE PACKAGE IS CHECKED, NOT ASSUMED.**  The packing
script reads the container back and refuses to report success unless it holds
exactly three entries and one of them is a `.cnmt.nca`.

**=> ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ WHAT IS AND IS NOT IN IT: the executable, its meta and the
control.  NOT the game's data -- the seven volumes and the .depot tree,
about 11.8 GiB -- which has never been in the executable and is still copied
onto the card separately.**

#### 9.44.2 THE NEW CONSOLE DUMP ANSWERS THE SPAL QUESTION IN ONE FIELD

**MEASURED 2026-11-11, the dump the user attached after the run of
2026-11-10:**

    esr=0000000092000005
    pc=0000001abfbb96f4
    lr=0000001abfbb96f0
    far=00000059a492e5d0
    fault_tpidr_el0=000000018eff3e60
    sp=000000018eff39b0
    fp=000000018eff39e0

**=> THE EXCEPTION CLASS IS STILL SPAL** (0x24), and `pc = lr + 4` again.

**=> ⇒ ⇒ AND THE ONE FIELD THAT MATTERS IS `fault_tpidr_el0`: IT IS
0x18eff3e60, AND THE STACK POINTER IS 0x18eff39b0.**  The thread pointer that
names the thread-local area -- the value every PAC key derives from -- points
**1,200 bytes ABOVE THE STACK POINTER, INSIDE THE STACK FRAME.**

**=> ⇒ ⇒ ⇒ THAT IS THE PORT'S OWN FAKE TCB.**  `GuestTlsScope`
(`tls_probe.cpp`) fabricates a 256-byte block ON THE STACK and points
TPIDR_EL0 at it, and it is active across the whole guest-constructor window
(the loader's calls at `multi_dso_linker.cpp:2345`, `:2748`, `:2910`).  On a
console, TPIDR_EL0 must name a real thread-local area whose PAC key material
lives there; a stack address for TPIDR_EL0 means the guest's `autiasp`/
`paciasp` computes a PAC against the wrong key.

**=> ⇒ ⇒ ⇒ ⇒ AND THE GUEST'S CODE IS PAC-COMPILED** (1328 `paciasp` + 1372
`autiasp` in `libc++_shared.so`, section 9.41), so the guest signs and
authenticates pointers on every function entry and exit.  **=> THE PORT'S
FAKE TLS IS THE MOST LIKELY CAUSE OF THE SPAL**, and it is a port-side bug,
not a guest bug.

**=> ⇒ ⇒ ⇒ ⇒ ⇒ THE FIX IS NAMED AND SMALL: the port must not clobber
TPIDR_EL0.**  Save the host's value and restore it around the guest
constructor window, or give the guest a real thread-local block.  The next
console run is not needed to find that; it is needed to confirm the fix.

#### 9.44.3 THE CONTROLLED ROUND SETTLED THE RACE-COUNT QUESTION

**THE QUESTION.**  The start lights went 7 (chunk1..chunk3) -> 19 (fps1) ->
27 (yield2) -> **1** (slim1, census2, aa0, fx0, view960) -> 2 (pcall1buf,
loaderdiag1).  The obvious reading is "the port regressed".

**THE EXPERIMENT.**  `run-pinned-round.ps1` SNAPSHOTS the guest's state and
never RESTORES it, so every round began from whatever the previous round left
behind -- and the guest advances to the next event after each race and writes
its popups into the save.  So the round `ctl1` restored
`.build/guest-state/yield2` (the state that produced the 27) and ran the SAME
binary as `loaderdiag1` for the same 400 seconds.

    round          state            binary      event  lights  race  hud  scanf
    yield2         inherited        A           300    27      36    0     0
    loaderdiag1    inherited        B (9543C130) 168    2       2   88   13385
    ctl1           RESTORED yield2  B (9543C130) 167    2       2   88   13385

**=> ⇒ ⇒ AND THE STATE IS NOT THE VARIABLE.**  ctl1 put the SAME binary
back on the state that produced 27, and it came back at 2/2 -- the same
numbers as the round that inherited a drifted save.  **=> THE COLLAPSE IS
NOT THE SAVE.**

**=> ⇒ ⇒ ⇒ SO IT IS THE OTHER HALF OF THE A/B: the binary, or the logging
switch that moved with it** (`loaderdiag1` and `ctl1` ran with `percall=1`,
13,385 guest-scanf lines; `yield2` ran with `percall=0`, zero).  **=> ⇒ ⇒ ⇒ ⇒
AND THE PATH TRACE COLLAPSED IN THE SAME WINDOW** -- from 108,419 lines
(105,150 of them asset paths) to 80 -- when `logResolvedPathOnce()` was
thinned to `gui/processed/` only, and section 9.33 already recorded that the
port's own front-end navigation reads that trace.  **⇒ THE NEXT EXPERIMENT IS
ONE VARIABLE: the same binary with the trace restored.**
"""


def main():
    readme_path, issues_path = sys.argv[1], sys.argv[2]

    with io.open(readme_path, "r", encoding="utf-8") as handle:
        readme = handle.read()
    index = readme.find(README_MARK)
    if index < 0:
        print("README 10.12 not found")
        return 1
    readme = readme[:index].rstrip("\n") + "\n" + README_NEW
    with io.open(readme_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(readme)

    with io.open(issues_path, "r", encoding="utf-8") as handle:
        issues = handle.read()
    index = issues.find(ISSUE_MARK)
    if index < 0:
        print("9.44 not found")
        return 1
    issues = issues[:index].rstrip("\n") + "\n" + ISSUE_NEW
    with io.open(issues_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(issues)

    print("rewrote README 10.12 and 9.44")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
