# Third-party code and licensing

This port is released under the **MIT License** (see `LICENSE`). MIT requires
that the copyright notice and permission notice accompany any substantial
portion of third-party code, so this file records where the bundled code came
from and under which terms. Everything in `native-switch/source` outside
`third_party/` was written for this project.

## Bundled third-party code

| Path | Origin | Licence |
|---|---|---|
| `native-switch/source/eap_port/third_party/glad.h`, `glad_egl.h` | Khronos `glad` generator output / Khronos headers | **MIT** or **Apache-2.0**, at your option |
| `native-switch/source/eap_port/third_party/stb_truetype.h` | [stb](https://github.com/nothings/stb) by **Sean Barrett** | **MIT** (upstream also offers public domain) |
| `native-switch/romfs/licenses/Noto-CJK-OFL.txt` | Noto CJK fonts, shipped for text rendering | **SIL Open Font License 1.1** |

All three are permissive and are compatible with distributing this port under
MIT. `Noto-CJK-OFL.txt` is shipped verbatim in the RomFS and must be kept with
any redistribution.

## The `reference/` directory

`reference/realracing3-native-arm/` is the **Real Racing 3 PortMaster port by
EapRules, licensed GPL-3.0**. It is kept here as a reference for the bionic
loader interface, the Android class stubs, and the asset layout.

It is **not** part of this project, and none of its source is compiled into
this build. `native-switch/Makefile`'s `SOURCES` lists only directories under
`native-switch/source`. The files under `native-switch/source/eap_port/` that
carry an EapRules interface name — `so_util_adapter.h`, `trace_adapter.h`,
`platform_adapter.h`, `crash_adapter.h` — were written for this project and
re-declare those interfaces against this project's own symbol resolver and
logging. They are not copies.

If any of those adapter files is ever replaced by a straight copy from
`reference/`, this port would become a GPL-3.0 work and the MIT `LICENSE` would
no longer be accurate. That would have to be fixed by re-deriving the
interface, not by re-licensing the repository.

## Game data

No Real Racing 3 code, assets, or data are committed to this repository. The
guest binary `libRealRacing3.so` and the game data volumes are taken from the
user's own copy of the application. The port loads them at runtime and
circumvents no protection, licence check, or DRM.

**WHAT THE PUBLISHED PACKAGE ACTUALLY CARRIES, MEASURED FROM THE RELEASE
THAT WAS DOWNLOADED BACK AND RE-VERIFIED** (v0.22.14).  The NSP is 50,535,128
bytes; its RomFS holds 35 files over 44,948,780 bytes, and those files
include the guest's own runtime:

    romfs/libRealRacing3.so   33,110,472   byte-identical to the APK's
    romfs/libfmodex.so         1,274,928   byte-identical to the APK's
    romfs/libc++_shared.so     1,014,704
    romfs/libNimble.so              5,824
    romfs/original/*             5,785,627   the .depot text
    romfs/ui/*                  3,150,232   the interface faces
    romfs/licenses/...              4,388   the OFL face's licence text

**THE PACKAGING SCRIPT REQUIRES THE GUEST BINARY TO BE THERE**
(`tools/Pack-NativeSwitchNsp.ps1` refuses to pack without
`romfs/libRealRacing3.so`), because the port has exactly one source for the
guest's C++ runtime and an NSP without it does not start.  The app-only zip
in the same release carries the same RomFS tree.

**WHAT NO RELEASE CARRIES: THE GAME'S DATA.**  The seven data volumes (the
game's textures, tracks, cars and audio) are not in any release, and the
largest published asset is the app package.  The volumes are the user's own,
mounted from a data pack they prepared; the port also accepts the loose
`.depot/` tree on the card, but the volumes are the tested path.

**AND THE CLEAN BUILD STILL EXISTS.**  `tools/Build-NativeSwitch.ps1
-CleanLoader` produces the variant that carries none of the above: its
RomFS holds only `libc++_shared.so`, the OFL-licensed Chinese bootstrap face
and that face's licence text, and the guest's own files are then found on
the card, where the user put them.  The port's code contains the *names* of
those files (it has to, to look for them); it contains none of their bytes.

Anyone producing a build from this source must supply their own copy of the
application to do so.
