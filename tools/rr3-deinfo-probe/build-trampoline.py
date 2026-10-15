#!/usr/bin/env python3
"""Regenerate and verify the rr3 DeInfo probe trampoline byte array.

Assembles tools/rr3-deinfo-probe/trampoline.s with the project's devkitPro
aarch64 toolchain, dumps it with the matching objdump, and emits the frozen
`kTrampolineCode` array for native-switch/source/rr3_deinfo_probe.cpp.

The point of this script is that the byte array in the .cpp is never hand-typed.
It is whatever this assembler produces, and the checks below fail loudly if the
assembler's output stops matching what the .cpp promises.

WHAT CHANGED AND WHY
--------------------
The trampoline used to carry three ADRP/ADD reloc pairs for its capture slots,
replay one guest instruction, and `ret`. jitCreate() puts this blob 12.4 GB from
the guest site, and neither a 128 MiB `B` nor a 4 GiB ADRP reaches that far, so
the entry jump AND all three data relocations were unreachable and
deinfo-install failed closed every run. The trampoline now reaches every
non-local address through a 64-bit `ldr xN, =const` literal, which has no
distance limit at all, and exits by `br` on a literal-loaded register. The
checks below assert that no distance-dependent reloc survives.

Run:
    python tools/rr3-deinfo-probe/build-trampoline.py            # verify + print
    python tools/rr3-deinfo-probe/build-trampoline.py --emit     # print C array
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
ASM = HERE / "trampoline.s"
TOOLBIN = REPO / ".tools" / "devkitPro" / "msys64" / "opt" / "devkitpro" / "devkitA64" / "bin"
OUT = REPO / ".build" / "deinfo"
CPP = REPO / "native-switch" / "source" / "rr3_deinfo_probe.cpp"
HPP = REPO / "native-switch" / "include" / "rr3" / "rr3_deinfo_probe.hpp"

# The 16-byte window install() overwrites, read straight out of the guest ELF by
# check_guest() below rather than trusted from a brief.
GUEST_SITE_VA = 0xCD2AA0
GUEST_WINDOW_BYTES = 16
# Guest 0xcd2aa4 is `cbz x20, 0xcd2b0c` and the guest resumes at site+16. Both
# are reproduced by the trampoline as absolute transfers, so both matter.
GUEST_CBZ_TARGET_VA = 0xCD2B0C
GUEST_RESUME_VA = 0xCD2AB0

# The three guest instructions that are position independent and therefore
# replayed with byte-identical encodings, by offset in the trampoline image.
# They are guest window words 0, 2 and 3; word 1 (the cbz) is not replayed
# verbatim because its displacement is PC-relative.
REPLAY_OFFSETS = {0x0F8: 0xF85B83B4, 0x120: 0xF85C03B5, 0x124: 0xEB1402BF}

# Byte offset of the 64-bit literal pool and its size. The pool must start on an
# 8-byte boundary; the single load-bearing `nop` at 0x12c exists to make that
# true without the assembler inserting filler.
POOL_OFFSET = 0x130
POOL_BYTES = 40
CODE_BYTES = POOL_OFFSET

# The two control transfers out of the trampoline. There is no `b`, no `bl` and
# no `ret` anywhere: the blob is entered by the guest's own `br` and leaves by
# one of these.
BRANCH_OUT_OFFSETS = {0x11C, 0x128}

# Byte offsets of the five `ldr xN, <literal>` instructions, with the exact
# encodings the assembler must produce. install() asserts each one before it
# writes, so a re-assembly that moves one of these makes install() refuse.
LDR_LITERAL_WORDS = {
    0x078: 0x580005D0,  # x16 <- &g_rr3DeInfoB      (litB)
    0x07C: 0x580005F1,  # x17 <- &g_rr3DeInfoE      (litE)
    0x080: 0x58000612,  # x18 <- &g_rr3DeInfoSeq    (litSeq)
    0x0FC: 0x58000270,  # x16 <- guest 0xcd2b0c     (litCbz)
    0x118: 0x580001D1,  # x17 <- guest 0xcd2ab0     (litResume)
}

# Byte offsets of the literal slots install() overwrites, with the placeholder
# the assembler must have left there.
POOL_SLOTS = {
    "litB": 0x130,
    "litE": 0x138,
    "litSeq": 0x140,
    "litCbz": 0x148,
    "litResume": 0x150,
}
POOL_PLACEHOLDERS = {
    "litB": 0x00000000C0DE0001,
    "litE": 0x00000000C0DE0002,
    "litSeq": 0x00000000C0DE0003,
    "litCbz": 0x00000000C0DE0004,
    "litResume": 0x00000000C0DE0005,
}


def run(cmd: list[str]) -> bytes:
    return subprocess.run(cmd, check=True, capture_output=True).stdout


def assemble() -> tuple[bytes, str]:
    OUT.mkdir(parents=True, exist_ok=True)
    obj = OUT / "trampoline.o"
    binary = OUT / "trampoline.bin"
    exe = lambda n: str(TOOLBIN / (n + ".exe"))  # noqa: E731
    run([exe("aarch64-none-elf-as"), "-o", str(obj), str(ASM)])
    run([exe("aarch64-none-elf-objcopy"), "-O", "binary", "-j", ".text",
         str(obj), str(binary)])
    code = binary.read_bytes()
    disasm = run([exe("aarch64-none-elf-objdump"), "-d", str(obj)]).decode("utf-8")
    return code, disasm


def parse_disasm(disasm: str) -> list[tuple[int, int, str, str]]:
    out = []
    pat = re.compile(r"^\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$")
    for line in disasm.splitlines():
        m = pat.match(line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16),
                        m.group(3), m.group(4).strip()))
    return out


def word(code: bytes, off: int) -> int:
    return int.from_bytes(code[off:off + 4], "little")


def qword(code: bytes, off: int) -> int:
    return int.from_bytes(code[off:off + 8], "little")


# ---------------------------------------------------------------------------
# The guest ELF is the ground truth. Nothing below trusts the brief.
# ---------------------------------------------------------------------------

def guest_window() -> list[int]:
    from elftools.elf.elffile import ELFFile
    so = REPO / "reverse" / "native" / "arm64" / "libRealRacing3.so"
    with open(so, "rb") as fh:
        elf = ELFFile(fh)
        seg = next(s for s in elf.iter_segments()
                   if s["p_type"] == "PT_LOAD" and s["p_flags"] & 1)
        fh.seek(seg["p_offset"] + (GUEST_SITE_VA - seg["p_vaddr"]))
        raw = fh.read(GUEST_WINDOW_BYTES)
    return [int.from_bytes(raw[i:i + 4], "little")
            for i in range(0, len(raw), 4)]


def check_guest() -> list[int]:
    window = guest_window()
    if window[0] != 0xF85B83B4:
        sys.exit("FAIL: guest 0x%x holds 0x%08x, expected 0x%08x"
                 % (GUEST_SITE_VA, window[0], 0xF85B83B4))
    if window[1] != 0xB4000354:
        sys.exit("FAIL: guest 0x%x word 1 is 0x%08x, expected 0xb4000354 (cbz)"
                 % (GUEST_SITE_VA + 4, window[1]))
    print("guest 0x%x..0x%x window:" % (GUEST_SITE_VA, GUEST_SITE_VA + 16))
    for i, w in enumerate(window):
        print("    +%2d  0x%08x" % (i * 4, w))
    return window


def check_header(window: list[int]) -> None:
    """The header's view of the site must match the bytes actually in the ELF."""
    text = HPP.read_text(encoding="utf-8")

    def u64(name: str) -> int:
        m = re.search(re.escape(name) + r"\s*=\s*0x([0-9a-fA-F]+)ull", text)
        if not m:
            sys.exit("FAIL: %s not found in %s" % (name, HPP.name))
        return int(m.group(1), 16)

    def u32(name: str) -> int:
        m = re.search(re.escape(name) + r"\s*=\s*0x([0-9a-fA-F]+)u\b", text)
        if not m:
            sys.exit("FAIL: %s not found in %s" % (name, HPP.name))
        return int(m.group(1), 16)

    if u64("kDeInfoSiteElfVa") != GUEST_SITE_VA:
        sys.exit("FAIL: kDeInfoSiteElfVa disagrees with the disassembled site")
    if u32("kDeInfoOriginalWord") != window[0]:
        sys.exit("FAIL: kDeInfoOriginalWord != guest word 0")
    if u32("kDeInfoWindowBytes") != GUEST_WINDOW_BYTES:
        sys.exit("FAIL: kDeInfoWindowBytes != %d" % GUEST_WINDOW_BYTES)

    declared = re.search(r"kDeInfoEatenWordsData\[[^\]]*\]\s*=\s*\{(.*?)\}", text, re.S)
    if not declared:
        sys.exit("FAIL: kDeInfoEatenWords[] not found in %s" % HPP.name)
    eaten = [int(x, 16) for x in
             re.findall(r"0x([0-9a-fA-F]{8})u", declared.group(1))]
    if eaten != window:
        sys.exit("FAIL: kDeInfoEatenWords %s != guest window %s"
                 % ([hex(w) for w in eaten], [hex(w) for w in window]))
    if u64("kDeInfoCbzTargetElfVa") != GUEST_CBZ_TARGET_VA:
        sys.exit("FAIL: kDeInfoCbzTargetElfVa != 0x%x" % GUEST_CBZ_TARGET_VA)
    if u64("kDeInfoResumeElfVa") != GUEST_RESUME_VA:
        sys.exit("FAIL: kDeInfoResumeElfVa != 0x%x" % GUEST_RESUME_VA)
    if u32("kDeInfoTrampolineSize") != CODE_BYTES + POOL_BYTES:
        sys.exit("FAIL: kDeInfoTrampolineSize != %d" % (CODE_BYTES + POOL_BYTES))
    print("header constants agree with the guest ELF  OK")


