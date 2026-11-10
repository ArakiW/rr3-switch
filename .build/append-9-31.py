"""Append section 9.31 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: round fps2 measured the port's per-call overhead with the new instrument,
and the answer is NOT the one the brief expected: the logging was not the
dominant cost.  The port's JNI SERVICE path is.  That has to be on the record,
because it changes what the next iteration does.
"""
import io
import sys

SECTION = """
### 9.31 The per-call log was not the cost: the JNI SERVICE path is

**MEASURED 2026-11-10, round `fps2`** (the build with the `percall` switch at
its DEFAULT, which is OFF, the bounded asset resolution cache, and the new
instrument on the census line), against round `chunk3` (the same card, the same
guest state, the per-call logging ON and no cache).

    chunk3 (percall ON,  no cache): 41.9 log lines per frame
    fps2   (percall OFF, cache ON) : 30.0 log lines per frame

and the race phase (frames > 2500):

    chunk3:  tt median 35.0 ms   p25 32.0 ms
    fps2:    tt median 37.0 ms   p25 32.0 ms   jni median 21.0 ms
                                        fs median 0.0 ms

**⇒ ⇒ THE SWITCH WORKS: THE PER-CALL LINES ARE GONE.**  The phases that used
to dominate the log are not there any more -- chunk3's top six were scissor
(37,583), map-app (25,403), cc-dispatch (20,978), jni-void (20,976),
jni-utf163 (20,685) and cc-dispatch-bool (11,267); fps2's top six are
guest-scanf-bounded (17,553), served-digest (7,847), open-ok (7,399),
fs-candidate (4,002), guest-log-any (3,999) and guest-free-seq (3,586).

**⇒ ⇒ AND THE FRAME TIME DID NOT MOVE: 35.0 ms -> 37.0 ms median, which is
INSIDE the round-to-round variance of the loading phase.**  So the 42 log
lines a frame were NOT the dominant cost, and the honest conclusion is that
removing them buys nothing measurable on its own.

**⇒ ⇒ WHAT THE INSTRUMENT SAYS IS THE COST: THE PORT'S JNI SERVICE PATH.**
`jni` -- the time inside the port's JNI dispatch entry points -- is 21 ms in
the race phase, against a 37 ms frame.  `fs` -- the time inside the asset
resolution itself -- is 0 ms, because the cache is serving the repeated
requests.  So the port spends 21 ms a frame SERVING THE GUEST's calls, and
almost none of it is the resolution: it is what the guest asks for through
those calls (and the remaining per-frame lines -- served-digest, open-ok,
fs-candidate, guest-scanf-bounded -- are the asset-service logs, still 16
lines a frame, still one fprintf plus one fflush each).

**⇒ ⇒ SO 60 FPS NEEDS THE NEXT TWO THINGS, IN THIS ORDER:**
1. the asset-SERVICE logs go behind the same switch (they are not per-call
   lines, they are per-request lines, and there are 16 of them a frame);
2. the cache must stop touching the card on every hit -- it revalidates with
   `::stat` per hit, which is a card touch per request -- and the JNI
   service path's 21 ms has to be accounted for request by request.

**⇒ ⇒ AND THE CEILING IS NOW HONEST:** with the port's own service work at
21 ms of a 37 ms frame, 16.67 ms is unreachable until that 21 ms comes down.
The guest's own drawing is what is left after it, and that part is not the
port's to optimise.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.31 The per-call log was not the cost" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
