#!/usr/bin/env python3
"""The P1 instrument and the FMOD lever.

P1 IS "DOES THE GUEST PRODUCE PCM FOR THE DEVICE".  The instrument has to
count the guest's bytes and not the device's own self-test bytes, and the
number has to reach the round's summary.

THE LEVER IS THE R1 EXPERIMENT.  It must be:
  * OFF by default -- every released build answers the guest's FMOD
    symbols with the port's shim;
  * applied at the ONE place the interception happens, so there is no
    second path that still answers;
  * ALL-OR-NOTHING for the guest's FMOD symbols, because a partial
    withholding recreates the mixed-layout failure the shim exists to end;
  * reachable from a round, so the A/B needs no rebuild.
"""
from pathlib import Path


def body_of(text, marker):
    """The text of the function whose DEFINITION starts at marker.

    A forward declaration has the same text as the definition, so the
    declaration is skipped: the definition is the occurrence whose next
    "{" comes before the next ";".
    """
    cursor = 0
    while True:
        start = text.find(marker, cursor)
        assert start >= 0, "no definition of " + marker
        open_brace = text.find("{", start)
        semicolon = text.find(";", start)
        if open_brace >= 0 and (semicolon < 0 or open_brace < semicolon):
            depth = 0
            for index in range(open_brace, len(text)):
                if text[index] == "{":
                    depth += 1
                elif text[index] == "}":
                    depth -= 1
                    if depth == 0:
                        return text[start:index + 1]
            raise AssertionError("unbalanced braces after " + marker)
        cursor = start + len(marker)


