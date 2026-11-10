"""Append section 9.33 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the resolution cache BROKE THE FRONT END, and the mechanism is worth
keeping: a cache that logs every hit replaces a deduped log with a per-request
one, and the per-request SD-card writes starved the guest's own loading.  It
also corrects section 9.31's conclusion: the logging WAS a cost, just not
where the brief expected it.
"""
import io
import sys

SECTION = """
### 9.33 The cache broke the front end, and the log said so

**MEASURED 2026-11-10: rounds chunk3, fps1, fps2 and yield1, all on the same
card with the same guest state, and the front-end census says which build
comes up.**

    round   build                                    layout menu event race  frames
    chunk2  before                                    4    20   193   10   5640
    chunk3  before                                    4    20   179   10   5280
    fps1    the pacing fix alone                     13    67   324   28   1800
    fps2    + percall switch + cache + instrument     0     0   143    0   2640
    yield1  + the pursuit's yield criterion          0     0   137    0   2640

**⇒ ⇒ SO THE PACING FIX IS INNOCENT (fps1's front end was BETTER: 13/67/324
and it raced 28 times), and the break came with the cache.**  Two rounds in a
row did not even open the main menu.

**⇒ ⇒ AND THE LOG SAYS EXACTLY WHY.**  The per-call phases are gone (the
switch works), and the ASSET-SERVICE phases EXPLODED:

    phase                  chunk3    yield1    per frame (chunk3 -> yield1)
    guest-scanf-bounded      1,005    14,297     0.19  ->  5.42
    served-digest            2,578     6,956     0.48  ->  2.64
    open-ok                  2,174     6,413     0.41  ->  2.43
    guest-free-seq           1,401     3,096     0.26  ->  1.17

**⇒ ⇒ THE MECHANISM: THE CACHE LOGS EVERY HIT.**  Before the cache, the
repeated path requests were DEDUPED on the way to the log -- one line per
DISTINCT path, and the guest's set of distinct paths is small.  The cache's hit
path writes a line per REQUEST, so the log went from tens of distinct paths to
thousands of requests, and every line is one fprintf plus one fflush on the
card.  **THE PORT'S OWN LOGGING STARVED THE GUEST'S LOADING**, and the guest
never got past the event map.

**⇒ ⇒ AND THIS CORRECTS SECTION 9.31.**  There it was written that "the
per-call log was NOT the cost, and removing it buys nothing measurable".  The
truth is narrower: the PER-CALL log was not the cost, but the logging as a
whole was -- it moved from the per-call sites to the asset-service sites, and
the asset-service sites got WORSE because the cache removed their dedup.

**⇒ ⇒ SO THE FIX HAS TWO HALVES, AND BOTH ARE NEEDED:**
1. the asset-service lines go behind the same `percall` switch (they are
   per-request lines, and there are 16 of them a frame), and
2. the cache's hit path must not write a line per hit -- a cache that removes
   work must not ADD a write per request, and a line that is written on every
   hit of the same path is the same repetition the dedup existed to stop.

**⇒ ⇒ AND THE LESSON IS THE GENERAL ONE:** a cache changes WHICH work is done,
and the work it removes is not always the work that was expensive.  Here the
cache removed the path mapping and the card stat, and replaced them with a
log line per request -- and the log line was the expensive thing all along.
**⇒ ⇒ SO A CACHE HAS TO BE MEASURED ON THE WHOLE FRAME, NOT ON THE FUNCTION
IT REPLACES.**
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.33 The cache broke the front end" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
