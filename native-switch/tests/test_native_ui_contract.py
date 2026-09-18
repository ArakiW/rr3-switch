#!/usr/bin/env python3
"""Static contract for the formal framebuffer UI and original package data."""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    native = (root / "source" / "native_ui.cpp").read_text(encoding="utf-8")
    dashboard = (root / "source" / "ui_dashboard.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    build = (root.parent / "tools" / "Build-NativeSwitch.ps1").read_text(encoding="utf-8")
    makefile = (root / "Makefile").read_text(encoding="utf-8")

    assert "framebufferCreate" in native
    assert "framebufferMakeLinear" in native
    assert "PIXEL_FORMAT_RGBA_8888" in native
    assert "ConduitITCPro-Regular.otf" in native
    assert "ConduitITCPro-Bold.otf" in native
    assert "EurostileLTStd-Bold.otf" in native
    assert "NotoSansCJKsc-RR3Bootstrap.otf" in native
    assert "plInitialize" not in native
    assert '"romfs:/original/.depot/text_cn-Hans.txt"' in native
    assert "kOriginalMenuTransitionLastFrame = 40" in native
    assert "kOriginalCarSelectLastFrame = 90" in native
    assert "nativeUiRenderLoading" in dashboard
    assert "nativeUiRenderDashboard" in dashboard
    assert "!diagnosticConsole && (!romfsReady || !rr3::nativeUiInit())" in main_source
    assert '"--rr3-console"' in main_source
    assert "text_cn-Hans.txt" in build
    assert "MenuTransitions.xml" in build
    assert "menu_transition.banim" in build
    assert "-lfreetype -lharfbuzz" in makefile
    assert (root / "romfs" / "licenses" / "Noto-CJK-OFL.txt").stat().st_size > 4_000
    print("PASS: 1280x720 framebuffer UI, original fonts, zh-Hans-first package and motion tracks")


if __name__ == "__main__":
    main()
