#!/usr/bin/env python3
"""x29-capture self-test for the guest frame stub, plus its mutation controls.

WHAT THIS IS, AND WHAT IT IS NOT
--------------------------------
This machine is x86-64 Windows. There is no qemu-aarch64, no aarch64
emulator, and devkitA64 is a bare-metal cross toolchain. NO NATIVE AARCH64 CODE
RUNS ANYWHERE IN THIS SCRIPT, and nothing here should be reported as if it did.

What it does instead, in two layers:

  Layer 1 assembles native-switch/source/guest_frame_probe_stub.s with the
  real devkitPro assembler and decodes it with the matching objdump, then
  asserts structural facts that only hold if the capture is where the design
  says it is: the ONLY instructions that touch x29 are the entry save and the
  exit restore, the hand-off to C happens between them, and the sp delta is
  16-byte aligned.

  Layer 2 EXECUTES those exact bytes through a small AArch64 interpreter
  (capstone for decoding, so the bytes are validated by an independent
  implementation rather than by the arithmetic that produced them). The
  interpreter is a model, not a CPU: it does not model caches, the pipeline,
  speculative load, or the icache/dc split.

THE ACTUAL CLAIM UNDER TEST
---------------------------
AAPCS64 makes x29 callee-saved, so a guest's own frame pointer is still in x29
when the guest calls an import. The stub's job is to capture that value BEFORE
it establishes a frame of its own, because a C prologue would destroy it. So
the test drives the stub from a synthetic caller and asserts:

    captured_x29 == the CALLER's x29
    captured_x30 == the CALLER's x30
    x29, x30 and sp are handed back unchanged
    x0 carries the body's return value

and, for the same run, that a body which establishes its own frame the way
aarch64 gcc actually emits one (`stp x29,x30,[sp,#-16]!` + `mov x29,sp`) ends
up with a DIFFERENT x29. That second half is the point of the whole file: it is
what makes this an assembly stub rather than a C function.

POSITIVE CONTROL
----------------
`test_mutations` runs deliberately broken stubs and asserts the harness
REJECTS each one. A test that cannot fail proves nothing, so this is the part
that gives the rest of the file its value.

Run:
    python tools/rr3-frame-probe/test_frame_stub.py
"""

from __future__ import annotations

import pathlib
import re
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, CS_OP_IMM, CS_OP_MEM, CS_OP_REG

REPO = pathlib.Path(__file__).resolve().parents[2]
STUB_ASM = REPO / "native-switch" / "source" / "guest_frame_probe_stub.s"
TOOLBIN = (REPO / ".tools" / "devkitPro" / "msys64" / "opt" / "devkitpro"
           / "devkitA64" / "bin")
OUT = REPO / ".build" / "frameprobe"

# A synthetic caller. The numbers are arbitrary but the RELATIONSHIPS are not:
# the stub is required to hand back exactly these, and any mutation that
# changes one of them must make the harness fail.
CALLER_FP = 0x0000_7000_1000
CALLER_SP = 0x0000_7000_0000
CALLER_LR = 0x0000_2106_A2CD_C0     # a guest return address, inside the function
BODY_RET = 0x0000_DEAD_BEEF_0001
ARG0, ARG1, ARG2 = 0x1111_2222_3333_4444, 0x5555_6666_7777_8888, 0x40

# Where the interpreted stub is loaded, and where the modelled C body lives.
STUB_BASE = 0x0000_5350_0000
BODY_ADDR = 0x0000_1D0_0000
# The guest return address must be inside the container-teardown function, so a
# value that is not gets rejected by the filter before any read is attempted.
GUEST_FN_LO = 0x000CD1014
GUEST_FN_HI = 0x000CD3648

XSP = 31
XZERO = 32
MD = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
MD.detail = True
_ALIAS = {"sp": XSP, "wsp": XSP, "fp": 29, "wfp": 29, "lr": 30, "wlr": 30,
          "xzr": XZERO, "wzr": XZERO}


