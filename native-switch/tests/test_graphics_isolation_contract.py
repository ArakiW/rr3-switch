#!/usr/bin/env python3
"""Static proof that Mesa and the software console never share one NRO stage."""

from pathlib import Path


def ordered(text: str, tokens: tuple[str, ...]) -> None:
    cursor = -1
    for token in tokens:
        cursor = text.index(token, cursor + 1)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    main_source = (root / "source" / "main.cpp").read_text(encoding="utf-8")
    probe = (root / "source" / "graphics_probe.cpp").read_text(encoding="utf-8")
    view_probe = (root / "source" / "guest_view_probe.cpp").read_text(encoding="utf-8")
    view_trace = (root / "source" / "guest_view_trace.cpp").read_text(encoding="utf-8")
    dashboard = (root / "source" / "ui_dashboard.cpp").read_text(encoding="utf-8")
    resolver = (root / "source" / "symbol_resolver.cpp").read_text(encoding="utf-8")
    bionic = (root / "source" / "bionic_shims.cpp").read_text(encoding="utf-8")
    makefile = (root / "Makefile").read_text(encoding="utf-8")

    branch = main_source.index('hasArgument(argc, argv, "--rr3-renderer")')
    graphics = main_source.index("runWindowGraphicsProbe", branch)
    result_argv = main_source.index('"--rr3-renderer-result=', graphics)
    child_return = main_source.index("return R_SUCCEEDED(returnResult)", result_argv)
    native_ui = main_source.index("nativeUiInit()")
    console = main_source.index("consoleInit(nullptr)")
    assert branch < graphics < result_argv < child_return < console < native_ui
    assert main_source.count("runWindowGraphicsProbe") == 1
    assert main_source.count("nativeUiInit()") == 2
    view_call = main_source.index("runGuestViewProbe")
    view_exit = main_source.rfind("rr3::nativeUiExit();", 0, view_call)
    view_window = main_source.index("NWindow* viewWindow = nwindowGetDefault();", view_exit)
    view_restore = main_source.index("rr3::nativeUiInit()", view_call)
    assert view_exit < view_window < view_call < view_restore
    assert main_source.count("runGuestViewProbe") == 1
    assert main_source.count("nativeUiInit()") == 2
    assert main_source.count("nwindowGetDefault()") >= 1
    assert main_source.count("consoleExit(nullptr)") == 1
    assert 'hasArgument(argc, argv, "--rr3-console")' in main_source
    assert "if (rr3::nativeUiActive())" in main_source
    assert "rr3::nativeUiExit();" in main_source


    ordered(
        probe,
        (
            "eglGetDisplay",
            "eglInitialize",
            "eglChooseConfig",
            "eglCreateWindowSurface",
            "eglCreateContext",
            "eglMakeCurrent(display, surface",
            "eglSwapBuffers",
        ),
    )
    assert (
        probe.rindex("eglMakeCurrent(display, EGL_NO_SURFACE")
        < probe.rindex("eglDestroyContext")
        < probe.rindex("eglDestroySurface")
        < probe.rindex("eglTerminate")
    )
    assert "eglReleaseThread" not in probe
    assert "eglBindAPI" not in probe
    ordered(
        view_probe,
        (
            "eglGetDisplay",
            "eglInitialize",
            "eglChooseConfig",
            "eglCreateWindowSurface",
            "eglCreateContext",
            "eglMakeCurrent(display, surface",
            "GuestTlsScope",
            'progressPath, "onViewCreatedJNI", 0',
            'progressPath, "onViewChangedJNI", 0',
            'progressPath, "onViewRenderJNI", 0',
            "eglSwapBuffers",
        ),
    )
    assert "eglReleaseThread" not in view_probe
    assert "eglBindAPI" not in view_probe
    assert "view-host-gl-outside-enter" in view_probe
    assert "view-host-gl-inside-enter" in view_probe
    assert "guestJniEnvironment()" in view_probe
    assert "guestActivityObject()" in view_probe
    assert "probe.changedJni = diffJniHistograms(afterChanged, afterCreated)" in view_probe
    for field, stage in (("2800Called", 9), ("2800Returned", 10),
                         ("3e38Called", 11), ("3e38Returned", 12),
                         ("240FirstCalled", 23), ("240FirstReturned", 24),
                         ("130Called", 25), ("130Returned", 26),
                         ("40FirstCalled", 27), ("40FirstReturned", 28),
                         ("40SecondCalled", 29), ("40SecondReturned", 30)):
        assert f"probe.postGlMalloc{field} = guestViewTraceReached({stage});" in view_probe
    assert "probe.lastTraceStage >=" not in view_probe
    assert "g_seenStages |= 1ULL << stage;" in view_trace
    assert 'guest_view_post_gl stage=%u malloc=2800:%d/%d 3e38:%d/%d 240-first:%d/%d 130:%d/%d 40-first:%d/%d 40-second:%d/%d' in dashboard
    ordered(
        view_probe,
        (
            "hostGlOutsideTlsCalled",
            "hostGlInsideTlsCalled",
            "enableGuestViewGlTrace",
            'progressPath, "onViewCreatedJNI", 0',
            'progressPath, "onViewChangedJNI", 0',
            'progressPath, "onViewRenderJNI", 0',
            "disableGuestViewGlTrace",
        ),
    )
    assert 'RR3_SHIM("glGetString", guestViewTracedGlGetString)' in resolver
    assert 'RR3_SHIM("malloc", guestViewTracedMalloc)' in resolver
    assert 'RR3_SHIM("memcpy", guestViewTracedMemcpy)' in resolver
    assert 'RR3_SHIM("free", guestViewTracedFree)' in resolver
    assert 'RR3_SHIM("memset", guestViewTracedMemset)' in resolver
    assert 'guestViewTraceStackCheckFailure' in view_trace
    assert 'guestViewTraceStackCheckFailure(reinterpret_cast<uintptr_t>(' in bionic
    assert 'view-post-gl-memcpy-enter' in view_trace
    assert 'view-post-gl-memcpy-return' in view_trace
    assert 'view-post-gl-free-enter' in view_trace
    assert 'view-post-gl-free-return' in view_trace
    assert '!guestViewTraceReached(30)' in view_trace
    assert 'trace=callers-v1 malloc40_caller=%016llx copy_caller=%016llx ' in view_trace
    assert 'dst=%016llx src=%016llx len=%llx free_caller=%016llx ptr=%016llx' in view_trace
    for field in ('g_second40Caller', 'g_copyCaller', 'g_copyDestination',
                  'g_copySource', 'g_copyLength', 'g_freeCaller', 'g_freeAddress'):
        assert view_trace.count(f'{field} = 0;') == 3, field
    ordered(view_trace, ('g_copyCaller = caller;', 'g_copyLength = length;',
                         'checkpointPhase("view-post-gl-memcpy-enter", 31,',
                         'void* result = std::memcpy(destination, source, length);',
                         'checkpointPhase("view-post-gl-memcpy-return", 32,'))
    ordered(view_trace, ('g_freeCaller = caller;', 'g_freeAddress = reinterpret_cast',
                         'checkpointPhase("view-post-gl-free-enter", 33,',
                         'std::free(allocation);',
                         'checkpointPhase("view-post-gl-free-return", 34,'))
    recovery = (root / 'source' / 'recovery_guard.cpp').read_text(encoding='utf-8')
    assert 'target=%016llx%s%s\\n' in recovery
    assert 'details != nullptr ? details : ""' in recovery
    assert 'fsync(fileno(file))' in recovery
    ordered(main_source, ('if (!rr3::prepareGuestCrashCapture())',
                          'rr3::nativeUiExit();', 'rr3::runGuestViewProbe(',
                          'rr3::finishGuestCrashCapture();'))
    handler = recovery.split('extern "C" void __libnx_exception_handler', 1)[1].split('namespace rr3', 1)[0]
    for register in ('pc', 'lr', 'sp', 'fp', 'far', 'esr', 'pstate'):
        assert f'crashHex("{register}", dump->{register}' in handler
    for forbidden in ('std::', 'malloc(', 'free(', 'fopen(', 'snprintf(', 'completeGuardedAction'):
        assert forbidden not in handler, forbidden
    assert 'svcExitProcess();' in handler
    assert 'fsFileWrite(&g_crashFile, 0, g_crashText, g_crashLength, FsWriteOption_Flush)' in handler
    assert '__nx_exception_stack[0x10000]' in recovery
    assert 'fsFsCreateFile(sd, path, 0, 0)' in recovery
    for api, phase, stage in (('CreateShader', 'create-shader', 38),
                              ('ShaderSource', 'shader-source', 40),
                              ('CompileShader', 'compile-shader', 42),
                              ('LinkProgram', 'link-program', 44)):
        assert f'RR3_SHIM("gl{api}", guestViewTracedGl{api})' in resolver
        assert f'"view-gl-{phase}-enter", {stage}' in view_trace
        assert f'"view-gl-{phase}-return", {stage + 1}' in view_trace
    for name, target in (('abort', 'guestTracedAbort'), ('exit', 'guestTracedExit'), ('raise', 'guestTracedRaise')):
        assert f'RR3_SHIM("{name}", {target})' in resolver
    for name, call in (('abort', 'std::abort();'), ('exit', 'std::exit(status);'), ('raise', 'return std::raise(signal);')):
        ordered(recovery, (f'recordGuestTermination("{name}"', call))
    terminal = recovery.split('void recordGuestTermination(', 1)[1].split('bool recoveryMarkerExists', 1)[0]
    for forbidden in ('std::', 'malloc(', 'fopen(', 'completeGuardedAction', 'svcExitProcess'):
        assert forbidden not in terminal
    assert 'status=termination-request' in terminal
    assert 'RR3 termination-v2' in terminal
    assert 'append(guestAbortMessage());' in terminal
    assert 'abort_message_begin' in terminal and 'abort_message_end' in terminal
    assert 'androidSetAbortMessage(nullptr);' in recovery
    libcxx = (root / 'source' / 'libcxx_shims.cpp').read_text(encoding='utf-8')
    assert 'thread_local char gAbortMessage[1024]' in libcxx
    assert 'gAbortMessage[sizeof(gAbortMessage) - 1] = ' in libcxx
    assert 'fsFileWrite(&g_crashFile, 0, text, size, FsWriteOption_Flush)' in terminal
    ordered(view_probe, ('probe.renderReturned = true;', '"onViewRenderJNI-return", 46',
                         '"view-gl-finish-enter", 47', 'glFinish();', '"view-gl-finish-return", 48',
                         '"view-swap-enter", 49', 'probe.swapped = eglSwapBuffers',
                         '"view-swap-return", 50', '"view-egl-release-enter", 51',
                         'releaseEgl(display, surface, context);', '"guest-view-complete"'))
    assert 'gltrace=shader-v1' in view_trace
    assert '"view-post-gl-malloc-%s-enter"' in view_trace
    assert '"view-post-gl-malloc-%s-return"' in view_trace
    assert '"view-post-gl-memset-%s-enter"' in view_trace
    assert '"view-post-gl-memset-%s-return"' in view_trace
    assert 'case 0x2800: return "2800"' in view_trace
    assert 'case 0x3e38: return "3e38"' in view_trace
    assert 'case 0x240: return occurrence == 1 ? "240-first" : nullptr' in view_trace
    assert 'case 0x130: return "130"' in view_trace
    assert 'case 0x40: return occurrence == 1 ? "40-first"' in view_trace
    assert 'occurrence == 2 ? "40-second" : nullptr' in view_trace
    assert 'size == 0x240 ? 23U' in view_trace
    assert 'size == 0x130 ? 25U' in view_trace
    assert 'size == 0x40 ? (occurrence == 1 ? 27U : 29U)' in view_trace
    assert 'vectorCall = ++g_vectorGrowth240Calls' in view_trace
    assert 'vectorCall = ++g_vectorStorage40Calls' in view_trace
    assert 'if (!guestViewTraceReached(26))' in view_trace
    assert 'if (vectorCall > 1)' in view_trace
    assert 'if (vectorCall > 2)' in view_trace
    assert view_trace.count('g_vectorGrowth240Calls = 0') >= 2
    assert view_trace.count('g_vectorStorage40Calls = 0') >= 2
    assert 'case 0x1550: return "1550"' in view_trace
    assert 'case 0x320: return "320"' in view_trace
    assert "__builtin_return_address(0)" in view_trace
    ordered(
        view_trace,
        (
            "void enableGuestViewGlTrace",
            "void disableGuestViewGlTrace",
            "const GLubyte* guestViewTracedGlGetString",
            "::glGetString(name)",
            "void* guestViewTracedMalloc",
            "std::malloc(size)",
            "void* guestViewTracedMemset",
            "std::memset(destination, value, length)",
        ),
    )
    assert "eglReleaseThread" not in view_probe
    assert "eglBindAPI" not in view_probe
    assert "frame-loop-enter" in view_probe
    assert "HidNpadButton_Plus" in view_probe
    assert "padInitializeDefault(&framePad)" in view_probe
    assert "-lEGL -lGLESv2 -lglapi -ldrm_nouveau" in makefile
    print("PASS: renderer child returns before either native UI backend; GLES2 EGL order locked")


if __name__ == "__main__":
    main()