def main():
    root = Path(__file__).resolve().parents[1]
    stub = (root / "source" / "fmod_audio_stub.cpp").read_text(encoding="utf-8")
    header = (root / "include" / "rr3" / "fmod_audio_stub.hpp").read_text(encoding="utf-8")
    resolver = (root / "source" / "symbol_resolver.cpp").read_text(encoding="utf-8")
    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")
    view = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    census = (root / ".." / "tools" / "Test-OnEden.ps1").resolve().read_text(encoding="utf-8")
    deployer = (root / ".." / ".build" / "deploy-census-conf.ps1").resolve().read_text(encoding="utf-8")
    round = (root / ".." / ".build" / "run-pinned-round.ps1").resolve().read_text(encoding="utf-8")

    # 1. THE DEVICE COUNTS THE GUEST'S PCM.
    assert "gStats.pcmEnqueues" in stub and "gStats.pcmBytes" in stub
    assert "uint32_t pcmEnqueues{};" in header
    assert "uint32_t pcmBytes{};" in header
    # The format the guest asked for is read out of its SLDataFormat.
    assert "SlDataFormatPcm" in stub
    assert "gStats.formatRate = format->sampleRate;" in stub
    assert "gStats.formatChannels = format->numChannels;" in stub
    assert "gStats.formatBits = format->bitsPerSample;" in stub

    # 2. THE DEVICE'S OWN SELF-TEST BYTES ARE NOT THE GUEST'S.
    assert "if (!gInSelfTest) {" in stub, \
        "the PCM counters do not exclude the self-test"
    assert "gInSelfTest = true;" in stub and "gInSelfTest = false;" in stub

    # 3. THE NUMBERS REACH THE SUMMARY.
    assert "pcm_enq=%u" in view and "pcm_bytes=%u" in view
    assert "fmt=%u/%u/%u" in view
    assert "audioStats.pcmEnqueues" in view
    assert "' pcm_enq=(\\d+)'" in census, \
        "the round census does not read the PCM counters"
    assert "$summary.msPcmEnqueues = [int]$pcmEnq[$pcmEnq.Count - 1].Groups[1].Value" in census
    assert "PCM enqueued" in census

    # 4. THE LEVER IS OFF BY DEFAULT AND IS ONE KEY.
    assert '"fmod=real"' in resolver
    assert 'sdmc:/switch/rr3-census.conf' in resolver
    # The lever reads the file and looks for the key: there is NO other way
    # for it to say "the guest's own library".  A mutant that made it
    # unconditional passed the first version of this test, because nothing
    # here said what the DEFAULT is.
    #
    # THE PROPERTY, NOT THE ADDRESS.  The first version of this test pinned
    # the file reading to ONE function body, so moving the reading into a
    # helper broke the test without changing any behaviour.  That is the
    # same mistake this project keeps making with layouts: naming a place
    # instead of naming a fact.
    lever_state = body_of(resolver, "int guestFmodLeverState(")
    lever = body_of(resolver, "bool guestFmodUsesRealLibrary(")
    assert "std::fopen" in lever_state and "std::fgets" in lever_state, \
        "the lever does not read the config file"
    assert 'strstr(line, "fmod=real")' in lever_state
    assert "return true" not in lever_state, \
        "the lever can say 'the guest's own FMOD' without reading the key"
    assert "cached = real ? 2 : 1;" in lever_state
    assert "guestFmodLeverState() == 2" in lever, \
        "the predicate does not ask the state that was just read"
    assert "std::fopen" not in lever, "the config file is read twice"
    # It is consulted ONLY when the key is there: the default path returns
    # the shim's address.
    assert "if (isGuestFmodSymbol(symbol.name) && guestFmodUsesRealLibrary()) {" in resolver
    # ALL-OR-NOTHING: both spellings of the guest's FMOD symbols.
    assert 'std::strncmp(name, "_ZN4FMOD", 8) == 0' in resolver
    assert 'std::strncmp(name, "FMOD_", 5) == 0' in resolver
    # It is applied at the ONE place the interception happens.
    assert resolver.count("guestFmodUsesRealLibrary()") >= 2, \
        "the lever is not consulted at the symbol lookup"

    # 5. A ROUND CAN TURN IT ON, WITH NO REBUILD.
    assert "[switch]$FmodReal," in round
    assert "-FmodReal:$FmodReal" in round
    assert "[switch]$FmodReal" in deployer
    assert '$text += "fmod=real' in deployer

    # 6. THE ROUND'S LOG SAYS WHAT THE LEVER DID AND HOW THE RELOCATIONS
    #    WENT.  Without these two lines a round cannot tell "the switch was
    #    off" from "the switch was never read", and cannot tell "the
    #    relocations were applied" from "nobody looked".
    assert "fmod-bind: lever=%s state=%d" in linker, \
        "the loader does not print the FMOD lever's state"
    assert "guestFmodLeverState()" in linker
    assert "isFmodSymbolName" in linker
    assert " rel=%llu int=%llu host=%llu cross=%llu weak=%llu patched=%llu verified=%llu" in linker, \
        "the loader does not print the relocation census"


    assert "result.verifiedWrites" in linker
    # THE PORT HAS ONE LOG FILE, AND THE ROUND ARCHIVES THAT ONE.  A
    # second path ("rr3-native-rehost/guest-view.log", with a slash) is a
    # DIFFERENT FILE on the card: the diagnostics go there, the round never
    # looks there, and the instrument is invisible.  THAT IS WHAT HAPPENED
    # ONCE, and it cost a whole round.
    assert "sdmc:/switch/rr3-native-rehost.guest-view.log" in linker
    assert "sdmc:/switch/rr3-native-rehost/guest-view.log" not in linker, \
        "the loader writes to a second log file that the round never reads"

    # A boot-time diagnostic that is not flushed may not exist: the line was
    # lost once, in the buffer, when the handle was closed on a flush error.
    # The assertion is about POSITION -- the flush comes right after the
    # line it belongs to -- because "the file contains the word flush" is
    # not the property.
    marker = "diag, 201, 0);"
    position = linker.find(marker)
    assert position >= 0
    tail = linker[position:position + 400]
    assert "flushGuestBootLog" in tail, \
        "the loader's boot diagnostics are not flushed"
    assert tail.find("flushGuestBootLog") - position < 260, \
        "the flush is not next to the line it belongs to"


    print("PASS: the PCM the guest produces is counted, the numbers reach the "
          "summary, and the FMOD lever is off by default and all-or-nothing")


if __name__ == "__main__":
    main()