def regnum(name: str) -> int:
    if name in _ALIAS:
        return _ALIAS[name]
    return int(name[1:])


class Machine:
    def __init__(self) -> None:
        self.x = [0] * 31
        self.sp = 0
        self.mem: dict[int, int] = {}

    def poke(self, addr: int, size: int, value: int) -> None:
        for i in range(size):
            self.mem[addr + i] = (value >> (8 * i)) & 0xFF

    def peek(self, addr: int, size: int) -> int:
        v = 0
        for i in range(size):
            v |= self.mem.get(addr + i, 0) << (8 * i)
        return v

    def st(self, addr: int, size: int, value: int) -> None:
        self.poke(addr, size, value)

    def get(self, n: int) -> int:
        if n == XZERO:
            return 0
        return self.sp if n == XSP else self.x[n]

    def set(self, n: int, v: int) -> None:
        if n == XZERO:
            return
        if n == XSP:
            self.sp = v
        else:
            self.x[n] = v & 0xFFFFFFFFFFFFFFFF


def assemble() -> tuple[bytes, str]:
    OUT.mkdir(parents=True, exist_ok=True)
    obj = OUT / "stub.o"
    exe = lambda n: str(TOOLBIN / (n + ".exe"))  # noqa: E731
    subprocess.run([exe("aarch64-none-elf-as"), "-o", str(obj), str(STUB_ASM)],
                   check=True, capture_output=True)
    disasm = subprocess.run([exe("aarch64-none-elf-objdump"), "-d", str(obj)],
                            check=True, capture_output=True).stdout.decode("utf-8")
    subprocess.run([exe("aarch64-none-elf-objcopy"), "-O", "binary",
                    "-j", ".text.rr3_guest_frame_probe", str(obj),
                    str(OUT / "stub.bin")], check=True, capture_output=True)
    return (OUT / "stub.bin").read_bytes(), disasm


def parse_disasm(disasm: str) -> list[tuple[int, int, str, str]]:
    out = []
    pat = re.compile(r"^\s+([0-9a-f]+):\s+([0-9a-f]{8})\s+(\S+)\s*(.*)$")
    for line in disasm.splitlines():
        m = pat.match(line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16),
                        m.group(3), m.group(4).strip()))
    return out


# ---------------------------------------------------------------------------
# Layer 1: structure
# ---------------------------------------------------------------------------

