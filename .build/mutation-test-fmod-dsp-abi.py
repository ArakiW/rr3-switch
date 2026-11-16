#!/usr/bin/env python3
"""Mutation-test the FMOD_DSP_DESCRIPTION ABI contract.

THE PROPERTY: the port's header describes the struct the way THE GUEST
builds it -- same offsets, same declared types, same callback signatures --
and the shim keeps the description so the guest's own audio callback can be
reached and called.

Every mutant below breaks one of those, and every one of them MUST fail the
contract test.  A contract test that cannot fail is not a contract.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "include", "rr3", "fmod_shim.hpp"),
    ("native-switch", "source", "fmod_shim.cpp"),
    ("native-switch", "tests", "test_fmod_dsp_abi_contract.py"),
]
HEADER = "native-switch/include/rr3/fmod_shim.hpp"
SHIM = "native-switch/source/fmod_shim.cpp"

MUTANTS = (
    "A-the-name-becomes-a-pointer-again",
    "B-the-read-callback-loses-its-type",
    "C-the-description-is-thrown-away-again",
    "D-getUserData-ignores-the-description",
    "E-the-read-callbacks-arguments-are-swapped",
    "F-the-create-callback-loses-its-type",
    "G-the-getparameter-callback-loses-an-argument",
    "H-the-release-callback-loses-its-type",
)


def read(path):
    with io.open(path, "r", encoding="utf-8", newline="") as handle:
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
    if name == "A-the-name-becomes-a-pointer-again":
        path = os.path.join(root, HEADER)
        text = read(path)
        old = "    char name[32];"
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, "    const char* name;"))
    elif name == "B-the-read-callback-loses-its-type":
        path = os.path.join(root, HEADER)
        text = read(path)
        old = "    FMOD_DSP_READCALLBACK read;"
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, "    void* read;"))
    elif name == "C-the-description-is-thrown-away-again":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = "        created->description = description;"
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, "        created->description = nullptr;"))
    elif name == "D-getUserData-ignores-the-description":
        path = os.path.join(root, SHIM)
        text = read(path)
        old = "*userdata = self->description != nullptr ? self->description->userdata"
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, "*userdata = self->description != nullptr ? self->userData"))
    elif name == "E-the-read-callbacks-arguments-are-swapped":
        path = os.path.join(root, HEADER)
        text = read(path)
        pattern = re.compile(
            r"typedef int \(\*FMOD_DSP_READCALLBACK\)\(void\* state,\s*"
            r"float\* inbuffer,\s*float\* outbuffer,")
        assert pattern.search(text), "mutant E did not apply"
        write(path, pattern.sub(
            "typedef int (*FMOD_DSP_READCALLBACK)(void* state,\n"
            "                                     float* outbuffer,\n"
            "                                     float* inbuffer,", text))
    elif name == "F-the-create-callback-loses-its-type":
        path = os.path.join(root, HEADER)
        text = read(path)
        old = "    FMOD_DSP_CREATECALLBACK create;"
        assert old in text, "mutant F did not apply"
        write(path, text.replace(old, "    void* create;"))
    elif name == "G-the-getparameter-callback-loses-an-argument":
        path = os.path.join(root, HEADER)
        text = read(path)
        pattern = re.compile(
            r"typedef int \(\*FMOD_DSP_GETPARAMCALLBACK\)\(void\* state, int param,\s*"
            r"float\* value,\s*char\* valuestr\);")
        assert pattern.search(text), "mutant G did not apply"
        write(path, pattern.sub(
            "typedef int (*FMOD_DSP_GETPARAMCALLBACK)(void* state, int param,\n"
            "                                     float* value);", text))
    elif name == "H-the-release-callback-loses-its-type":
        path = os.path.join(root, HEADER)
        text = read(path)
        old = "    FMOD_DSP_RELEASECALLBACK release;"
        assert old in text, "mutant H did not apply"
        write(path, text.replace(old, "    void* release;"))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests",
                        "test_fmod_dsp_abi_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-dsp-abi-mutants-")
    failures = []

    tree = os.path.join(base, "baseline")
    build_tree(tree)
    code, last = run_test(tree)
    print("baseline          : exit=%d  %s" % (code, last[:80]))
    if code != 0:
        failures.append("the BASELINE does not pass")

    for name in MUTANTS:
        tree = os.path.join(base, name)
        build_tree(tree)
        mutate(tree, name)
        code, last = run_test(tree)
        caught = code != 0
        print("%-44s: exit=%d  %s   %s" % (name, code,
                                           "CAUGHT" if caught else "MISSED",
                                           last[:60]))
        if not caught:
            failures.append(name)

    shutil.rmtree(base, ignore_errors=True)
    if failures:
        print("FAIL: %d mutant(s) not caught: %s" % (len(failures),
                                                    ", ".join(failures)))
        return 1
    print("PASS: every one of the %d mutants was caught" % len(MUTANTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
