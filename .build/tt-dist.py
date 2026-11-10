"""The frame-budget distribution of one archived Eden round.

WHY: the 60 FPS question is a question about the DISTRIBUTION of the frame
time, not about one sample.  And the distribution has to be split by PHASE:
a frame that is waiting for an asset to be mapped from the card (fs > 0) is
a loading frame, not a rendering frame, and averaging the two produces a
number that describes neither.

The census line carries the three costs the frame can wait on:
    tt  total frame time          (swap - top)
    tr  the guest's own render    (render - top)
    ts  the swap                  (swap - render)
    tp  the port's own overhead   (tt - tr - ts)
    jni the JNI service path
    fs  the filesystem service path

=> THE 60 FPS NUMBER IS THE tt OF THE RENDERING FRAMES.
"""
import io
import os
import re
import sys

LINE = re.compile(
    r"frame=(\d+).*?draws=(\d+).*?\btt=(\d+) tr=(\d+) ts=(\d+) tp=(\d+)"
    r".*?\bjni=(\d+) jnitex=(\d+) jnistr=(\d+) jnifile=(\d+) jnioth=(\d+)"
    r".*?\bfs=(\d+) fsr=(\d+) fshit=(\d+) fsmiss=(\d+)"
)


def percentile(values, p):
    if not values:
        return 0
    ordered = sorted(values)
    index = int(round((len(ordered) - 1) * p))
    return ordered[index]


def load(path):
    frames = []
    with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            if "phase=guest-frame-census" not in raw:
                continue
            match = LINE.search(raw)
            if not match:
                continue
            g = match.groups()
            frames.append(
                {
                    "frame": int(g[0]),
                    "draws": int(g[1]),
                    "tt": int(g[2]),
                    "tr": int(g[3]),
                    "ts": int(g[4]),
                    "tp": int(g[5]),
                    "jni": int(g[6]),
                    "jnioth": int(g[10]),
                    "fs": int(g[11]),
                    "fsr": int(g[12]),
                }
            )
    return frames


def report(name, frames):
    if not frames:
        print("%s: no census lines" % name)
        return
    render = [f for f in frames if f["fs"] == 0]
    loading = [f for f in frames if f["fs"] > 0]

    def block(tag, group):
        if not group:
            print("  %-10s n=0" % tag)
            return
        tt = [f["tt"] for f in group]
        tr = [f["tr"] for f in group]
        tp = [f["tp"] for f in group]
        jni = [f["jni"] for f in group]
        under = sum(1 for v in tt if v <= 17)
        print(
            "  %-10s n=%-5d tt p25=%-5d med=%-5d p75=%-5d max=%-6d"
            " | tr med=%-5d | tp med=%-4d | jni med=%-4d | tt<=17ms: %d/%d (%.0f%%)"
            % (
                tag,
                len(group),
                percentile(tt, 0.25),
                percentile(tt, 0.50),
                percentile(tt, 0.75),
                max(tt),
                percentile(tr, 0.50),
                percentile(tp, 0.50),
                percentile(jni, 0.50),
                under,
                len(group),
                100.0 * under / len(group),
            )
        )

    print("%s  (%d census lines)" % (name, len(frames)))
    block("ALL", frames)
    block("render", render)
    block("loading", loading)


def main():
    for archive in sys.argv[1:]:
        path = os.path.join(archive, "rr3-native-rehost.guest-view.log")
        if not os.path.exists(path):
            print("%s: no guest-view.log" % archive)
            continue
        report(os.path.basename(archive), load(path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