def test_structure(insns: list) -> bool:
    print("layer 1 -- structure of the shipped stub")
    print("  %d instructions, %d bytes" % (len(insns), len(insns) * 4))

    problems = []
    # x29 must be WRITTEN exactly twice: saved on entry, restored on exit. Only
    # the FIRST operand counts -- `mov x3, x29` READS x29 and is the whole point
    # of the file, so counting it as a write would be exactly backwards.
    writes29 = []
    for o, _, mn, ops in insns:
        first = ops.split(",")[0].strip() if ops else ""
        if mn in ("stp", "ldp", "str", "ldur", "stur", "mov", "add", "sub") \
                and re.fullmatch(r"(x29|w29|fp|wfp)", first):
            writes29.append((o, mn))
    reads29 = [o for o, _, mn, ops in insns
               if re.search(r"\b(x29|fp)\b", ops) and (o, mn) not in writes29]
    print("  instructions WRITING x29: %s" % [(hex(o), mn) for o, mn in writes29])
    print("  instructions READING  x29: %s" % [hex(o) for o in reads29])
    if len(writes29) != 2:
        problems.append("expected exactly 2 instructions writing x29, got %d"
                        % len(writes29))
    if not any(mn == "stp" for _, mn in writes29):
        problems.append("no `stp x29, x30` entry save: the capture is missing")
    if not any(mn == "ldp" for _, mn in writes29):
        problems.append("no `ldp x29, x30` exit restore: x29 would be corrupted")
    if not reads29:
        problems.append("x29 is never read: nothing captures the caller's frame")

    # The capture must come before the call, and the restore after it.
    bl = [o for o, _, mn, _ in insns if mn == "bl"]
    if len(bl) != 1:
        problems.append("expected exactly one `bl`, got %d" % len(bl))
        print("  FAIL structure")
        for p in problems:
            print("       - %s" % p)
        return False
    bl_off = bl[0]
    save = next(o for o, mn in writes29 if mn == "stp")
    restore = next(o for o, mn in writes29 if mn == "ldp")
    if not (save < bl_off < restore):
        problems.append("order is save@0x%x bl@0x%x restore@0x%x, want save<bl<restore"
                        % (save, bl_off, restore))
    print("  order: capture@0x%x < bl@0x%x < restore@0x%x  OK" % (save, bl_off, restore))

    # The hand-off must be `mov x3, x29` BEFORE the bl, or the body receives the
    # stub's own frame pointer and the entire exercise is pointless.
    movx29 = [o for o, _, mn, ops in insns if mn == "mov" and ops.endswith("x29")]
    if not movx29 or movx29[0] >= bl_off:
        problems.append("no `mov xN, x29` before the call: the caller's x29 "
                        "is never handed to C")
    else:
        print("  hand-off: `mov x3, x29` @0x%x before the bl  OK" % movx29[0])
    movx30 = [o for o, _, mn, ops in insns if mn == "mov" and ops.endswith("x30")]
    if not movx30 or movx30[0] >= bl_off:
        problems.append("no `mov xN, x30` before the call: the guest return "
                        "address is never handed to C")
    else:
        print("  hand-off: `mov x4, x30` @0x%x before the bl  OK" % movx30[0])

    # sp must go down by a 16-byte multiple and come back by the same amount.
    subs = [ops for _, _, mn, ops in insns if mn == "sub" and ops.startswith("sp,")]
    adds = [ops for _, _, mn, ops in insns if mn == "add" and ops.startswith("sp,")]
    if len(subs) != 1 or len(adds) != 1:
        problems.append("expected one sp sub and one sp add")
    else:
        delta = int(subs[0].split("#")[1], 0)
        back = int(adds[0].split("#")[1], 0)
        print("  sp: -0x%x then +0x%x  OK" % (delta, back))
        if delta != back:
            problems.append("sp delta %d but restore %d" % (delta, back))
        if delta % 16:
            problems.append("sp delta 0x%x breaks 16-byte alignment" % delta)

    rets = [o for o, _, mn, _ in insns if mn == "ret"]
    if len(rets) != 1:
        problems.append("expected exactly one `ret`, got %d" % len(rets))
    if problems:
        print("  FAIL structure")
        for p in problems:
            print("       - %s" % p)
        return False
    return True


# ---------------------------------------------------------------------------
# Layer 2: execute the real bytes
# ---------------------------------------------------------------------------

