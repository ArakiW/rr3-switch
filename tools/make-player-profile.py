import io
import os
import struct

SIZE = 128
MAGIC = 0xABCF_FCBA        # cmp w21, w8 at guest 0x15ac508
CTRL_OFF = 20              # ControlMethods_e
FLIP_OFF = 24
METHOD = 7                 # Wheel_Manual ("Wheel B")


def build():
    buf = bytearray(SIZE)
    struct.pack_into("<I", buf, 0, MAGIC)
    struct.pack_into("<I", buf, CTRL_OFF, METHOD)
    buf[FLIP_OFF] = 0
    return bytes(buf)


root = r"E:\Eden\user\sdmc\switch\rr3-native-rehost"
# Exactly the candidates the guest's own stat-path trace shows it probing.
targets = [
    os.path.join(root, "doc", "player_profile.2.dat.0"),
    os.path.join(root, "doc", "player_profile.2.dat"),
    os.path.join(root, "doc", "player_profile.dat"),
    os.path.join(root, ".depot", "profile.dat"),
    os.path.join(root, "apk", "res", "profile.dat"),
    os.path.join(root, "doc", "profile.dat"),
]
for path in targets:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, "wb") as handle:
        handle.write(build())
    print("  %-58s %d B  magic 0x%08X" % (path, SIZE, MAGIC))
