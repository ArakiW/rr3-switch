#!/usr/bin/env python3
"""Mutation-test the P1 instrument and the FMOD lever."""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "source", "fmod_audio_stub.cpp"),
    ("native-switch", "include", "rr3", "fmod_audio_stub.hpp"),
    ("native-switch", "source", "symbol_resolver.cpp"),
    ("native-switch", "source", "multi_dso_linker.cpp"),
    ("native-switch", "source", "guest_view_probe.cpp"),
    ("native-switch", "tests", "test_fmod_lever_contract.py"),
    ("tools", "Test-OnEden.ps1"),
    (".build", "deploy-census-conf.ps1"),
    (".build", "run-pinned-round.ps1"),
]
STUB = "native-switch/source/fmod_audio_stub.cpp"
LINKER = "native-switch/source/multi_dso_linker.cpp"
HEADER = "native-switch/include/rr3/fmod_audio_stub.hpp"
RESOLVER = "native-switch/source/symbol_resolver.cpp"
VIEW = "native-switch/source/guest_view_probe.cpp"
PS1 = "tools/Test-OnEden.ps1"
DEPLOYER = ".build/deploy-census-conf.ps1"
ROUND = ".build/run-pinned-round.ps1"


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
    if name == "A-self-test-bytes-count-as-the-guests":
        path = os.path.join(root, STUB)
        text = read(path)
        old = "    if (!gInSelfTest) {"
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, "    if (true) {"))
    elif name == "B-the-format-is-never-read":
        path = os.path.join(root, STUB)
        text = read(path)
        old = "        gStats.formatRate = format->sampleRate;"
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, "        gStats.formatRate = 0;"))
    elif name == "C-census-drops-pcm":
        path = os.path.join(root, VIEW)
        text = read(path)
        old = " pcm_enq=%u pcm_bytes=%u"
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, ""))
    elif name == "D-census-does-not-read-pcm":
        path = os.path.join(root, PS1)
        text = read(path)
        old = "$summary.msPcmEnqueues = [int]$pcmEnq[$pcmEnq.Count - 1].Groups[1].Value"
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, "# mutant D: read and thrown away"))
    elif name == "E-the-lever-withholds-only-half":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """    return std::strncmp(name, "_ZN4FMOD", 8) == 0 ||
        std::strncmp(name, "FMOD_", 5) == 0;"""
        assert old in text, "mutant E did not apply"
        write(path, text.replace(old, '    return std::strncmp(name, "_ZN4FMOD", 8) == 0;'))
    elif name == "F-the-lever-is-always-on":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """    static int cached = 0; // 0 = not read yet, 1 = the port's shim, 2 = real"""
        assert old in text, "mutant F did not apply"
        write(path, text.replace(old, """    static int cached = 0; // 0 = not read yet, 1 = the port's shim, 2 = real
    if (true) { return true; }"""))
    elif name == "G-the-lever-state-is-never-printed":
        path = os.path.join(root, LINKER)
        text = read(path)
        old = '"fmod-bind: lever=%s state=%d cross=%llu host=%llu missing=%llu"'
        assert old in text, "mutant G did not apply"
        write(path, text.replace(old, '"fmod-bind: lever=unknown state=0 cross=0 host=0 missing=0"'))
    elif name == "H-the-relocation-census-is-not-printed":
        path = os.path.join(root, LINKER)
        text = read(path)
        old = '" rel=%llu int=%llu host=%llu cross=%llu weak=%llu patched=%llu verified=%llu"'
        assert old in text, "mutant H did not apply"
        write(path, text.replace(old, '"'))
    elif name == "I-the-boot-diagnostics-are-not-flushed":
        path = os.path.join(root, LINKER)
        text = read(path)
        old = 'rr3::flushGuestBootLog("sdmc:/switch/rr3-native-rehost.guest-view.log");'
        assert old in text, "mutant I did not apply"
        write(path, text.replace(old, ""))
    elif name == "J-the-loader-writes-to-a-second-log-file":
        path = os.path.join(root, LINKER)
        text = read(path)
        old = '"sdmc:/switch/rr3-native-rehost.guest-view.log"'
        assert old in text, "mutant J did not apply"
        write(path, text.replace(old, '"sdmc:/switch/rr3-native-rehost/guest-view.log"', 1))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests", "test_fmod_lever_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-lever-mutants-")
    failures = []

    tree = os.path.join(base, "baseline")
    build_tree(tree)
    code, last = run_test(tree)
    print("baseline          : exit=%d  %s" % (code, last[:80]))
    if code != 0:
        failures.append("the BASELINE does not pass")

    for name in ("A-self-test-bytes-count-as-the-guests", "B-the-format-is-never-read",
                 "C-census-drops-pcm", "D-census-does-not-read-pcm",
                 "E-the-lever-withholds-only-half", "F-the-lever-is-always-on",
                 "G-the-lever-state-is-never-printed",

                 "H-the-relocation-census-is-not-printed",

                 "I-the-boot-diagnostics-are-not-flushed",
                 "J-the-loader-writes-to-a-second-log-file"):


        tree = os.path.join(base, name)
        build_tree(tree)
        mutate(tree, name)
        code, last = run_test(tree)
        caught = code != 0
        print("%-38s: exit=%d  %s   %s" % (name, code, "CAUGHT" if caught else "MISSED", last[:60]))
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
