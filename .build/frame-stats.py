"""The frame statistics of a round, from the guest-frame-census lines.

WHY: the port's frame cost is already on the census line (tt/tr/ts/tp), every
60 frames, so the measurement needs no new instrument -- it needs a script that
computes the same numbers the same way every time, or two rounds cannot be
compared.

WHAT THE FOUR NUMBERS MEAN (the port's own comment in guest_view_probe.cpp):

    tt  the whole iteration
    tr  the guest's render call (the port calls the guest's render entry)
    ts  eglSwapBuffers -- the present
    tp  the port's own per-frame work outside those two (tt - tr - ts)

and, on the build with the 60 FPS instrument:

    jni  the time spent inside the port's JNI dispatch
    fs   the time spent inside the port's filesystem resolution
    fshit / fsmiss   the resolution cache's hit and miss counts

THE RACE PHASE IS frames > 2500.  The boot and the loading phase are much
slower than the race and they are not what "the game's frame rate" means, so
they are reported separately rather than averaged in.

Usage: python frame-stats.py <archive-dir-or-guest-log> [more...]
"""
import re
import statistics
import sys
from pathlib import Path

FIELDS = ("tt", "tr", "ts", "tp", "jni", "fs")
COUNT_RE = re.compile(r"\b(fhit|fsmiss|frame|frames|draws|arr)=(\d+)")
NUM_RE = re.compile(r"\b(tt|tr|ts|tp|jni|fs)=(\d+)")


def find_log(arg: Path) -> Path:
    if arg.is_file():
        return arg
    candidate = arg / "rr3-native-rehost.guest-view.log"
    if candidate.is_file():
        return candidate
    raise SystemExit("no guest log under %s" % arg)


def stats(values):
    if not values:
        return None
    ordered = sorted(values)
    return {
        "n": len(ordered),
        "avg": statistics.fmean(ordered),
        "p90": ordered[min(len(ordered) - 1, int(len(ordered) * 0.9))],
        "max": ordered[-1],
    }


def report(path: Path) -> None:
    rows = []
    log_line_re = re.compile(r"phase=guest-frame-census\b")
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not log_line_re.search(line):
                continue
            row = {k: int(v) for k, v in NUM_RE.findall(line)}
            for key, value in COUNT_RE.findall(line):
                row[key] = int(value)
            if "tt" in row:
                rows.append(row)
    if not rows:
        print("%s: no census lines" % path)
        return

    print("=== %s ===" % path)
    print("  census samples: %d" % len(rows))
    groups = [("WHOLE RUN", rows), ("RACE (frames>2500)", [r for r in rows if r.get("frame", 0) > 2500])]
    for name, group in groups:
        if not group:
            continue
        print("  -- %s --" % name)
        for field in FIELDS:
            s = stats([r[field] for r in group if field in r])
            if s is None:
                continue
            fps = 1000.0 / s["avg"] if s["avg"] > 0 else 0.0
            print("     %-4s n=%-5d avg=%7.2f ms  p90=%7.2f ms  max=%8.2f ms   (%5.1f fps if this were the whole frame)"
                  % (field, s["n"], s["avg"], s["p90"], s["max"], fps))
        hits = sum(r.get("fhit", 0) for r in group)
        misses = sum(r.get("fsmiss", 0) for r in group)
        if hits or misses:
            total = hits + misses
            print("     resolution cache: %d hits / %d misses = %.1f%% hit rate"
                  % (hits, misses, (100.0 * hits / total) if total else 0.0))
        draws = group[-1].get("draws", 0)
        if draws:
            frames = max(1, group[-1].get("frame", 0))
            print("     the guest's own traffic: %d draw calls over %d frames = %.1f draws/frame"
                  % (draws, frames, draws / frames))
    print()


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for arg in sys.argv[1:]:
        report(find_log(Path(arg)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
