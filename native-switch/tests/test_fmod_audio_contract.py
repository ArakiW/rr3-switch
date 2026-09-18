#!/usr/bin/env python3
"""Static contract: FMOD guest audio must not deadlock on Horizon.

Silent OpenSL device stub + clear AAudio errors + bounded sem_wait.
Must never claim audible output (audible is always 0).
"""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    header = (root / "include" / "rr3" / "fmod_audio_stub.hpp").read_text(encoding="utf-8")
    stub = (root / "source" / "fmod_audio_stub.cpp").read_text(encoding="utf-8")
    sem = (root / "source" / "semaphore_bridge.cpp").read_text(encoding="utf-8")
    resolver = (root / "source" / "symbol_resolver.cpp").read_text(encoding="utf-8")
    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")
    ui = (root / "source" / "ui_dashboard.cpp").read_text(encoding="utf-8")
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    session = (root / "tests" / "reference_loader_session.py").read_text(encoding="utf-8")
    startup = (root / "tests" / "test_startup_plan_contract.py").read_text(encoding="utf-8")
    analysis = (root / "FMOD_CONSTRUCTOR_ANALYSIS.md").read_text(encoding="utf-8")

    # 1. Honest telemetry: audible is always zero, never "already sounding".
    assert "audible" in header
    assert "copy.audible = 0" in stub
    assert "audible stays 0" in stub or "audible is ALWAYS zero" in header
    assert "silent=yes" in ui
    # Must not claim playback succeeded on hardware.
    assert "playing=yes" not in stub
    assert "audio_ok=yes" not in ui

    # 2. Silent OpenSL device: engine create + enqueue discard, no hardware wait.
    assert "slCreateEngine" in stub
    assert "bufferEnqueues" in stub
    assert "SL_RESULT" not in stub or "kSlSuccess" in stub
    assert "fmodAudioStubSelfTest" in stub
    assert "fmodAudioStubSelfTest" in main_source

    # 3. AAudio refuses with a clear unavailable code, not a hang.
    assert "kAAudioErrorUnavailable" in stub
    assert "aaudioCreateStreamBuilder" in stub
    assert "AAudioStreamBuilder_create" in resolver
    assert 'RR3_SHIM("slCreateEngine"' in resolver

    # 4. Virtual audio DSOs: guestDlopen never blocks on a missing device lib.
    assert "libOpenSLES.so" in linker
    assert "libaaudio.so" in linker
    assert "isVirtualAudioLibrary" in linker
    assert "resolveVirtualAudioSymbol" in linker
    assert '"libOpenSLES.so"' in session
    assert '"libaaudio.so"' in session

    # 5. Bounded sem_wait slices (no raw unbounded semaphoreWait in guestSemWait).
    assert "semaphoreWait(" not in sem  # replaced by trywait loop
    assert "semaphoreTryWait" in sem
    assert 'guestViewTraceSyncBlocked("sem-wait"' in sem
    assert "kSyncBlockedReportNanos" in sem

    # 6. Cond wait remains sliced (pre-existing contract for FMOD worker threads).
    pthread = (root / "source" / "pthread_bridge.cpp").read_text(encoding="utf-8")
    assert "guestViewTraceSyncBlocked(\"cond-wait\"" in pthread

    # 7. Dependency ctor 3/3 path is untouched: startup plan still expects 3.
    assert 'assert plan["dependency_constructor_count"] == 3' in startup

    # 8. Analysis documents the audio policy without claiming sound works.
    assert "silent" in analysis.lower()
    assert "audible" in analysis.lower() or "not audible" in analysis.lower()

    print(
        "PASS: FMOD audio silent stub, clear AAudio errors, bounded sem-wait, "
        "no false audible claim, dependency ctor contract preserved"
    )


if __name__ == "__main__":
    main()
