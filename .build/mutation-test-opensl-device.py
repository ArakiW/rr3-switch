#!/usr/bin/env python3
"""Mutation-test the OpenSL device contract test."""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "source", "fmod_audio_stub.cpp"),
    ("native-switch", "source", "guest_view_probe.cpp"),
    ("native-switch", "source", "main.cpp"),
    ("native-switch", "include", "rr3", "fmod_audio_stub.hpp"),
    ("native-switch", "tests", "test_opensl_device_contract.py"),
    ("native-switch", "romfs", "libfmodex.so"),
    ("tools", "Test-OnEden.ps1"),
]
STUB = "native-switch/source/fmod_audio_stub.cpp"
VIEW = "native-switch/source/guest_view_probe.cpp"
MAIN = "native-switch/source/main.cpp"
PS1 = "tools/Test-OnEden.ps1"


def read(path):
    with io.open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def write(path, text):
    with io.open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def build_tree(root):
    for parts in REL:
        src = os.path.join(REPO, *parts)
        dst = os.path.join(root, *parts)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)


def mutate(root, name):
    if name == "A-unknown-iid-answers-the-queue":
        path = os.path.join(root, STUB)
        text = read(path)
        old = """    // UNKNOWN IID: AN EXPLICIT FAILURE.  The old fallback ("the buffer
    // queue for anything") is exactly what made a MISSING interface look
    // like a PRESENT one.
    *out = nullptr;
    return kSlInterfaceUnavailable;"""
        new = """    *out = const_cast<void*>(static_cast<const void*>(&gBufferQueueItfCell));
    return kSlSuccess;"""
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, new))
    elif name == "B-device-muted-by-default":
        path = os.path.join(root, STUB)
        text = read(path)
        old = "constexpr int32_t kDefaultVolumeLevel = 100;"
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, "constexpr int32_t kDefaultVolumeLevel = 0;"))
    elif name == "C-configuration-export-dropped":
        path = os.path.join(root, STUB)
        text = read(path)
        old = """    if (std::strcmp(symbolName, "SL_IID_ANDROIDCONFIGURATION") == 0) {
        return reinterpret_cast<void*>(&gIidAndroidConfiguration);
    }
"""
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, ""))
    elif name == "D-hardcoded-expectation":
        path = os.path.join(root, MAIN)
        text = read(path)
        old = "static_cast<unsigned long long>(rr3::fmodAudioStubSelfTestExpected()));"
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, 'static_cast<unsigned long long>(0x3f));'))
    elif name == "E-census-drops-ast":
        path = os.path.join(root, VIEW)
        text = read(path)
        old = " ast=0x%llx arate=%u ach=%u"
        assert old in text, "mutant E did not apply"
        write(path, text.replace(old, ""))
    elif name == "F-census-does-not-read-ast":
        path = os.path.join(root, PS1)
        text = read(path)
        old = "$summary.msAudioSelfTest = $audioBits[$audioBits.Count - 1].Groups[1].Value"
        assert old in text, "mutant F did not apply"
        write(path, text.replace(old, "# mutant F: the bits are read and thrown away"))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests", "test_opensl_device_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-opensl-mutants-")
    failures = []

    tree = os.path.join(base, "baseline")
    build_tree(tree)
    code, last = run_test(tree)
    print("baseline          : exit=%d  %s" % (code, last[:90]))
    if code != 0:
        failures.append("the BASELINE does not pass")

    for name in ("A-unknown-iid-answers-the-queue", "B-device-muted-by-default",
                 "C-configuration-export-dropped", "D-hardcoded-expectation",
                 "E-census-drops-ast", "F-census-does-not-read-ast"):
        tree = os.path.join(base, name)
        build_tree(tree)
        mutate(tree, name)
        code, last = run_test(tree)
        caught = code != 0
        print("%-32s: exit=%d  %s   %s" % (name, code, "CAUGHT" if caught else "MISSED", last[:70]))
        if not caught:
            failures.append(name)

    shutil.rmtree(base, ignore_errors=True)
    if failures:
        print("FAIL: %d mutant(s) not caught: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("PASS: every mutant was caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
