#!/usr/bin/env python3
"""The FMOD shim's pools must have a free slot, and its self-test must be
READ.

THE DEFECT THIS EXISTS FOR.  The Sound and DSP pools searched for a slot
with `released == true`, and every slot starts `released == false`, so in
a fresh session `createSound()` and `createDSPByType()` could not
allocate anything: the guest could never make a sound and never make a DSP.
The shim's own self-test knew -- it logged "bits=0xf1f expected=0xfff", the
three missing bits being exactly createSound (5), the callback that needs a
channel (6) and playSound (7) -- and nobody read it, because it was a log
line and not a number anything consumed.

So these checks cover two things: the pool fix itself, and the fact that
the self-test's result is now in the CENSUS, where the round's summary
prints it and a human (or an agent) reads it.

Every assertion here is aimed at one way of breaking the property, and the
mutation harness (.build/mutation-test-fmod-pool.py) requires each of them
to fail when the property is broken.
"""

from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    shim = (root / "source" / "fmod_shim.cpp").read_text(encoding="utf-8")
    view = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    census = (root / ".." / "tools" / "Test-OnEden.ps1").resolve().read_text(encoding="utf-8")

    # 1. THE POOL MARKER EXISTS, AND IT MARKS BOTH POOLS.
    assert "void markPoolsFree()" in shim, "there is no markPoolsFree()"
    marker_at = shim.index("void markPoolsFree()")
    body = shim[marker_at:marker_at + 2000]
    assert "gSounds[index].released = true;" in body, \
        "the never-used Sound slots are not marked free"
    assert "gDsps[index].released = true;" in body, \
        "the never-used DSP slots are not marked free"

    # 2. IT IS CALLED BEFORE EVERY ALLOCATION.  The three functions that
    #    hand out a Sound or a DSP are the three that can find no slot.
    for function in ("fmodSystemCreateSound", "fmodSystemCreateDSPByType",
                     "fmodSystemCreateDSP"):
        at = shim.index("FMOD_RESULT " + function + "(")
        window = shim[at:at + 900]
        assert "markPoolsFree();" in window, \
            function + " does not mark the pools free before allocating"

    # 3. THE SELF-TEST STILL EXPECTS ALL TWELVE CHECKS, and the port says so.
    #    The ASSERTION IS ON THE FORMAT STRING, not on a loose substring: a
    #    mutant that changed the expectation to 0xf1f once passed this test
    #    because the same text also appeared in a comment.
    assert '"bits=0x%llx expected=0xfff"' in shim, \
        "the self-test no longer says what it expects"
    assert "expected=0xf1f" not in shim, \
        "the self-test expects fewer than all twelve checks"

    # 4. THE CENSUS CARRIES THE FMOD STATE, at the HEAD of the line.
    assert '"frame=%llu lights=%llu race=%llu fmst=0x%llx snd=%llu str=%llu ch=%llu' in view, \
        "the census line does not carry the FMOD state at its head"
    assert "fmodStats.selfTestBits" in view
    assert "fmodStats.soundCreates" in view
    assert "fmodStats.channelsStarted" in view
    assert "rr3::fmodShimStats()" in view

    # 5. THE ROUND CENSUS READS IT AND PRINTS IT.
    assert "' fmst=0x([0-9a-fA-F]+)'" in census, \
        "the round census does not read the FMOD self-test bits"
    assert "' snd=(\\d+)'" in census
    assert "' str=(\\d+)'" in census
    assert "' ch=(\\d+)'" in census
    #    AND THE PARSED VALUE MUST BE USED.  A mutant that read the bits
    #    and threw them away passed the first version of this test.
    assert "$summary.msFmodSelfTest = $fmodBits[$fmodBits.Count - 1].Groups[1].Value" in census, \
        "the round census reads the FMOD bits and does not use them"
    assert "$summary.msFmodSelfTest" in census
    assert "want 0xfff" in census, \
        "the summary does not say what the self-test bits should be"

    print("PASS: the FMOD pools have free slots, and the shim's self-test is "
          "in the census where the round's summary prints it")


if __name__ == "__main__":
    main()