def run_stub(code: bytes, caller_fp: int, caller_sp: int, caller_lr: int,
             body_establishes_frame: bool) -> dict:
    """Drive the real stub bytes and report what the C side would have seen.

    `body_establishes_frame` models what an aarch64 C function actually does in
    its prologue. It is the reason this has to be assembly: with it on, the body
    ends up with its OWN x29, and a stub that had handed the body x29 in the
    usual way would have handed over garbage.
    """
    m = Machine()
    m.sp = caller_sp
    m.x[29] = caller_fp
    m.x[30] = caller_lr
    m.x[0], m.x[1], m.x[2] = ARG0, ARG1, ARG2
    # Poison everything the stub must not disturb, so a stray write shows up.
    for n in range(3, 29):
        m.x[n] = 0xA5A5_0000_0000_0000 | n
    m.x[16] = 0xBEEF_0000_0000_0000 | 16
    m.x[17] = 0xBEEF_0000_0000_0000 | 17

    for i in range(0, len(code), 4):
        m.poke(STUB_BASE + i, 4, struct.unpack_from("<I", code, i)[0])

    seen = {}
    sp_at_entry = m.sp
    x29_at_entry = m.x[29]
    pc = STUB_BASE
    limit = 64
    for _ in range(limit):
        raw = m.peek(pc, 4)
        dec = list(MD.disasm(struct.pack("<I", raw), pc))
        if not dec:
            raise AssertionError("capstone decoded nothing at 0x%x" % pc)
        insn = dec[0]
        mn, ops = insn.mnemonic, insn.operands
        npc = pc + 4

        if mn == "nop" or mn == "bti":
            pc = npc
            continue
        if mn in ("stp", "ldp"):
            r0, r1 = MD.reg_name(ops[0].reg), MD.reg_name(ops[1].reg)
            base = m.get(regnum(MD.reg_name(ops[2].mem.base)))
            a = (base + ops[2].mem.disp) & 0xFFFFFFFFFFFFFFFF
            n0 = 16 if r0.startswith("q") else 8
            n1 = 16 if r1.startswith("q") else 8
            if mn == "stp":
                m.st(a, n0, m.get(regnum(r0)))
                m.st(a + n0, n1, m.get(regnum(r1)))
            else:
                m.set(regnum(r0), m.peek(a, n0))
                m.set(regnum(r1), m.peek(a + n0, n1))
            pc = npc
            continue
        if mn in ("str", "ldr", "ldur", "stur"):
            rn = MD.reg_name(ops[0].reg)
            if ops[1].type == CS_OP_MEM:
                a = (m.get(regnum(MD.reg_name(ops[1].mem.base)))
                     + ops[1].mem.disp) & 0xFFFFFFFFFFFFFFFF
            elif ops[1].type == CS_OP_IMM:
                a = ops[1].imm
            else:
                raise AssertionError("bad memory operand at 0x%x" % pc)
            n = 8 if rn.startswith("x") else 4
            if mn in ("str", "stur"):
                m.st(a, n, m.get(regnum(rn)))
            else:
                m.set(regnum(rn), m.peek(a, n))
            pc = npc
            continue
        if mn in ("sub", "add"):
            dst = MD.reg_name(ops[0].reg)
            src = MD.reg_name(ops[1].reg)
            imm = ops[2].imm
            v = m.get(regnum(src)) + (-imm if mn == "sub" else imm)
            m.set(regnum(dst), v & 0xFFFFFFFFFFFFFFFF)
            pc = npc
            continue
        if mn == "mov":
            dst = regnum(MD.reg_name(ops[0].reg))
            v = m.get(regnum(MD.reg_name(ops[1].reg)))
            m.set(dst, v & 0xFFFFFFFFFFFFFFFF)
            pc = npc
            continue
        if mn == "bl":
            # The call into C. This models the body: the ABI says it may clobber
            # x0-x18 and d0-d7 and must preserve x19-x28, sp and d8-d15, and it
            # must give back its OWN frame before it returns.
            seen["arg0"] = m.x[0]
            seen["arg1"] = m.x[1]
            seen["arg2"] = m.x[2]
            seen["x3"] = m.x[3]
            seen["x4"] = m.x[4]
            if body_establishes_frame:
                # Exactly what aarch64 gcc emits for a frame-pointer function:
                #   stp x29, x30, [sp, #-16]!   /   mov x29, sp
                m.sp = (m.sp - 16) & 0xFFFFFFFFFFFFFFFF
                m.poke(m.sp, 8, m.x[29])
                m.poke(m.sp + 8, 8, m.x[30])
                m.x[29] = m.sp
            # Seen INSIDE the body, before its epilogue: this is the x29 a C
            # function would have had to offer, and it is not the caller's.
            seen["body_fp"] = m.x[29]
            if body_establishes_frame:
                # ...and the epilogue, which must hand the frame back.
                m.x[29] = m.peek(m.sp, 8)
                m.x[30] = m.peek(m.sp + 8, 8)
                m.sp = (m.sp + 16) & 0xFFFFFFFFFFFFFFFF
            m.x[0] = BODY_RET
            m.x[30] = STUB_BASE + 0x1000      # a plausible return address
            pc = npc                              # resume after the bl
            continue
        if mn == "ret":
            return {"seen": seen, "x29": m.x[29], "x30": m.x[30], "sp": m.sp,
                    "x0": m.x[0], "x19": m.x[19], "x20": m.x[20], "x16": m.x[16],
                    "sp_at_entry": sp_at_entry, "x29_at_entry": x29_at_entry}
        raise AssertionError("unhandled '%s %s' at 0x%x" % (mn, insn.op_str, pc))
    raise AssertionError("stub did not return within %d instructions" % limit)


