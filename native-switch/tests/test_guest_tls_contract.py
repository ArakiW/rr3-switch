#!/usr/bin/env python3
"""Static contract for Android TPIDR_EL0 scope around guest lifecycle calls."""

from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    header = (root / "include" / "rr3" / "tls_probe.hpp").read_text(encoding="utf-8")
    tls = (root / "source" / "tls_probe.cpp").read_text(encoding="utf-8")
    linker = (root / "source" / "multi_dso_linker.cpp").read_text(encoding="utf-8")

    assert "class GuestTlsScope" in header
    assert "guestTlsScopeSelfTest" in header
    assert '"msr tpidr_el0, %0\\n\\tisb"' in tls
    assert "block_ + 0x28" in tls
    assert "writeTpidrEl0(previous_)" in tls
    assert "readTpidrEl0() == before" in tls

    init = linker.index("GuestInitializationResult initializeGuestDependencies")
    init_scope = linker.index("GuestTlsScope tlsScope", init)
    init_call = linker.index("reinterpret_cast<void (*)()>(target)()", init_scope)
    finalizer = linker.index("bool finalizeInitializedGuestImages")
    finalizer_scope = linker.index("GuestTlsScope tlsScope", finalizer)
    finalizer_call = linker.index("reinterpret_cast<void (*)()>(target)()", finalizer_scope)
    assert init < init_scope < init_call
    assert finalizer < finalizer_scope < finalizer_call
    pthread = (root / "source" / "pthread_bridge.cpp").read_text(encoding="utf-8")
    trampoline = pthread.split("void* guestThreadStart(void* opaque)", 1)[1].split("struct ThreadTlsSelfTest", 1)[0]
    assert trampoline.index("std::free(opaque)") < trampoline.index("GuestTlsScope tlsScope")
    assert trampoline.index("GuestTlsScope tlsScope") < trampoline.index("context.start(context.argument)")
    assert trampoline.index("context.start(context.argument)") < trampoline.index("guestPthreadExit(result)")
    assert "return context.start" not in trampoline
    assert "::pthread_create(&hostThread, &hostAttribute, guestThreadStart, context)" in pthread
    assert "::pthread_create(&hostThread, &hostAttribute, start, argument)" not in pthread
    assert "if (result != 0) {\n                std::free(context);" in pthread
    assert "threadTlsSelfTestDestructor" in pthread
    assert "int guestPthreadDetach(GuestThread thread)" in pthread
    assert "Horizon's pthread_detach returns ENOSYS" not in pthread
    assert "ThreadLifecycle::Detached" in pthread
    assert "reapDetachedThreadWorker" in pthread
    assert "int startDetachedReaper()" in pthread
    assert "startDetachedReaperLocked" not in pthread
    assert "slot->joinable = false" in pthread
    assert "PTHREAD_CREATE_JOINABLE" in pthread
    assert "PTHREAD_CREATE_DETACHED" not in pthread
    assert "test.entryValid && test.destructorValid" in pthread
    assert "test.explicitExit = exitMode != 0" in pthread
    assert "guestViewTraceSyncBlocked" in pthread
    assert '"mutex", 52' in pthread
    assert '"cond-wait", 53' in pthread
    # Quiesce must arm the reaper exit flag and join ONLY the reaper.
    # Joining guest workers blocks handoff forever (device rounds 7-8).
    quiesce = pthread.split("unsigned int guestPthreadQuiesceAll()", 1)[1].split(
        "unsigned int guestPthreadLiveCount", 1
    )[0]
    assert quiesce.index("gQuiesceRequested = true") < quiesce.index("condvarWakeAll(&gThreadsChanged)")
    assert quiesce.index("gQuiesceRequested = true") < quiesce.index("::pthread_join(reaperHost")
    assert "::pthread_join(host" not in quiesce
    assert "reapDetachedThreadSlotLocked" not in quiesce
    assert "gReaperGuest" in pthread
    assert "guestPthreadLiveCount" in pthread
    assert "if (gQuiesceRequested)" in pthread.split("int guestPthreadCreate", 1)[1].split("int guestPthreadJoin", 1)[0]
    main = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    assert "guestPthreadLiveCount" in main
    assert "handoff-quiesce-stuck" in main
    assert "skipUnloadAfterExit" in main
    assert "handoff-dirty-exit" in main
    assert "handoff-process-exit" in main
    # Dirty exit must terminate immediately after the process-exit breadcrumb;
    # running nativeUiExit/unmount races live guest workers (round 9 error).
    dirty_block = main.split("if (skipUnloadAfterExit)", 1)[1].split(
        "if (!dashboard.unload.completed)", 1
    )[0]
    assert "handoff-process-exit" in dirty_block
    assert "svcExitProcess" in dirty_block
    assert "nativeUiExit" not in dirty_block
    assert "unmountExternalData" not in dirty_block
    trace = (root / "source" / "guest_view_trace.cpp").read_text(encoding="utf-8")
    assert "thread_local bool g_enabled = false" in trace
    assert "view-sync-blocked-%s" in trace
    print("PASS: guest lifecycle and pthread TLS entry/exit/destructor contracts")


if __name__ == "__main__":
    main()
