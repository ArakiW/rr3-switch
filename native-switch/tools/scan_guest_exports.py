from pathlib import Path

candidates = list(Path(r"E:\AI\RealRacing3-switch").rglob("libRealRacing3.so"))
print("candidates", [str(c) for c in candidates[:8]])
so = candidates[0]
data = so.read_bytes()
print("size", len(data), "path", so)

needles = [
    b"Java_com_firemint",
    b"SetButton",
    b"SetJoystick",
    b"Controller",
    b"onTouch",
    b"TouchEvent",
    b"InputDevice",
    b"GameController",
    b"KeyEvent",
    b"MotionEvent",
    b"onView",
]
seen = set()
for n in needles:
    idx = 0
    printed = 0
    while printed < 20:
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
        if 10 <= len(s) <= 100 and s not in seen:
            seen.add(s)
            print(hex(i), s)
            printed += 1
        idx = i + len(n)
print("done unique", len(seen))