def check(code: bytes, insns: list) -> None:
    code_insns = [(o, w, m, p) for (o, w, m, p) in insns if o < POOL_OFFSET]
    print("trampoline: %d instructions / %d code bytes, + %d byte literal pool"
          % (len(code_insns), CODE_BYTES, POOL_BYTES))
    if len(code) != CODE_BYTES + POOL_BYTES:
        sys.exit("FAIL: image is %d bytes, expected %d"
                 % (len(code), CODE_BYTES + POOL_BYTES))
    if len(code_insns) * 4 != CODE_BYTES:
        sys.exit("FAIL: %d instructions do not fill %d code bytes"
                 % (len(code_insns), CODE_BYTES))
    if POOL_OFFSET % 8:
        sys.exit("FAIL: literal pool is not 8-byte aligned")

    # 1. Zero distance-dependent relocs may survive. This is the whole point of
    #    the rewrite: jitCreate() lands 12.4 GB from the site, so an ADRP here
    #    would make install() fail closed again, silently and for a second
    #    reason.
    for off, _, mn, ops in insns:
        if mn in ("adrp", "adr", "b", "bl", "blr", "ret") or \
           mn.startswith(("b.", "cb", "tb")):
            if mn == "adr":
                continue  # blob-relative, +/-1 MiB is always satisfied here
            sys.exit("FAIL: distance-dependent '%s %s' at 0x%x" % (mn, ops, off))
    print("no adrp / no b / no bl / no ret / no conditional branch  OK")

    # 2. Exactly two control transfers out, both `br`, at the known offsets.
    brs = sorted(off for off, _, mn, _ in insns if mn == "br")
    if brs != sorted(BRANCH_OUT_OFFSETS):
        sys.exit("FAIL: `br` at %s, expected %s"
                 % ([hex(b) for b in brs],
                    [hex(b) for b in sorted(BRANCH_OUT_OFFSETS)]))
    print("control flow: br @0x11c (cbz target) and br @0x128 (resume), no others  OK")

    # 3. The three position-independent replays must be byte-identical to the
    #    guest words they stand in for.
    window = guest_window()
    for off, want in REPLAY_OFFSETS.items():
        got = word(code, off)
        if got != want:
            sys.exit("FAIL: replay @0x%x is 0x%08x, expected 0x%08x"
                     % (off, got, want))
    if (word(code, 0xF8), word(code, 0x120), word(code, 0x124)) != \
            (window[0], window[2], window[3]):
        sys.exit("FAIL: replays do not match guest window words 0/2/3")
    print("replays @0x%03x/0x%03x/0x%03x = 0x%08x/0x%08x/0x%08x "
          "== guest +0/+8/+12  OK"
          % (0xF8, 0x120, 0x124, word(code, 0xF8), word(code, 0x120),
             word(code, 0x124)))

    # 4. The five `ldr <x>, <literal>` instructions must still be exactly these
    #    encodings, so install() can assert them before writing a constant.
    for off, want in LDR_LITERAL_WORDS.items():
        got = word(code, off)
        if got != want:
            sys.exit("FAIL: ldr-literal @0x%x is 0x%08x, expected 0x%08x"
                     % (off, got, want))
    print("ldr-literal sites at %s  OK"
          % [hex(o) for o in sorted(LDR_LITERAL_WORDS)])

    # 5. The literal placeholders install() overwrites must be intact.
    for name, off in POOL_SLOTS.items():
        got = qword(code, off)
        if got != POOL_PLACEHOLDERS[name]:
            sys.exit("FAIL: %s @0x%x is 0x%016x, expected 0x%016x"
                     % (name, off, got, POOL_PLACEHOLDERS[name]))
    print("literal slots %s carry their placeholders  OK"
          % list(POOL_SLOTS))

    # 6. The save/restore sets and NZCV, same invariant as before the rewrite.
    split = next(off for off, _, mn, ops in insns
                 if mn == "ldur" and "[x29, #-72]" in ops)
    saved, restored, flags_saved, flags_restored, v_saved, v_restored = (
        set(), set(), [], [], set(), set())
    for off, raw, mn, ops in insns:
        pre = off < split
        if pre and mn == "stp":
            for r in re.findall(r"\b(x\d+|q\d+)\b", ops.split("[")[0]):
                (saved if r.startswith("x") else v_saved).add(int(r[1:]))
        if pre and mn == "str" and ops.startswith("x30"):
            saved.add(30)
        if pre and mn == "mrs" and "nzcv" in ops:
            flags_saved.append(off)
        if not pre and mn == "ldp":
            for r in re.findall(r"\b(x\d+|q\d+)\b", ops.split("[")[0]):
                (restored if r.startswith("x") else v_restored).add(int(r[1:]))
        if not pre and mn == "ldr" and ops.startswith("x30"):
            restored.add(30)
        if not pre and mn == "msr" and "nzcv" in ops:
            flags_restored.append(off)
    missing = sorted(set(range(31)) - saved)
    if missing:
        sys.exit("FAIL: GPRs not saved: %s" % missing)
    if restored != saved:
        sys.exit("FAIL: save/restore sets differ: %s" % sorted(saved ^ restored))
    if not flags_saved or not flags_restored:
        sys.exit("FAIL: NZCV not both saved and restored")
    if v_saved != set(range(16)) or v_restored != set(range(16)):
        sys.exit("FAIL: v0-v15 not fully saved and restored")
    print("saved+restored: x0-x30 (%d), v0-v15 full width (%d), NZCV  OK"
          % (len(saved), len(v_saved)))

    # 7. The slot loads must come before the pair store, and the pair store
    #    before the release store, or the reader can observe a torn pair.
    def find(pred):
        return next(off for off, _, mn, ops in insns if pred(mn, ops))
    load_b = find(lambda m, o: m == "ldur" and "[x29, #-72]" in o)
    load_e = find(lambda m, o: m == "ldur" and "[x29, #-64]" in o)
    str_b = find(lambda m, o: m == "str" and o.startswith("x9,"))
    str_e = find(lambda m, o: m == "str" and o.startswith("x10,"))
    stlr = find(lambda m, o: m == "stlr")
    if not (load_b < str_b and load_e < str_e and str_b < stlr and str_e < stlr):
        sys.exit("FAIL: store ordering is wrong (loads %x/%x, stores %x/%x, stlr %x)"
                 % (load_b, load_e, str_b, str_e, stlr))
    print("ordering: loads %x/%x -> stores %x/%x -> stlr %x  OK"
          % (load_b, load_e, str_b, str_e, stlr))


