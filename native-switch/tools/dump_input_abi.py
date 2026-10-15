from pathlib import Path
import subprocess

so = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
objdump = r"E:\AI\RealRacing3-switch\.tools\devkitPro\msys64\opt\devkitpro\devkitA64\bin\aarch64-none-elf-objdump.exe"

ranges = [
    ("ControllerConnected", 0x12AA450, 0x12AA590),
    ("ControllerDisconnected", 0x12AA59C, 0x12AA604),
    ("SetJoystickValue", 0x12AA604, 0x12AA724),
    ("SetButtonValue", 0x12AA724, 0x12AA820),
    ("updateAccel", 0x12AB548, 0x12AB5B4),
    ("updateGyro", 0x12AB5B4, 0x12AB640),
    ("onTouchBegin", 0x12B39D8, 0x12B3A14),
    ("onTouchMove", 0x12B3A14, 0x12B3A58),
    ("onTouchEnd", 0x12B3A58, 0x12B3AC0),
    ("onTouchCancel", 0x12B3AC0, 0x12B3B20),
]
for name, start, end in ranges:
    print("=" * 60, name)
    dump = subprocess.check_output(
        [objdump, "-d", f"--start-address={start:#x}", f"--stop-address={end:#x}", so],
        text=True,
        errors="replace",
    )
    # keep only instruction lines
    for line in dump.splitlines():
        if ":" in line and "\t" in line:
            print(line)
