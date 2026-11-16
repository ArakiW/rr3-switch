#!/usr/bin/env python3
"""Mutation-test the FMOD pool contract test.

A test that has never been shown to fail is not a test.  Each mutant below
breaks the property in one way; the test has to fail on every one.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "source", "fmod_shim.cpp"),
    ("native-switch", "source", "guest_view_probe.cpp"),
    ("native-switch", "tests", "test_fmod_pool_contract.py"),
    ("tools", "Test-OnEden.ps1"),
]
SHIM = "native-switch/source/fmod_shim.cpp"
VIEW = "native-switch/source/guest_view_probe.cpp"
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
    if name == "A-no-call-in-createSound":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = """    // THE POOL MUST HAVE A FREE SLOT.  See markPoolsFree().
    markPoolsFree();
    FMOD::Sound* created = nullptr;"""
        new = """    FMOD::Sound* created = nullptr;"""
        if old not in text:
            old = """    // THE POOL MUST HAVE A FREE SLOT.  See markPoolsFree().
    markPoolsFree();
"""
            new = ""
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, new, 1))
    elif name == "B-dsp-pool-not-marked":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = "        gDsps[index].released = true;"
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, "        gDsps[index].released = false;", 1))
    elif name == "C-marker-is-a-no-op":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = """    for (unsigned int index = 0; index < kMaxSounds; ++index) {
        gSounds[index].released = true;
        gSounds[index].inUse = false;
    }"""
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, "    // nothing is marked free", 1))
    elif name == "D-census-drops-fmst":
        path = os.path.join(root, VIEW)
        text = read(path)
        old = " fmst=0x%llx snd=%llu str=%llu ch=%llu"
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, "", 1))
    elif name == "E-census-does-not-read-fmst":
        path = os.path.join(root, PS1)
        text = read(path)
        old = "' fmst=0x([0-9a-fA-F]+)'"
        assert old in text, "mutant E did not apply"
        write(path, text.replace(old, "' fmst=0x([0-9a-fA-F]+)'", 1).replace(
            "$summary.msFmodSelfTest = $fmodBits[$fmodBits.Count - 1].Groups[1].Value",
            "# mutant E: the bits are read and thrown away"))
    elif name == "F-expected-value-changed":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = 'expected=0xfff'
        assert old in text, "mutant F did not apply"
        write(path, text.replace(old, 'expected=0xf1f', 1))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests", "test_fmod_pool_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-fmod-mutants-")
    failures = []

    tree = os.path.join(base, "baseline")
    build_tree(tree)
    code, last = run_test(tree)
    print("baseline          : exit=%d  %s" % (code, last[:100]))
    if code != 0:
        failures.append("the BASELINE does not pass")

    for name in ("A-no-call-in-createSound", "B-dsp-pool-not-marked",
                 "C-marker-is-a-no-op", "D-census-drops-fmst",
                 "E-census-does-not-read-fmst", "F-expected-value-changed"):
        tree = os.path.join(base, name)
        build_tree(tree)
        mutate(tree, name)
        code, last = run_test(tree)
        caught = code != 0
        print("%-30s: exit=%d  %s   %s" % (name, code, "CAUGHT" if caught else "MISSED", last[:80]))
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