def emit(code: bytes) -> None:
    lines = []
    for i in range(0, len(code), 12):
        chunk = code[i:i + 12]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))
    print("\n".join(lines))


def check_frozen(code: bytes) -> None:
    """The byte array in the .cpp must be exactly what the assembler emitted.

    Without this the array could be hand-edited and nothing would notice until
    the guest jumped into whatever the edited bytes turned out to be.
    """
    if not CPP.is_file():
        print("frozen array: %s not present, skipping" % CPP.name)
        return
    text = CPP.read_text(encoding="utf-8")
    m = re.search(r"kTrampolineCode\[kDeInfoTrampolineSize\] = \{(.*?)\};", text, re.S)
    if m is None:
        sys.exit("FAIL: no kTrampolineCode array found in %s" % CPP.name)
    frozen = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))
    if frozen != code:
        for i, (a, b) in enumerate(zip(code, frozen)):
            if a != b:
                sys.exit("FAIL: frozen array differs at byte 0x%x: "
                         "assembler 0x%02x, frozen 0x%02x" % (i, a, b))
        sys.exit("FAIL: frozen array is %d bytes, assembler emitted %d"
                 % (len(frozen), len(code)))
    print("frozen array in %s is byte-identical to the assembler output  OK"
          % CPP.name)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    args = ap.parse_args()

    window = check_guest()
    check_header(window)
    code, disasm = assemble()
    insns = parse_disasm(disasm)
    check(code, insns)
    check_frozen(code)
    if args.emit:
        emit(code)
    else:
        print()
        print(disasm)


if __name__ == "__main__":
    main()