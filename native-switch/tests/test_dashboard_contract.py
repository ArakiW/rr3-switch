#!/usr/bin/env python3
"""Static contract checks for the formal native loader dashboard."""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    ui = (root / "source" / "ui_dashboard.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    makefile = (root / "Makefile").read_text(encoding="utf-8")

    for section in (
        "SYSTEM STATUS",
        "RUNTIME SELF-TESTS",
        "DEPENDENCY GRAPH",
        "LOADER DIAGNOSTICS",
        "NEXT ENGINEERING GATES",
    ):
        assert section in ui, section
    for control in ("L/R", "ZR", "Y", "X", "B", "PLUS"):
        assert control in ui, control
    assert "writeDashboardReport" in ui
    assert "renderDashboard" in main_source
    assert "summaryShown" not in main_source
    assert "333 strong" not in ui
    assert "APP_TITLE   := RR3 Native Rehost" in makefile
    assert "APP_VERSION := 0.22.4" in makefile
    assert main_source.count("consoleExit(nullptr)") == 1
    assert main_source.count("runWindowGraphicsProbe") == 1
    assert main_source.index("runWindowGraphicsProbe") < main_source.index("nativeUiInit()")
    assert "consoleExit(nullptr);\n            dashboard.graphics" not in main_source
    assert "--rr3-renderer" in main_source
    assert "--rr3-renderer-result=" in main_source
    assert "envHasNextLoad" in main_source
    assert "envSetNextLoad" in main_source
    renderer_branch = main_source.index('hasArgument(argc, argv, "--rr3-renderer")')
    renderer_call = main_source.index("runWindowGraphicsProbe", renderer_branch)
    renderer_return = main_source.index("return R_SUCCEEDED(returnResult)", renderer_call)
    native_init = main_source.index("nativeUiInit()")
    assert renderer_branch < renderer_call < renderer_return < native_init
    assert 'hasArgument(argc, argv, "--rr3-console")' in main_source
    assert "developerChord" in main_source
    assert "visiblePages = developerMode ? rr3::kDashboardPages : 2" in main_source
    assert "runGuestViewProbe" in main_source
    assert "guest_view attempted=" in ui
    assert "onViewRender" in ui
    assert "Press Y to restart into renderer mode" in ui
    assert "Exit cleanup:" in ui
    assert "unload attempted=" in ui
    assert "STAGED GUEST INITIALIZATION" in ui
    assert "guest_init attempted=" in ui
    assert "Guest TPIDR_EL0 scope" in ui
    print("PASS: dashboard pages, controls, dynamic gate text, report export, NACP metadata")


if __name__ == "__main__":
    main()
