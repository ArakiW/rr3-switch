"""Did the path-resolution trace lose the asset paths?

WHY: docs/OPEN-ISSUES.md 9.33 says the port's own front-end navigation reads
the path-resolution trace, and that with the trace silenced the port stops
driving the guest and the guest sits in the event map.  Round yield2 (27
start-lights, 36 race screens) logged EVERY resolved path; the rounds after
slim1 log only `gui/processed/` ones.  If the navigation needs the asset
paths, the thinning took the navigation's eyes away -- and that is a
PORT-SIDE regression, not a slow guest.

So: count, per round, the map-app lines with an asset path and the ones
without.
"""
import io
import os
import re
import sys

MAP = re.compile(r"phase=map-app .*? (rr3\w*:[^\s]+)")
GUI = "/gui/processed/"


def main():
    print("round\tmapapp_total\tasset_path\tgui_path\tother")
    for archive in sys.argv[1:]:
        path = os.path.join(archive, "rr3-native-rehost.guest-view.log")
        if not os.path.exists(path):
            continue
        total = 0
        asset = 0
        gui = 0
        other = 0
        with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if "phase=map-app" not in line:
                    continue
                total += 1
                match = MAP.search(line)
                target = match.group(1) if match else ""
                if GUI in target:
                    gui += 1
                elif target:
                    asset += 1
                else:
                    other += 1
        print(
            "%s\t%d\t%d\t%d\t%d"
            % (os.path.basename(archive), total, asset, gui, other)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
