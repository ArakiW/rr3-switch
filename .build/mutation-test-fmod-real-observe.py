#!/usr/bin/env python3
"""Mutation-test the real-FMOD observation contract.

Every mutant below breaks one property of
native-switch/tests/test_fmod_real_observe_contract.py, and every one of them
MUST make that test fail.  A contract test that cannot fail is not a
contract.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

REPO = r"E:\AI\RealRacing3-switch"
REL = [
    ("native-switch", "source", "pthread_bridge.cpp"),
    ("native-switch", "source", "symbol_resolver.cpp"),
    ("native-switch", "tests", "test_fmod_real_observe_contract.py"),
]
PTHREAD = "native-switch/source/pthread_bridge.cpp"
RESOLVER = "native-switch/source/symbol_resolver.cpp"

MUTANTS = (
    "A-the-attribute-refuses-a-re-init-again",
    "B-the-observer-calls-the-port-own-shim",
    "C-the-observer-drops-the-real-result",
    "D-the-resolver-stops-routing-the-observer",
    "E-the-observed-name-only-exists-in-a-comment",
    "F-the-busy-mutex-path-is-no-longer-named",
)


def read(path):
    # NORMALISE THE LINE ENDINGS.  The sources are CRLF on this machine, and
    # a mutation pattern written with "\n" would simply not match -- which
    # looks exactly like "the mutant did not apply" and is not the same
    # thing.
    with io.open(path, "r", encoding="utf-8", newline="") as handle:
        return handle.read().replace("\r\n", "\n")


def write(path, text):
    with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def build_tree(root):
    for parts in REL:
        src = os.path.join(REPO, *parts)
        dst = os.path.join(root, *parts)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)


def mutate(root, name):
    if name == "A-the-attribute-refuses-a-re-init-again":
        path = os.path.join(root, PTHREAD)
        text = read(path)
        old = """        existing->type = PTHREAD_MUTEX_NORMAL;
        existing->used = true;
        mutexUnlock(&gRegistryLock);
        pthreadBridgeDiag("attr-reinit", attribute, caller);
        return 0;"""
        new = """        existing->type = PTHREAD_MUTEX_NORMAL;
        existing->used = true;
        mutexUnlock(&gRegistryLock);
        pthreadBridgeDiag("attr-reinit", attribute, caller);
        return EBUSY;"""
        assert old in text, "mutant A did not apply"
        write(path, text.replace(old, new))
    elif name == "B-the-observer-calls-the-port-own-shim":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """        real = reinterpret_cast<CreateFn>(
            findLoaderExportBySoname("libfmodex.so", "FMOD_System_Create"));"""
        new = """        real = reinterpret_cast<CreateFn>(&::FMOD_System_Create);"""
        assert old in text, "mutant B did not apply"
        write(path, text.replace(old, new))
    elif name == "C-the-observer-drops-the-real-result":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """        result = real(system);
        handle = (system != nullptr) ? *system : nullptr;"""
        new = """        real(system);
        handle = (system != nullptr) ? *system : nullptr;"""
        assert old in text, "mutant C did not apply"
        write(path, text.replace(old, new))
    elif name == "D-the-resolver-stops-routing-the-observer":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """                if (std::strcmp(symbol.name, "FMOD_System_Create") == 0) {
                    return reinterpret_cast<uintptr_t>(&fmodRealSystemCreateObserved);
                }
                return 0;"""
        new = """                return 0;"""
        assert old in text, "mutant D did not apply"
        write(path, text.replace(old, new))
    elif name == "E-the-observed-name-only-exists-in-a-comment":
        path = os.path.join(root, RESOLVER)
        text = read(path)
        old = """                if (std::strcmp(symbol.name, "FMOD_System_Create") == 0) {"""
        new = """                // "FMOD_System_Create" is the observed entry point.
                if (false) {"""
        assert old in text, "mutant E did not apply"
        write(path, text.replace(old, new))
    elif name == "F-the-busy-mutex-path-is-no-longer-named":
        path = os.path.join(root, PTHREAD)
        text = read(path)
        old = """        pthreadBridgeDiag("mutex-init-busy", mutex, caller);
        return EBUSY;"""
        new = """        return EBUSY;"""
        assert old in text, "mutant F did not apply"
        write(path, text.replace(old, new))
    else:
        raise SystemExit("unknown mutant " + name)


def run_test(root):
    test = os.path.join(root, "native-switch", "tests",
                        "test_fmod_real_observe_contract.py")
    proc = subprocess.run([sys.executable, test], capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip().split("\n")[-1]


def main():
    base = tempfile.mkdtemp(prefix="rr3-fmod-real-observe-mutants-")
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