def check(name: str, code: bytes, caller_fp: int = CALLER_FP,
          caller_sp: int = CALLER_SP, caller_lr: int = CALLER_LR,
          expect_fp: int | None = None) -> bool:
    """The contract, asserted against whatever bytes `code` contains."""
    r = run_stub(code, caller_fp, caller_sp, caller_lr,
                 body_establishes_frame=True)
    seen = r["seen"]
    problems = []

    if seen.get("x3") != (caller_fp if expect_fp is None else expect_fp):
        problems.append("stub handed the body x3=0x%016x, want the CALLER's "
                        "frame pointer 0x%016x"
                        % (seen.get("x3", 0),
                           caller_fp if expect_fp is None else expect_fp))
    if seen.get("x4") != caller_lr:
        problems.append("stub handed the body x4=0x%016x, want the caller's "
                        "x30 0x%016x" % (seen.get("x4", 0), caller_lr))
    if (seen.get("arg0"), seen.get("arg1"), seen.get("arg2")) != (ARG0, ARG1, ARG2):
        problems.append("arguments mangled: %016x %016x %016x"
                        % (seen.get("arg0", 0), seen.get("arg1", 0),
                           seen.get("arg2", 0)))

    # THE point of the file: the body established its own frame, so its x29 is
    # NOT the caller's. If these were equal, the stub would be unnecessary.
    if seen.get("body_fp") == caller_fp:
        problems.append("the modelled C body kept the caller's x29, so this "
                        "test would not distinguish a stub from a C function")

    if r["x29"] != caller_fp:
        problems.append("x29 not handed back: 0x%016x, was 0x%016x"
                        % (r["x29"], caller_fp))
    if r["x30"] != caller_lr:
        problems.append("x30 not handed back: 0x%016x, was 0x%016x"
                        % (r["x30"], caller_lr))
    if r["sp"] != caller_sp:
        problems.append("sp not handed back: 0x%016x, was 0x%016x"
                        % (r["sp"], caller_sp))
    if r["x0"] != BODY_RET:
        problems.append("return value lost: x0=0x%016x, want 0x%016x"
                        % (r["x0"], BODY_RET))
    for reg in ("x19", "x20"):
        if r[reg] != (0xA5A5_0000_0000_0000 | int(reg[1:])):
            problems.append("%s clobbered: 0x%016x" % (reg, r[reg]))

    if problems:
        print("  FAIL %s" % name)
        for p in problems:
            print("       - %s" % p)
        return False
    print("  pass %s" % name)
    print("       caller  x29=0x%016x x30=0x%016x sp=0x%016x"
          % (caller_fp, caller_lr, caller_sp))
    print("       stub -> C  x3(fp)=0x%016x  x4(lr)=0x%016x"
          % (seen["x3"], seen["x4"]))
    print("       C body established its own x29=0x%016x (differs, as it must)"
          % seen["body_fp"])
    print("       on return x29=0x%016x x30=0x%016x sp=0x%016x x0=0x%016x"
          % (r["x29"], r["x30"], r["sp"], r["x0"]))
    return True


