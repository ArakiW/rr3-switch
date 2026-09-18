#!/usr/bin/env python3
"""Release-contract checks for the formal Switch NRO and embedded core DSOs."""

from pathlib import Path
import struct


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    nro_path = root / "rr3-native-rehost.nro"
    nacp_path = root / "rr3-native-rehost.nacp"
    nro = nro_path.read_bytes()
    nacp = nacp_path.read_bytes()

    assert nro[0x10:0x14] == b"NRO0"
    assert b"0.22.4" in nro
    assert b"0.22.3" not in nro
    assert b"--rr3-renderer" in nro
    assert b"--rr3-renderer-result=" in nro
    assert "简体中文本地化".encode() in nro
    assert b"--rr3-console" in nro
    assert b"Switching the default NWindow" not in nro

    assert len(nacp) == 0x4000
    assert b"RR3 Native Rehost\0" in nacp
    assert b"Native Port Lab\0" in nacp
    assert b"0.22.4\0" in nacp

    embedded_size = 0
    for name in (
        "libRealRacing3.so",
        "libNimble.so",
        "libfmodex.so",
        "libc++_shared.so",
    ):
        image = (root / "romfs" / name).read_bytes()
        assert image[:4] == b"\x7fELF", name
        assert image[4] == 2, name  # ELFCLASS64
        assert struct.unpack_from("<H", image, 18)[0] == 183, name  # EM_AARCH64
        embedded_size += len(image)

    assert len(nro) > embedded_size
    depot = root / "romfs" / "original" / ".depot"
    assert (depot / "text_cn-Hans.txt").read_bytes()[:3] == b"\x86\x5f\xed"
    assert b"Chinese simplified,cn-Hans" in (depot / "langs.utf8").read_bytes()
    transitions = (depot / "MenuTransitions.xml").read_text(encoding="utf-8")
    assert '<KeyFrame start="0" end="40"/>' in transitions
    assert '<KeyFrame start="0" end="90"/>' in transitions
    for font in (
        "ConduitITCPro-Regular.otf",
        "ConduitITCPro-Bold.otf",
        "EurostileLTStd-Bold.otf",
        "NotoSansCJKsc-RR3Bootstrap.otf",
    ):
        assert (root / "romfs" / "ui" / font).stat().st_size > 20_000
    print(
        f"PASS: NRO/NACP 0.22.4, native zh-Hans UI assets and external-data contract, "
        f"4 AArch64 core DSOs ({len(nro)} bytes)"
    )


if __name__ == "__main__":
    main()
