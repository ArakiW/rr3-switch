# What did the pursuit actually DO in a round?
#
# WHY: the pursuit is 3 % of a lap where the baseline was 10.35 %, and the
# two known suspects are (a) the controller not being in pursuit at all and
# (b) the controller steering on the wrong thing.  Both are visible in the
# race-state line, and both are invisible in a summary.
#
# Usage: python pursuit-trace.py <archive-dir-or-guest-log> [...]
import re
import sys
from collections import Counter
from pathlib import Path

KEYS = ("f", "total", "perr", "pur", "steer", "mag", "dir", "flips", "stalls",
        "thr", "brk", "la", "lam", "rres", "ctdist", "speed", "mot", "sign")


def find_log(arg: Path) -> Path:
    if arg.is_file():
        return arg
    candidate = arg / "rr3-native-rehost.guest-view.log"
    if candidate.is_file():
        return candidate
    raise SystemExit(f"no guest log under {arg}")


def main() -> None:
    for arg in sys.argv[1:]:
        log = find_log(Path(arg))
        rows = []
        pattern = re.compile(r"phase=race-state\b.*")
        with log.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if "phase=race-state" not in line:
                    continue
                row = {}
                for key in KEYS:
                    match = re.search(r"(?:^|\s)" + key + r"=(-?[\w.]+)", line)
                    if match:
                        try:
                            row[key] = float(match.group(1))
                        except ValueError:
                            row[key] = match.group(1)
                if "f" in row:
                    rows.append(row)
        if not rows:
            print(f"{log}: no race-state lines")
            continue
        print(f"=== {log} ({len(rows)} race-state samples) ===")
        print(f"    frame   total    perr  pur  steer   mag  dir flips stalls thr brk   la  ctdist")
        step = max(1, len(rows) // 22)
        for row in rows[::step]:
            print("  {f:>7.0f} {total:>7.0f} {perr:>7.3f} {pur:>3.0f} {steer:>6.3f} "
                  "{mag:>5.2f} {dir:>3.0f} {flips:>5.0f} {stalls:>6.0f} {thr:>3.0f} {brk:>3.0f} "
                  "{la:>5.1f} {ctdist:>7.2f}".format(**{k: row.get(k, 0) for k in
                      ("f", "total", "perr", "pur", "steer", "mag", "dir", "flips",
                       "stalls", "thr", "brk", "la", "ctdist")}))
        pursuit_on = sum(1 for r in rows if r.get("pur", 0) > 0.5)
        driving = sum(1 for r in rows if r.get("drive", 0) > 0.5)
        flips = rows[-1].get("flips", 0)
        stalls = rows[-1].get("stalls", 0)
        last = rows[-1]
        print(f"    pursuit on {pursuit_on}/{len(rows)} samples; "
              f"drive {driving}/{len(rows)}; flips={flips:.0f} stalls={stalls:.0f}; "
              f"final total={last.get('total', 0):.0f} "
              f"(speed={last.get('speed', 0):.0f})")
        magnitudes = Counter(round(r.get("mag", 0), 1) for r in rows)
        print(f"    steering magnitude: {magnitudes.most_common(6)}")
        print()


if __name__ == "__main__":
    main()