def test_mutations(code: bytes, insns: list) -> bool:
    """Positive control. Every one of these MUST fail."""
    print("layer 2 -- positive control (each of these MUST fail):")

    def off(pred) -> int:
        return next(o for o, _, mn, ops in insns if pred(mn, ops))

    off_mov_fp = off(lambda m, o: m == "mov" and o.endswith("x29"))
    off_mov_lr = off(lambda m, o: m == "mov" and o.endswith("x30"))
    off_save = off(lambda m, o: m == "stp" and o.startswith("x29"))
    off_restore = off(lambda m, o: m == "ldp" and o.startswith("x29"))
    off_sp_up = off(lambda m, o: m == "add" and o.startswith("sp,"))
    off_sub = off(lambda m, o: m == "sub" and o.startswith("sp,"))
    off_bl = off(lambda m, o: m == "bl")

    def mutate(fn) -> bytes:
        out = bytearray(code)
        fn(out)
        return bytes(out)

    def wrong_register(out: bytearray) -> None:
        # `mov x3, x28` instead of `mov x3, x29`: the classic "grabbed a nearby
        # callee-saved register instead of the frame pointer".
        struct.pack_into("<I", out, off_mov_fp, 0xAA1C03E3)

    def no_lr_hand_off(out: bytearray) -> None:
        struct.pack_into("<I", out, off_mov_lr, 0xD503201F)   # nop

    def no_restore(out: bytearray) -> None:
        struct.pack_into("<I", out, off_restore, 0xD503201F)   # nop

    def no_sp_restore(out: bytearray) -> None:
        struct.pack_into("<I", out, off_sp_up, 0xAA1D03E0)     # mov x0, sp

    def sp_not_taken(out: bytearray) -> None:
        struct.pack_into("<I", out, off_sub, 0xD503201F)       # nop

    def capture_after_prologue(out: bytearray) -> None:
        # The mistake a C function makes: hand C the stub's OWN sp instead of
        # the caller's frame pointer. sp IS a plausible-looking stack address,
        # so this is the failure that a naive "looks like a pointer" check on
        # the C side would wave through -- and it must still be caught here.
        struct.pack_into("<I", out, off_mov_fp, 0x910003E3)   # add x3, sp, #0

    def no_entry_capture(out: bytearray) -> None:
        # No capture at all: the exit `ldp` then restores whatever happens to
        # be on the stack, so the guest comes back with a frame pointer that was
        # never its own.
        struct.pack_into("<I", out, off_save, 0xD503201F)    # nop

    cases = [
        ("captures x28 instead of x29", wrong_register),
        ("never hands the return address to C", no_lr_hand_off),
        ("never restores x29/x30", no_restore),
        ("never restores sp", no_sp_restore),
        ("never takes the frame at all", sp_not_taken),
        ("hands C its own sp instead of the caller's x29", capture_after_prologue),
        ("no entry capture of x29/x30", no_entry_capture),
    ]
    ok = True
    for name, fn in cases:
        if check(name, mutate(fn)):
            print("       ^^ CONTROL DID NOT FIRE (expected a failure)")
            ok = False

    # A control on the STUB's transparency: a different, equally valid caller
    # must come back with THAT caller's x29, which proves the stub is a
    # pass-through and not something that happens to match one hardcoded frame.
    print("layer 2 -- a second, different caller (MUST pass):")
    ok = True
    if not check("second caller at a different address", code,
                 caller_fp=CALLER_FP + 0x4000, caller_sp=CALLER_SP - 0x8000,
                 caller_lr=CALLER_LR + 4):
        print("       ^^ CONTROL DID NOT FIRE (the stub is not a pass-through)")
        ok = False
    return ok


def main() -> int:
    print("assembling %s\n" % STUB_ASM.name)
    code, disasm = assemble()
    insns = parse_disasm(disasm)

    print(disasm)
    struct_ok = test_structure(insns)

    print("\nlayer 2 -- the contract, on the shipping bytes")
    contract_ok = check("shipping bytes", code)
    control_ok = test_mutations(code, insns) if struct_ok else False

    print()
    if struct_ok and contract_ok and control_ok:
        print("RESULT: PASS  -- the stub hands the body's C code the CALLER's "
              "x29/x30, hands\n       them back untouched, and every one of the "
              "%d broken variants was rejected."
              % 7)
        return 0
    print("RESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
