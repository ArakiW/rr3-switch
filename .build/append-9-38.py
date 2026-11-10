"""Append section 9.38 to docs/OPEN-ISSUES.md (UTF-8, no BOM).

WHY: the per-bucket instrument answers the 60 FPS question at the level that
matters -- and the answer is not the one the earlier rounds implied.
"""
import io
import sys

SECTION = """
### 9.38 Where the frame actually is: the port is small, the GL path is big

**MEASURED 2026-11-10, the per-bucket instrument agy added (the census line
carries `jni=`, `jnitex=`, `jnistr=`, `jnifile=`, `jnioth=`), frames > 600 of
two rounds of the SAME binary.**

    percall1 (percall = 1, per-line flush)      yield2 (percall = 0, buffered, yielding)
    tt    median 146.5 ms                        tt    median  68.0 ms
    jni   median   2.0 ms   mean 354.4           jni   median   8.0 ms   mean 153.8
      jnitex  median 0.0                          jnitex  median 0.0
      jnistr  median 0.0                          jnistr  median 0.0
      jnifile median 0.0                          jnifile median 0.0
      jnioth  median 2.0   mean 315.8             jnioth  median 8.0   mean 153.6
    fs    median   1.0 ms                        fs    median  0.0 ms

**⇒ ⇒ THE PORT'S OWN BOOKKEEPING IS NO LONGER THE FRAME.**  With the log
buffered and the frame loop yielding, the port's JNI dispatch is 8 ms of a
68 ms frame and the asset resolution is 0 ms (the cache serves it).  The
texture, string and file paths are all 0 ms -- **the 21 ms that section 9.31
named was never in those paths.**

**⇒ ⇒ AND THE REST IS THE GUEST'S RENDER CALL AND THE GL SUBMISSION PATH.**
The guest issues about 330 draw calls a frame (the census's `draws=`), and the
frame's remaining 60 ms over 330 calls is about 180 microseconds per call --
which is NOT the cost of the guest's own simulation, it is the cost of a GL
submission through the port's wrappers and the emulator's Mesa.

**⇒ ⇒ SO THE 60 FPS TARGET SPLITS IN TWO, AND ONLY ONE OF THEM IS OURS:**
* **ON THE CONSOLE**, the same GL path goes to NVN instead of Mesa, and the
  console's CPU is much faster.  So 60 fps is plausible there, and it is the
  CONSOLE that has to measure it.
* **IN EDEN**, the emulator's GL path is the ceiling.  The port's logic is
  already a small part of the frame, so chasing the port further in Eden buys
  little: **35-40 fps is the realistic Eden target, and the emulator cannot
  show the number the user asked for.**

**⇒ ⇒ AND THE MEASUREMENT THE CONSOLE HAS TO MAKE:** the same census line, on
the console, with the same race phase -- `tt` against the 16.67 ms budget,
and `jni` against the port's share of it.  The instrument is already on the
card; what it needs is the console's numbers.
"""


def main() -> int:
    path = sys.argv[1]
    with io.open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    if "### 9.38 Where the frame actually is" in text:
        print("already appended")
        return 0
    text = text.rstrip("\n") + "\n" + SECTION
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    print("appended %d chars" % len(SECTION))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
