"""Update the release notes for v0.22.9 to describe the REAL package.

WHY: the first release's notes described a PFS0 with two files in it and
called it an NSP.  It was not, and the installer refused it.  The notes have
to say what the package actually is now: three NCAs, built by the official
SDK and the user's own CNMT-digest packer.
"""
import io
import sys

path = sys.argv[1]
with io.open(path, "r", encoding="utf-8") as handle:
    text = handle.read()

replacements = [
    (
        "`rr3-native-rehost.nsp`（5.0 MiB）",
        "`rr3-native-rehost.nsp`（5.2 MiB）",
    ),
    (
        "| `exefs/main` | 5,247,970 B | 端口可执行体（NSO） |\n"
        "| `exefs/main.npdm` | 848 B | 元数据（title id `0x010000000000F00D`） |",
        "| `a04cc32b….nca`（Program） | 5,256,192 B | 端口可执行体 + `main.npdm` |\n"
        "| `9d5377d3….nca`（Control） | 150,528 B | 控制数据：名字 + 图标 |\n"
        "| **`ba129cfa….cnmt.nca`（Meta）** | 4,096 B | **内容元数据（CNMT）—— 缺它就报「未找到内容元数据」** |",
    ),
    (
        "NSP sha256：`5F4E463008B89BA5D27992FC68B784E0326D0ABEA7208F4FFC1D3441FEB0A141`",
        "NSP sha256：`F4B549EEF05B474C90014D6D0E0EA6F28100603A908FD0B42B72EBFAF75F1BA1`",
    ),
    (
        "zip sha256：`AAB20C801EC3E8419559B4EF1931B6A9B99EA140CA4FC77FDF61A96975CC0D08`",
        "zip sha256：见下方 release 资产",
    ),
    (
        "**⇒ NSP 里没有 RomFS，也没有图标。**",
        "**⇒ ⇒ 第一版给的是只有两个文件的 PFS0，**那不是 NSP**，安装器直接拒绝（未找到内容元数据）。"
        "这一版经过官方 SDK 18.3.1 + 你自己的 hacBrewPack（cnmt_digest fork），带齐了 Program / Control / Meta 三个 NCA。\n\n"
        "**⇒ NSP 里没有 RomFS。**",
    ),
    (
        "title id `0x010000000000F00D`",
        "title id `0x0100ac50f00d0001`",
    ),
]

for old, new in replacements:
    if old not in text:
        print("NOT FOUND: %s" % old[:60])
    text = text.replace(old, new)

with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
    handle.write(text)
print("release notes updated")
