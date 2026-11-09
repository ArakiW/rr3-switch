# One line of verdict per round: did the race-metrics object ever come alive?
#
# WHY: the Nov-4 rounds found it at f=540 and the Nov-9 rounds never did.
# The question this answers is "was the object ever live", which is a
# different question from "how far did the car get", and it is the one
# that decides whether a round measured the pursuit at all.
#
# Usage: python .build\metrics-summary.py <run-tag-or-archive-dir>
import os
import re
import sys
import collections

ROOT = r"E:\AI\RealRacing3-switch\.build\eden-runs"


def find_archive(name):
    p = os.path.join(ROOT, name)
    if os.path.isdir(p):
        return p
    if os.path.isdir(ROOT):
        for d in sorted(os.listdir(ROOT)):
            if name in d:
                return os.path.join(ROOT, d)
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: metrics-summary.py <tag-or-archive-dir>")
        return 2
    target = sys.argv[1]
    if os.path.isdir(target):
        archive = target
    else:
        archive = find_archive(target)
    if archive is None:
        print("no archive for {0}".format(target))
        return 1
    log = os.path.join(archive, "rr3-native-rehost.guest-view.log")
    if not os.path.isfile(log):
        print("no guest-view.log in {0}".format(archive))
        return 1

    first_live = None
    first_gm = None
    last_f = 0
    lines = 0
    rej = collections.Counter()
    with open(log, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "phase=race-state" not in line:
                continue
            lines += 1
            m = re.search(r"\bf=(\d+)", line)
            if m:
                last_f = max(last_f, int(m.group(1)))
            if first_gm is None and "gm_ok=1" in line:
                first_gm = m.group(1) if m else "?"
            if first_live is None and re.search(r"\bmetrics=1\b", line):
                first_live = m.group(1) if m else "?"
            r = re.search(r"\brej=([a-z0-9-]+)", line)
            if r:
                rej[r.group(1)] += 1

    print("archive        : {0}".format(os.path.basename(archive)))
    print("race-state rows: {0}".format(lines))
    print("first gm_ok=1  : {0}".format(first_gm if first_gm else "NEVER"))
    print("first metrics=1: {0}".format(first_live if first_live else "NEVER"))
    print("last frame     : {0}".format(last_f))
    print("rejections     : {0}".format(dict(rej)))
    summary = os.path.join(archive, "run-summary.txt")
    if os.path.isfile(summary):
        with open(summary, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if re.match(r"^(frames|wall clock|nro )", line):
                    print(line.rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
