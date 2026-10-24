#!/usr/bin/env python3
"""Static contract for Joy-Con -> ControllerManager JNI input bridge."""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    header = (root / "include" / "rr3" / "guest_input.hpp").read_text(encoding="utf-8")
    source = (root / "source" / "guest_input.cpp").read_text(encoding="utf-8")
    probe = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    java = Path(r"E:\AI\RealRacing3-switch\reverse\java\sources\com\firemint\realracing\ControllerManager.java")
    if not java.exists():
        java = None

    assert "prepareGuestControllerInput" in header
    assert "connectGuestController" in header
    assert "pumpGuestControllerInput" in header
    assert "SetButtonValueJNI" in source
    assert "SetJoystickValueJNI" in source
    assert "ControllerConnectedJNI" in source
    # Ordinals must match Java ControllerButtons enum order.
    assert "kBtnA = 0" in source
    assert "kBtnB = 1" in source
    assert "kBtnX = 2" in source
    assert "kBtnY = 3" in source
    assert "kAxisLThumbX = 0" in source
    assert "kAxisRTrigger = 5" in source
    # ZR IS ALSO THE THROTTLE. The guest's own profile binds the accelerator
    # to "button 0 (A) | axis 5 (RT) | button 7 (R2)", and the port maps ZR
    # onto button 7 -- which is what the profile's name says. But MEASURED,
    # button 7 does not accelerate: with only button 7 held the guest draws
    # its own "hold the brake to reverse" message, which is the guest saying
    # the car is NOT moving (archive 20261024-135257-eden-thr-btnR2; the
    # same run with button 0 injected has no such message). So on a real
    # Switch the right trigger would be dead, and it is the accelerator's
    # natural home. ZR therefore also drives the guest's button 0.
    assert "HidNpadButton_A | HidNpadButton_ZR" in source
    # Frame loop must pump input and keep EGL when workers live.
    assert "pumpGuestControllerInput" in probe
    assert "connectGuestController" in probe
    assert "glClearColor" in probe
    assert "view-egl-keep-workers" in probe
    assert "guestPthreadLiveCount" in probe
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    assert "view-probe-dirty-exit" in main_source
    if java is not None:
        text = java.read_text(encoding="utf-8", errors="replace")
        assert "SetButtonValueJNI(int i, boolean z, int i2)" in text
        assert "SetJoystickValueJNI(int i, float f, int i2)" in text
        assert "ControllerConnectedJNI(String str, String str2, int i, int i2)" in text
        assert "BTN_A" in text and "AXIS_LTHUMB_X" in text
    print("PASS: guest controller input bridge contracts")


if __name__ == "__main__":
    main()
