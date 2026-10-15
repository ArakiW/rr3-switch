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

No Real Racing 3 code, assets, or data are distributed by this project. The
guest binary `libRealRacing3.so` and the game data volumes are taken from the
user's own copy of the application. The port loads them at runtime and
circumvents no protection, licence check, or DRM.
