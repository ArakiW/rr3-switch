#!/usr/bin/env python3
"""Helper: extract 2dflat.vsh from core.romfs and print first bytes for analysis.

Run with:  python tools/_extract_vsh.py
The user can then feed the result into decode_rr3_depot.py attack.
"""

from __future__ import annotations
import sys
from pathlib import Path

# Reuse the RomFS class from decode_rr3_depot.py
sys.path.insert(0, str(Path(__file__).parent))
from decode_rr3_depot import RomFS, hexdump  # noqa: E402

ROMFS = Path(r"E:\AI\RealRacing3-switch\dist\rr3-native-rehost-complete-0.22.4\switch\rr3-native-rehost\data\core.romfs")
OUT = Path(r"E:\AI\RealRacing3-switch\.tools\_work\2dflat.vsh.encrypted")
GUI_OUT = Path(r"E:\AI\RealRacing3-switch\.tools\_work\LoadingScreen_Init.gui.encrypted")

def main() -> int:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    romfs = RomFS(ROMFS)
    try:
        vsh = romfs.read_file("files/.depot/Shaders/rr2/2dflat.vsh")
    except FileNotFoundError:
        print("2dflat.vsh not present in this RomFS")
    else:
        OUT.write_bytes(vsh)
        print(f"=== 2dflat.vsh ({len(vsh)} bytes)  md5={__import__('hashlib').md5(vsh).hexdigest()} ===")
        print(hexdump(vsh, 64))
        print(f"  bytes[2:6] = {vsh[2:6].hex()}  (shared vsh magic)")

    try:
        gui = romfs.read_file("files/.depot/gui/processed/LoadingScreen_Init.gui")
    except FileNotFoundError:
        print("LoadingScreen_Init.gui not present in this RomFS")
    else:
        GUI_OUT.write_bytes(gui)
        print(f"\n=== LoadingScreen_Init.gui ({len(gui)} bytes)  md5={__import__('hashlib').md5(gui).hexdigest()} ===")
        print(hexdump(gui, 64))
        print(f"  bytes[2:6] = {gui[2:6].hex()}")

    romfs.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())