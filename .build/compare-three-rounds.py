"""Compare the race markers of a few archived rounds, side by side.

WHY: the start-light count collapsed from 27 to 1 across the recent rounds,
and the obvious reading is "the port regressed".  The controlled round
(ctl1) put the SAME binary back on the SAME guest state that produced 27,
so these three rows answer the question:

    yield2      the reference: race 36, lights 27
    loaderdiag1 the same binary as ctl1, but on an INHERITED state
    ctl1        the same binary as loaderdiag1, on the RESTORED yield2 state

If ctl1 comes back at 27 and loaderdiag1 at 1, then the code did not
regress -- the rounds were measuring the guest's save.
"""
import io
import os
import sys

MARKERS = [
    "EventMapScreen",
    "StartLightsScreen",
    "InGameScreen",
    "Reload HUD planes",
    "assetlist-req",
    "manifest-asset",
    "api-touch",
    "path-req",
    "served-digest",
    "guest-scanf-bounded",
]


def count(archive):
    path = os.path.join(archive, "rr3-native-rehost.guest-view.log")
    if not os.path.exists(path):
        return None
    counts = {m: 0 for m in MARKERS}
    census = 0
    with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "phase=guest-frame-census" in line:
                census += 1
            for marker in MARKERS:
                if marker in line:
                    counts[marker] += 1
    return counts, census


def main():
    print("%-22s %6s %6s %6s %6s %9s %9s" % (
        "round", "event", "lights", "race", "hud", "scanf", "census"))
    for archive in sys.argv[1:]:
        result = count(archive)
        if result is None:
            print("%-22s  (no log)" % os.path.basename(archive))
            continue
        counts, census = result
        print("%-22s %6d %6d %6d %6d %9d %9d" % (
            os.path.basename(archive),
            counts["EventMapScreen"],
            counts["StartLightsScreen"],
            counts["InGameScreen"],
            counts["Reload HUD planes"],
            counts["guest-scanf-bounded"],
            census,
        ))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
