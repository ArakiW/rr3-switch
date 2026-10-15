from pathlib import Path

so = Path(r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so")
data = so.read_bytes()

# Search controller button mapping strings
needles = [
    b"BUTTON_A",
    b"BUTTON_B",
    b"KEYCODE_BUTTON",
    b"Gamepad",
    b"pad_a",
    b"btn_a",
    b"ControllerManager",
    b"SetButtonValue",
    b"SetJoystickValue",
    b"leftTrigger",
    b"rightTrigger",
    b"axis_x",
    b"AXIS_X",
    b"AXIS_Y",
    b"AXIS_Z",
    b"AXIS_LTRIGGER",
]
for n in needles:
    idx = 0
    count = 0
    while count < 6:
        i = data.find(n, idx)
        if i < 0:
            break
        a = i
        while a > 0 and 32 <= data[a - 1] < 127:
            a -= 1
        b = i
        while b < len(data) and 32 <= data[b] < 127:
            b += 1
        s = data[a:b].decode("ascii", "replace")
        if 6 <= len(s) <= 90:
            print(hex(i), s)
            count += 1
        idx = i + len(n)
    print(f"--- {n.decode(errors='replace')}")
