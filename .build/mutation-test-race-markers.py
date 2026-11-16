#!/usr/bin/env python3
"""Mutation-test the race-marker contract test.

A test that has never been shown to FAIL is not a test.  This copies the
five files the contract test reads into a scratch tree, breaks the
property five different ways, and requires the test to fail on every one.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "source", "guest_filesystem.cpp"),
    ("native-switch", "source", "guest_view_probe.cpp"),
    ("native-switch", "include", "rr3", "guest_filesystem.hpp"),
    ("native-switch", "tests", "test_guest_race_marker_contract.py"),
    ("tools", "Test-OnEden.ps1"),
]

FS = "native-switch/source/guest_filesystem.cpp"
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
    if name == "A-guard-removed":
        path = os.path.join(root, FS)
        text = read(path)
        old = """            if (std::strstr(counted, "gui/processed/StartLightsScreen.gui") != nullptr) {
                __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);
            }"""
        new = """            __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);"""
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, new))
    elif name == "A2-braces-removed":
        path = os.path.join(root, FS)
        text = read(path)
        old = """            if (std::strstr(counted, "gui/processed/StartLightsScreen.gui") != nullptr) {
                __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);
            }"""
        new = """            if (std::strstr(counted, "gui/processed/StartLightsScreen.gui") != nullptr)
                __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);"""
        assert old in text, "mutant A2 did not apply"
        write(path, text.replace(old, new))
    elif name == "B-marker-typo":
        path = os.path.join(root, FS)
        text = read(path)
        old = "gui/processed/StartLightsScreen.gui"
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, "gui/processed/StartLights.gui"))
    elif name == "C-census-uses-text":
        path = os.path.join(root, PS1)
        text = read(path)
        old = "$summary.msStartLights = [int]$portStartLights[$portStartLights.Count - 1].Groups[1].Value"
        new = "$summary.msStartLights = [regex]::Matches($guestText, 'StartLightsScreen').Count"
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, new))
    elif name == "D-census-takes-first":
        path = os.path.join(root, PS1)
        text = read(path)
        old = "$portStartLights[$portStartLights.Count - 1]"
        new = "$portStartLights[0]"
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, new))
    elif name == "E-back-in-the-trace":
        path = os.path.join(root, FS)
        text = read(path)
        block = """            if (std::strstr(counted, "gui/processed/StartLightsScreen.gui") != nullptr) {
                __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);
            }
"""
        assert block in text, "mutant E did not apply"
        text = text.replace(block, "")
        anchor = "void logResolvedPathOnce(const char* path) {\n"
        assert anchor in text
        text = text.replace(
            anchor,
            anchor
            + "    if (std::strstr(path, \"gui/processed/StartLightsScreen.gui\") != nullptr) {\n"
            + "        __atomic_add_fetch(&g_startLightsOpens, 1ULL, __ATOMIC_RELAXED);\n"
            + "    }\n")
        write(path, text)
    elif name == "F-numbers-back-at-the-tail":
        path = os.path.join(root, VIEW)
        text = read(path)
        old = '"frame=%llu lights=%llu race=%llu census bound=%d'
        new = '"frame=%llu census bound=%d'
        assert old in text, "mutant F did not apply"
        text = text.replace(old, new)
        old2 = '" gui=%llu twosq=%llu",'
        new2 = '" gui=%llu twosq=%llu lights=%llu race=%llu",'
        assert old2 in text
        write(path, text.replace(old2, new2))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests", "test_guest_race_marker_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-mutants-")
    failures = []

    tree = os.path.join(base, "baseline")
    build_tree(tree)
    code, last = run_test(tree)
    print("baseline          : exit=%d  %s" % (code, last[:110]))
    if code != 0:
        failures.append("the BASELINE does not pass")

    for name in ("A-guard-removed", "A2-braces-removed", "B-marker-typo", "C-census-uses-text",
                 "D-census-takes-first", "E-back-in-the-trace",
                 "F-numbers-back-at-the-tail"):
        tree = os.path.join(base, name)
        build_tree(tree)
        mutate(tree, name)
        code, last = run_test(tree)
        caught = code != 0
        print("%-18s: exit=%d  %s   %s" % (name, code, "CAUGHT" if caught else "MISSED", last[:90]))
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
