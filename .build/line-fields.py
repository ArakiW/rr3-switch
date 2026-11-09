"""The HEAD and MIDDLE fields of the race-state line, which were always aligned.

WHY: the tail of the line was printed with four fields shifted (dir, mot, cchunk)
in the chunk1 round.  The head and middle were never affected, so they are
trustworthy for that round.  These fields say how far the car was from the
line, which look-ahead the port used, and how far it was from the racing line.

Usage: python line-fields.py <guest-view.log> [first] [last]
"""
import re
import sys

FIELD_RE = re.compile(
    r"\b(f|total|prog|rate|lres|rres|la|lam|line|lachunk|ctdist|ctok|tsok|"
    r"perr|pur|steer|mag|sign|flips|stalls|thr|brk|cpos|snode|sdist)="
    r"(-?[\w.]+)")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    first = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    last = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9

    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "ctdist=" not in line:
                continue
            fields = dict(FIELD_RE.findall(line))
            if "f" not in fields:
                continue
            rows.append(fields)

    rows = rows[first:last]
    print("samples: %d" % len(rows))
    header = ["f", "total", "prog", "rate", "lres", "rres", "la", "lam",
              "line", "ctdist", "ctok", "perr", "pur", "steer", "mag", "sign",
              "flips", "stalls", "thr", "brk"]
    print(" | ".join("%9s" % h for h in header))
    for row in rows:
        print(" | ".join("%9s" % row.get(h, "-") for h in header))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
