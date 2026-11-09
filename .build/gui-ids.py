#!/usr/bin/env python3
"""Which screen owns the control, and what is next to it?

The pursuit's front-end driver needs a TABLE: this screen, that control.  The
table has to be grounded in the guest's own .gui files, not guessed from a
screenshot, so this asks the index that tools/gui_index.py builds.

Usage:
    python .build\gui-ids.py <id> [<id> ...]        one line per id
    python .build\gui-ids.py --screens EVENTMAP     the ids of a screen
    python .build\gui-ids.py --around <id>          the ids next to one
"""
import json
import sys
from pathlib import Path

INDEX = Path(r"E:\AI\RealRacing3-switch\.build\gui_index.json")


def load():
    with INDEX.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def main() -> int:
    if not INDEX.is_file():
        print("no index; run: python tools\\gui_index.py --out .build\\gui_index.json")
        return 1
    data = load()
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2

    # The index's shape: find the screen map and the reverse index.
    screens = data.get("by_screen_name") or {}
    reverse = data.get("ids") or {}

    if args[0] == "--screens":
        needle = args[1].lower()
        for screen, ids in sorted(screens.items()):
            if needle in screen.lower():
                print("== {0}  ({1} ids)".format(screen, len(ids)))
                for i in sorted(ids):
                    print("   {0}".format(i))
        return 0

    if args[0] == "--around":
        want = args[1]
        owners = reverse.get(want) or reverse.get(want.upper()) or []
        if not owners:
            print("no screen owns {0}".format(want))
            return 1
        for screen in owners:
            ids = sorted(screens.get(screen, []))
            if want not in ids:
                continue
            i = ids.index(want)
            print("== {0}  ({1} ids)".format(screen, len(ids)))
            for j in range(max(0, i - 8), min(len(ids), i + 9)):
                mark = ">>" if j == i else "  "
                print("   {0} {1}".format(mark, ids[j]))
        return 0

    for want in args:
        owners = reverse.get(want) or reverse.get(want.upper()) or []
        if not owners:
            print("{0:34} -- NOT FOUND".format(want))
            continue
        for screen in owners[:6]:
            ids = sorted(screens.get(screen, []))
            print("{0:34} {1}   (on {2})".format(want, ids.index(want) if want in ids else "?", screen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
