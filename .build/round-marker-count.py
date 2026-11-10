"""Count the key guest/port markers in every archived Eden round.

WHY: the census says msStartLights fell from 27 (yield2) to 1 (slim1 and
everything after).  That is the difference between "the guest started a race"
and "the guest sat in the front end".  Before hunting a cause, measure every
marker in every round in ONE pass, so the timeline is a fact and not a story.

The markers are the same strings the census counts, plus the asset-serving
counters, so the timeline can tell a FUNCTIONAL regression (the port stopped
serving something) apart from an EMULATOR artefact (the guest is just slower).
"""
import io
import os
import re
import sys

MARKERS = [
    # the guest's own race markers (these are what the census reads)
    "EventMapScreen",
    "StartLightsScreen",
    "InGameScreen",
    "Reload HUD planes",
    # the front end
    "main_menu",
    # asset service
    "assetlist-req",
    "manifest-asset",
    "api-touch",
    "path-req",
    "served-digest",
    "open-ok",
    "guest-scanf-bounded",
    "cannot-open",
    "unknown error stat",
    "Assets are added",
    # the port's own trace
    "loader-diag",
    "phase=loadstate",
    "phase=fe-gate",
    "phase=guest-thread",
    # the census
    " tt=",
    "gui=",
    "twosq=",
]


def count_markers(path):
    counts = {m: 0 for m in MARKERS}
    size = 0
    with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            size += len(line)
            for m in MARKERS:
                if m in line:
                    counts[m] += 1
    return counts, size


def main():
    root = sys.argv[1]
    names = sorted(
        (d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d))),
        reverse=True,
    )
    logname = "rr3-native-rehost.guest-view.log"
    header = ["round"] + [m.replace(" ", "_") for m in MARKERS] + ["MB"]
    print("\t".join(header))
    for name in names:
        log = os.path.join(root, name, logname)
        if not os.path.exists(log):
            print("\t".join([name] + ["-"] * (len(MARKERS) + 1)))
            continue
        counts, size = count_markers(log)
        row = [name] + [str(counts[m]) for m in MARKERS] + ["%.1f" % (size / 1048576.0)]
        print("\t".join(row))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
