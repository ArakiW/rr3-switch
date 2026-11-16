# DOES THE GUEST-ABI CONTRACT TEST ACTUALLY CATCH ANYTHING?
#
# WHY: a contract test that has never been falsified may be protecting
# nothing.  Every mutant below is a REAL defect that the port has actually
# had, or a plausible way it could regress, and every one of them MUST make
# the contract test FAIL.  If a mutant survives, the test is answering the
# ledger and not the code.
#
# RUN:  python .build/mutation-test-fmod-guest-abi.py
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / "native-switch" / "tests" / "test_fmod_guest_abi_contract.py"
HPP = ROOT / "native-switch" / "include" / "rr3" / "fmod_shim.hpp"
CPP = ROOT / "native-switch" / "source" / "fmod_shim.cpp"
PROBE = ROOT / "native-switch" / "source" / "guest_view_probe.cpp"

MUTANTS = [
    (
        "A-the-openstate-goes-back-to-modern-fmod-5",
        HPP,
        r"FMOD_OPENSTATE_READY = 0,",
        "FMOD_OPENSTATE_READY = 3,",
    ),
    (
        "B-the-stereo-speaker-mode-goes-back-to-3",
        HPP,
        r"FMOD_SPEAKERMODE_STEREO = 2,",
        "FMOD_SPEAKERMODE_STEREO = 3,",
    ),
    (
        "C-the-dsp-type-list-grows-a-fader-again",
        HPP,
        r"FMOD_DSP_TYPE_FLANGE = 7,",
        "FMOD_DSP_TYPE_FADER = 7,\n    FMOD_DSP_TYPE_FLANGE = 8,",
    ),
    (
        "D-the-exinfo-fields-move-back-one-slot",
        HPP,
        r"    unsigned int numchannels;          // \+0x0c\n"
        r"    unsigned int defaultfrequency;     // \+0x10\n"
        r"    unsigned int format;               // \+0x14\n"
        r"    unsigned int decodebufferlength;   // \+0x18",
        "    unsigned int decodebufferlength;   // +0x0c\n"
        "    unsigned int numchannels;          // +0x10\n"
        "    unsigned int defaultfrequency;     // +0x14\n"
        "    unsigned int format;               // +0x18",
    ),
    (
        "E-the-length-answers-only-type-zero-again",
        CPP,
        r"\*length = \(lengthtype <= 2U\)",
        "*length = (lengthtype == 0U)",
    ),
    (
        "F-the-position-answers-only-type-zero-again",
        CPP,
        r"\*position = \(positiontype <= 1U\)",
        "*position = (positiontype == 0U)",
    ),
    (
        "G-the-format-request-overwrites-the-device-again",
        CPP,
        r"    self->requestedRate = samplerate;",
        "    self->requestedRate = samplerate;\n    self->softwareRate = samplerate;",
    ),
    (
        "H-the-census-loses-the-gate-counter",
        PROBE,
        r"osq=%llu osr=%llu ",
        "osq=%llu ",
    ),
]


def read(path):
    return path.read_text(encoding="utf-8", newline="").replace("\r\n", "\n")


def write(path, text):
    path.write_text(text.replace("\n", "\r\n"), encoding="utf-8", newline="")


def run_test():
    return subprocess.run(
        [sys.executable, str(TEST)],
        capture_output=True,
        text=True,
    )


baseline = run_test()
if baseline.returncode != 0:
    print("FAIL: the contract test does not pass on the unmodified tree")
    print(baseline.stdout)
    print(baseline.stderr)
    sys.exit(1)
print("baseline : exit=0  " + baseline.stdout.strip())

failures = []
for name, path, pattern, replacement in MUTANTS:
    original = read(path)
    mutated, count = re.subn(pattern, replacement, original, count=1)
    if count != 1:
        failures.append((name, "the mutant did not apply (pattern not found)"))
        print("%-52s : MUTANT DID NOT APPLY" % name)
        continue
    try:
        write(path, mutated)
        result = run_test()
    finally:
        write(path, original)
    if result.returncode == 0:
        failures.append((name, "the mutant SURVIVED"))
        print("%-52s : exit=0  MISSED" % name)
    else:
        first = ""
        for line in result.stdout.splitlines():
            if line.strip().startswith("-"):
                first = line.strip()[1:].strip()
                break
        print("%-52s : exit=%d  CAUGHT  %s" % (name, result.returncode, first[:90]))

if failures:
    print()
    print("FAIL: %d of %d mutants were not caught" % (len(failures), len(MUTANTS)))
    for name, why in failures:
        print("  - %s: %s" % (name, why))
    sys.exit(1)

print()
print("PASS: every one of the %d mutants was caught" % len(MUTANTS))
