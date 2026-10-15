from pathlib import Path
import subprocess
import shutil

elf = Path(r"E:\AI\RealRacing3-switch\native-switch\rr3-native-rehost.elf")
nm = r"E:\AI\RealRacing3-switch\.tools\devkitPro\msys64\opt\devkitpro\devkitA64\bin\aarch64-none-elf-nm.exe"
objdump = r"E:\AI\RealRacing3-switch\.tools\devkitPro\msys64\opt\devkitpro\devkitA64\bin\aarch64-none-elf-objdump.exe"

# nm on guest SO
so = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
out = subprocess.check_output([nm, "-D", so], text=True, errors="replace")
keys = [
    "SetButtonValueJNI",
    "SetJoystickValueJNI",
    "ControllerConnectedJNI",
    "ControllerDisconnectedJNI",
    "onTouchBeginJNI",
    "onTouchMoveJNI",
    "onTouchEndJNI",
    "onTouchCancelJNI",
    "updateAccelValues",
    "updateGyroValues",
    "onViewRenderJNI",
    "onViewChangedJNI",
]
for line in out.splitlines():
    if any(k in line for k in keys):
        print(line)

# Disassemble SetButtonValueJNI - find address
addr = None
for line in out.splitlines():
    if "SetButtonValueJNI" in line and " T " in line:
        addr = line.split()[0]
        print("ADDR", addr)
        break
if addr:
    dump = subprocess.check_output(
        [objdump, "-d", f"--start-address=0x{addr}", f"--stop-address=0x{int(addr,16)+160:x}", so],
        text=True,
        errors="replace",
    )
    print(dump[:4000])
