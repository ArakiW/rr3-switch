"""Count the menu-loading signals in a guest-view log.

    python tools/guest-analysis/menu-signals.py <log> [<log> ...]

WHY. The port's progress is judged on a handful of guest-side strings, and
the judgement has to be the SAME judgement before and after a change, or the
comparison is worthless. This is that judgement, in one place.

Each signal is counted as a LOG LINE that contains the pattern, so a
multi-line guest message counts once.

    res-order      the guest's "Res order:" banner
    res-order-entries  the node lines the ImageResManager prints UNDER it
                      ("(%f, %f) - %f - %s"), which is the list the guest
                      actually built
    atlas-fail     "Failed to load image from atlas"
    loading_icon   any mention of loading_icon
    atlas-req      a request for a sprites.android.atlas
    sprites_       any mention of sprites_
    gui-processed  any mention of gui/processed
    MainMenu       any mention of MainMenu
    twosquare      any mention of twosquare
    EventMapScreen any mention of EventMapScreen
    gfx-stats      the port's own per-frame graphics line
    frames/draws   the frame and draw counters on those lines

The res-order-entries count is the one that matters for the resolution list:
the guest prints "Res order:" and then one line per node it kept, and the
node format is the ONLY one that identifies itself as such.
"""
import re
import sys

SIGNALS = [
    ("res-order", re.compile(r"Res order:")),
    ("res-order-entries", re.compile(r"\(\d+\.\d+, \d+\.\d+\) - \d+\.\d+ - ")),
    ("atlas-fail", re.compile(r"Failed to load image from atlas")),
    ("loading_icon", re.compile(r"loading_icon")),
    ("atlas-req", re.compile(r"sprites\.android\.atlas")),
    ("sprites_", re.compile(r"sprites_")),
    ("gui-processed", re.compile(r"gui/processed")),
    ("MainMenu", re.compile(r"MainMenu")),
    ("twosquare", re.compile(r"twosquare")),
    ("EventMapScreen", re.compile(r"EventMapScreen")),
    ("gfx-stats", re.compile(r"gfx-stats")),
]

# The port's gfx-stats line carries the frame counter and the draw counter:
#   ... frames=432 calls=... draws=0/0 src=0/0 ...
FRAME_RE = re.compile(r"\bframes=(\d+)")
DRAW_RE = re.compile(r"\bdraws=(\d+)/(\d+)")


def count(path):
    counts = {name: 0 for name, _ in SIGNALS}
    frames = []
    draws = []
    total = 0
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            total += 1
            for name, pattern in SIGNALS:
                if pattern.search(line):
                    counts[name] += 1
            if counts["gfx-stats"]:
                m = FRAME_RE.search(line)
                if m:
                    frames.append(int(m.group(1)))
                m = DRAW_RE.search(line)
                if m:
                    draws.append(int(m.group(1)))
    counts["lines"] = total
    counts["max-frame"] = max(frames) if frames else -1
    counts["max-draws"] = max(draws) if draws else -1
    counts["gfx-stats-lines"] = len(frames)
    return counts


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    results = [(path, count(path)) for path in argv[1:]]
    names = [name for name, _ in SIGNALS] + [
        "lines", "gfx-stats-lines", "max-frame", "max-draws"]
    width = max(len(path) for path, _ in results) + 2
    print(" " * width + "".join("%-16s" % n for n in names))
    for path, counts in results:
        print("%-*s" % (width, path) +
              "".join("%-16s" % counts.get(n, 0) for n in names))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
