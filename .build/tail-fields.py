"""Read the race-state lines of a guest-view log and tabulate the TAIL fields.

WHY: a commit added the `cchunk` field to the format string AFTER `mot` but
put its argument BEFORE `dir`.  The count of specifiers and of arguments is
equal, so the mistake is a POSITION error, not a count error: only the four
fields BETWEEN the two insertion points (`dir`, `mot.x`, `mot.y`, `cchunk`)
print the wrong value.  Everything from `perr` onwards is aligned.

This script prints the fields that are affected and the ones that are not, so
the claim can be checked against a log instead of believed.

Usage: python tail-fields.py <guest-view.log> [max-lines]
"""
import re
import sys

FIELD_RE = re.compile(r"\b(f|dir|mot|cchunk|perr|pur|total|ctdist|flips|stalls|steer|mag|sign|thr|brk)=([^ ]+)")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 10**9

    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if "ctdist=" not in line:
                continue
            fields = dict(FIELD_RE.findall(line))
            if "ctdist" not in fields:
                continue
            rows.append(fields)
            if len(rows) >= limit:
                break

    if not rows:
        print("no race-state lines with ctdist in", path)
        return 1

    print("samples: %d" % len(rows))
    header = ["f", "dir", "mot", "cchunk", "perr", "pur", "total", "ctdist",
              "flips", "stalls", "steer", "mag", "sign", "thr", "brk"]
    print(" | ".join("%10s" % h for h in header))
    for row in rows:
        print(" | ".join("%10s" % row.get(h, "-") for h in header))

    # The affected fields and the unaffected ones, separately.
    print()
    print("AFFECTED (printed the wrong argument at commit d99ce0ca): dir, mot, cchunk")
    for key in ("dir", "mot", "cchunk"):
        values = [row.get(key) for row in rows if key in row]
        uniq = sorted(set(values))
        print("  %-7s distinct=%d  min=%s max=%s" %
              (key, len(uniq), uniq[0] if uniq else "-", uniq[-1] if uniq else "-"))
    print()
    print("UNAFFECTED (aligned): perr, pur, total, ctdist, flips, stalls, steer, mag, sign")
    for key in ("perr", "pur", "total", "ctdist", "flips", "stalls"):
        values = [row.get(key) for row in rows if key in row]
        uniq = sorted(set(values))
        print("  %-7s distinct=%d  first=%s last=%s" %
              (key, len(uniq), values[0] if values else "-", values[-1] if values else "-"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
