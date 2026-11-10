"""Are the filesystem cache counters ever moved?

WHY: the census prints `fs=` (the time the filesystem service took) and
the resolution cache's hit and miss counters.  In the archived rounds the
TIME is large (99-120 seconds per 400-second round) and the COUNTERS are
always zero.  Either the cache is dead code in the hot path, or the
counters are not wired to the thing that is being timed.  Those are
different bugs with different fixes, so measure which one it is.

TWO CENSUS FORMATS ARE READ:
    old   fs= fshit= fhit= fsmiss=   (the third field was a duplicate)
    new   fs= fsr= fshit= fsmiss=    (fsr is the resolution, fs-fsr the I/O)
"""
import io
import os
import re
import sys

OLD = re.compile(
    r"\bfs=(\d+) fshit=(\d+) fhit=(\d+) fsmiss=(\d+)"
)
NEW = re.compile(
    r"\bfs=(\d+) fsr=(\d+) fshit=(\d+) fsmiss=(\d+)"
)


def main():
    print("round\tcensus\tfs>0\tfs_total_ms\tresolve_ms\thit_max\tmiss_max\tmoved")
    for archive in sys.argv[1:]:
        path = os.path.join(archive, "rr3-native-rehost.guest-view.log")
        if not os.path.exists(path):
            continue
        total = 0
        fs_positive = 0
        fs_total = 0
        resolve_total = 0
        max_hit = 0
        max_miss = 0
        moved = 0
        with io.open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if "phase=guest-frame-census" not in line:
                    continue
                new = NEW.search(line)
                old = OLD.search(line)
                if new is not None:
                    fs = int(new.group(1))
                    resolve = int(new.group(2))
                    hit = int(new.group(3))
                    miss = int(new.group(4))
                elif old is not None:
                    fs = int(old.group(1))
                    resolve = 0
                    hit = int(old.group(2))
                    miss = int(old.group(4))
                else:
                    continue
                total += 1
                if fs > 0:
                    fs_positive += 1
                    fs_total += fs
                    resolve_total += resolve
                    if hit or miss:
                        moved += 1
                    max_hit = max(max_hit, hit)
                    max_miss = max(max_miss, miss)
        print(
            "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d"
            % (
                os.path.basename(archive),
                total,
                fs_positive,
                fs_total,
                resolve_total,
                max_hit,
                max_miss,
                moved,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
