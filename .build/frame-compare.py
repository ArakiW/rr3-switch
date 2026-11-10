"""Compare two rounds' frame costs, same phase, same script.

Usage: python frame-compare.py <tag1> <archive1> <tag2> <archive2> ...
"""
import re
import statistics
import sys
from pathlib import Path

BASE = Path(r"E:\AI\RealRacing3-switch\.build\eden-runs")
NUM = re.compile(r"\b(tt|tr|ts|tp|jni|jnitex|jnistr|jnifile|jnioth|fs|gui|twosq)=(\d+)")


def load(archive: str):
    path = BASE / archive / "rr3-native-rehost.guest-view.log"
    rows = []
    lines = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            lines += 1
            if "phase=guest-frame-census" not in line:
                continue
            row = {k: int(v) for k, v in NUM.findall(line)}
            if "tt" in row:
                rows.append(row)
    return rows, lines


def main() -> int:
    args = sys.argv[1:]
    for i in range(0, len(args), 2):
        tag, archive = args[i], args[i + 1]
        rows, lines = load(archive)
        if not rows:
            print("%-8s no census data" % tag)
            continue
        frames = rows[-1].get("frame", 0)
        print("=== %s (%s) ===" % (tag, archive))
        print("   frames=%-6d log lines=%-8d => %.1f lines/frame" %
              (frames, lines, lines / max(1, frames)))
        for label, lo in (("loading (f<=600)", 0), ("rest (f>600)", 600)):
            sel = [r for r in rows if (r.get("frame", 0) > lo if lo else True)]
            if not sel:
                continue
            print("   -- %s  n=%d" % (label, len(sel)))
            for k in ("tt", "tr", "ts", "tp", "jni", "jnioth", "fs"):
                v = [r[k] for r in sel if k in r]
                if v:
                    print("      %-7s median %8.1f ms   p25 %8.1f   mean %8.1f" %
                          (k, statistics.median(v), sorted(v)[len(v) // 4], statistics.fmean(v)))
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
