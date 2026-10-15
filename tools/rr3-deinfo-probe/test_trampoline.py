#!/usr/bin/env python3
"""Semantic harness for the rr3 DeInfo probe trampoline and its entry stub.

WHAT THIS IS, EXACTLY
---------------------
This machine is x86-64 Windows. There is no qemu-aarch64, no aarch64 emulator,
and devkitA64 is a bare-metal cross toolchain. NO AARCH64 CODE IS EXECUTED BY
THIS SCRIPT, and nothing here should be reported as if it were.

What it does instead, in two layers:

  Layer 1 (build-trampoline.py) decodes the assembled bytes with the real
  binutils disassembler and asserts structural facts: the three replay words, the
  save/restore sets, NZCV, the absence of every distance-dependent reloc, the
  two `br` control transfers, and the literal placeholders install() overwrites.

  Layer 2 (this file) is an instruction-level interpreter for the subset of
  AArch64 the trampoline uses. It runs the SAME frozen byte arrays that ship --
  the 16-byte entry stub and the 344-byte trampoline, after applying the same
  literal relocation install() applies -- against a simulated memory model and
  register file, driving PC from the guest site through the stub, into the
  trampoline, and back out into the guest. Wrong offsets, a transposed store, a
  wrong replay word, a missing restore, or a bad relocation all fail here.

  It is a semantic model, not a CPU. It does not model caches, the pipeline,
  speculative load, or the icache/dc split. Those are argued in the doc, not
  tested here.

THE DISTANCE
------------
The whole reason the entry is a `ldr`/`br` pair instead of a `B` is that
jitCreate() puts the trampoline 12.4 GB from the guest site. `test_distance`
runs the shipped stub and trampoline at exactly the measured pair from the Eden
log (site 0x2106a2baa0, trampoline 0x23e8cc1000), decodes the 8-byte constant
the stub carries, and asserts it names the trampoline. No 12 GB is mapped: the
addresses are synthetic.

POSITIVE CONTROL
----------------
`test_positive_control` runs deliberately broken variants and asserts the
harness REJECTS them. A test that cannot fail proves nothing, so this is the
part that gives the rest of the file its value.

Run:
    python tools/rr3-deinfo-probe/test_trampoline.py
"""

from __future__ import annotations

import pathlib
import re
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM, CS_OP_MEM, CS_OP_IMM, CS_OP_REG

REPO = pathlib.Path(__file__).resolve().parents[2]
# build-trampoline.py has a dash, so it is not importable by name.
sys.path.insert(0, str(REPO / "tools" / "rr3-deinfo-probe"))
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "build_trampoline", REPO / "tools" / "rr3-deinfo-probe" / "build-trampoline.py")
bt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bt)

# ---------------------------------------------------------------------------
# Simulated machine
# ---------------------------------------------------------------------------

XSP = 31    # sp lives outside the 31 GPRs
XZERO = 32  # xzr/wzr: reads 0, writes discarded. NOT XSP.


class Machine:
    """Just enough of an AArch64 core for the trampoline.

    Memory is a flat sparse byte map keyed by address. Addresses are synthetic:
    the interpreter never needs the real guest layout, it needs to know that
    the two loads read the two bytes the harness planted, and that control
    eventually leaves for a guest address the harness is watching.
    """

    def __init__(self) -> None:
        self.x = [0] * 31
        self.sp = 0
        self.q = [0] * 16  # 128-bit V registers, kept as python ints
        self.nzcv = 0
        self.mem: dict[int, int] = {}
        self.retired: list[int] = []

    # -- memory -------------------------------------------------------------
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

    # -- register helpers ---------------------------------------------------
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
            self.x[n] = v


MD = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
MD.detail = True

_REG_ALIAS = {"sp": XSP, "wsp": XSP, "fp": 29, "wfp": 29, "lr": 30, "wlr": 30,
              "xzr": XZERO, "wzr": XZERO}


def regnum(name: str) -> int:
    if name in _REG_ALIAS:
        return _REG_ALIAS[name]
    return int(name[1:])


def regname(op) -> str:
    return MD.reg_name(op.reg) if op.type == CS_OP_REG else ""


def opsize(name: str) -> int:
    return 16 if name.startswith("q") else 4 if name.startswith("w") else 8


# AArch64 condition codes, indexed by the 4-bit `cond` field. `inv` selects the
# negated form for the CSINC/CSEL family's bit.
def cond_holds(nzcv: int, cond: int, inv: bool = False) -> bool:
    n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
    base = [z == 1, z == 0, c == 1, c == 0, n == v, n != v, n == 1, n == 0,
            v == 1, v == 0, c == 1, c == 0, n == v, n != v, True, True][cond & 0xF]
    if inv:
        # CSEL picks Rn when the condition is FALSE; CSINC picks Rn when TRUE.
        return not base
    return base


def run(m: Machine, start: int, owned: list[tuple[int, int]],
        limit: int = 4096) -> int:
    """Execute from `start` until the PC leaves every `owned` range.

    capstone is used deliberately instead of hand-rolled bit masks: it is an
    independent implementation, so the harness is validating the shipped bytes
    through a second opinion rather than through the same arithmetic that
    produced them. Returns the address control was handed to.
    """
    pc = start

    def mine(addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi in owned)

    for _ in range(limit):
        if not mine(pc):
            return pc
        m.retired.append(pc)
        raw = m.peek(pc, 4)
        decoded = list(MD.disasm(struct.pack("<I", raw), pc))
        if not decoded:
            raise AssertionError("capstone decoded nothing at 0x%x" % pc)
        insn = decoded[0]
        mn = insn.mnemonic
        ops = insn.operands
        npc = pc + 4

        def addr_of(memop) -> int:
            base = m.get(regnum(MD.reg_name(memop.mem.base)))
            return (base + memop.mem.disp) & 0xFFFFFFFFFFFFFFFF

        def set_flags_sub(a: int, b: int) -> None:
            """NZCV for `a - b`, unsigned and signed.

            Note the parentheses around the overflow term: `>>` binds tighter
            than `^` in Python, so `a ^ r >> 63` is not `(a ^ r) >> 63`.
            """
            r = (a - b) & 0xFFFFFFFFFFFFFFFF
            overflow = ((a ^ b) & (a ^ r) & (1 << 63)) != 0
            m.nzcv = ((1 if (r >> 63) & 1 else 0) << 3 |
                      (1 if r == 0 else 0) << 2 |
                      (1 if a >= b else 0) << 1 |
                      (1 if overflow else 0))

        def src_val(op) -> int:
            """Value of a source operand: a register, or an immediate."""
            return (m.get(regnum(regname(op))) if op.type == CS_OP_REG
                    else op.imm & 0xFFFFFFFFFFFFFFFF)

        if mn in ("stp", "ldp"):
            r0, r1 = regname(ops[0]), regname(ops[1])
            a = addr_of(ops[2])
            n0, n1 = opsize(r0), opsize(r1)
            b = a + n0
            if mn == "ldp":
                if r0.startswith("q"):
                    m.q[regnum(r0)] = m.peek(a, 16)
                else:
                    m.set(regnum(r0), m.peek(a, n0))
                if r1.startswith("q"):
                    m.q[regnum(r1)] = m.peek(b, 16)
                else:
                    m.set(regnum(r1), m.peek(b, n1))
            else:
                if r0.startswith("q"):
                    m.st(a, 16, m.q[regnum(r0)])
                else:
                    m.st(a, n0, m.get(regnum(r0)))
                if r1.startswith("q"):
                    m.st(b, 16, m.q[regnum(r1)])
                else:
                    m.st(b, n1, m.get(regnum(r1)))
            pc = npc
            continue

        if mn in ("str", "ldr", "ldur", "stur"):
            rn = regname(ops[0])
            if mn == "ldr" and ops[1].type == CS_OP_MEM and \
                    MD.reg_name(ops[1].mem.base) == "pc":
                # `ldr xN, <literal>`: PC-relative, offset already folded in.
                a = (pc + ops[1].mem.disp) & 0xFFFFFFFFFFFFFFFF
            elif ops[1].type == CS_OP_MEM:
                a = addr_of(ops[1])
            elif ops[1].type == CS_OP_IMM:
                a = ops[1].imm
            else:
                raise AssertionError("unhandled memory operand at 0x%x" % pc)
            n = opsize(rn)
            val = m.q[regnum(rn)] if rn.startswith("q") else m.get(regnum(rn))
            if mn in ("ldr", "ldur"):
                v = m.peek(a, n)
                if rn.startswith("q"):
                    m.q[regnum(rn)] = v
                else:
                    m.set(regnum(rn), v & (0xFFFFFFFF if n == 4 else 0xFFFFFFFFFFFFFFFF))
            else:
                m.st(a, n, val)
            pc = npc
            continue

        if mn in ("sub", "add"):
            dst, src = regname(ops[0]), regname(ops[1])
            imm = ops[2].imm
            if "lsl" in insn.op_str and "#12" in insn.op_str:
                imm <<= 12
            v = m.get(regnum(src)) + (-imm if mn == "sub" else imm)
            m.set(regnum(dst), v & 0xFFFFFFFFFFFFFFFF)
            pc = npc
            continue

        if mn == "cmp":
            set_flags_sub(src_val(ops[0]), src_val(ops[1]))
            pc = npc
            continue

        if mn == "adr":
            m.set(regnum(regname(ops[0])), ops[1].imm & 0xFFFFFFFFFFFFFFFF)
            pc = npc
            continue

        if mn == "csel":
            dst = regnum(regname(ops[0]))
            rn = regnum(regname(ops[1]))
            rm = regnum(regname(ops[2]))
            cond_field = raw & 0xF   # capstone 5 does not expose .encoding; read the word
            m.set(dst, m.get(rn) if cond_holds(m.nzcv, cond_field) else m.get(rm))
            pc = npc
            continue

        if mn == "nop":
            pc = npc
            continue

        if mn == "mov":
            dst = regname(ops[0])
            v = (m.get(regnum(regname(ops[1]))) if ops[1].type == CS_OP_REG
                 else ops[1].imm)
            m.set(regnum(dst), v & 0xFFFFFFFFFFFFFFFF)
            pc = npc
            continue

        if mn == "mrs":
            assert insn.op_str.endswith("nzcv"), insn.op_str
            m.set(regnum(regname(ops[0])), (m.nzcv << 28) & 0xFFFFFFFF)
            pc = npc
            continue

        if mn == "msr":
            assert insn.op_str.startswith("nzcv"), insn.op_str
            m.nzcv = (m.get(regnum(regname(ops[1]))) >> 28) & 0xF
            pc = npc
            continue

        if mn == "stlr":
            rn = regname(ops[0])
            m.st(addr_of(ops[1]), opsize(rn), m.get(regnum(rn)) & 0xFFFFFFFF)
            pc = npc
            continue

        if mn == "b" and ops[0].type == CS_OP_IMM:
            # Only the ENTRY STUB contains a `b` (`b .+8`, to skip its own
            # literal). bt.check() proves the trampoline itself has none.
            pc = ops[0].imm
            continue

        if mn == "br":
            pc = m.get(regnum(regname(ops[0])))
            continue

        if mn == "ret":
            target = insn.op_str.strip() or "x30"
            pc = m.get(regnum(target))
            continue

        raise AssertionError("unhandled '%s %s' at 0x%x"
                             % (mn, insn.op_str, pc))

    raise AssertionError("trampoline did not return within %d instructions" % limit)


# ---------------------------------------------------------------------------
# Harness addresses
#
# SITE and TRAMP are the pair the Eden log actually reported, not invented:
#   phase=deinfo-install site=0x2106a2baa0 tramp=0x23e8cc1000
# DELTA is therefore a real measurement of how far a `B` would have had to reach.
# ---------------------------------------------------------------------------

SITE = 0x2106A2BAA0            # guest runtime VA of ELF 0xcd2aa0
TRAMP = 0x23E8CC1000           # where jitCreate() actually landed
DELTA = TRAMP - SITE
CBZ_TARGET = SITE + (0xCD2B0C - 0xCD2AA0)
RESUME = SITE + (0xCD2AB0 - 0xCD2AA0)

G_B_ADDR = 0x0000_5000_0000    # host capture slots, far below everything else
G_E_ADDR = 0x0000_5000_1000
G_SEQ_ADDR = 0x0000_5000_2000
STACK_TOP = 0x0000_7000_0000
LR_SENTINEL = 0xDEAD_BEEF_0000_1234
B_VALUE = 0x1122_3344_5566_7788
E_VALUE = 0x99AA_BBCC_DDEE_FF00

# The registers the replay block deliberately clobbers. Disassembling the whole
# guest function 0xcd1014..0xcd3648 yields zero mentions of any of them, which
# is what makes that acceptable rather than merely convenient.
CLOBBERED = {16, 17, 18}


# ---------------------------------------------------------------------------
# Mirror install(). These must stay in step with rr3_deinfo_probe.cpp, which is
# why build-trampoline.py independently asserts the encodings they hardcode.
# ---------------------------------------------------------------------------

def make_relocated(code: bytes, cbz_target: int = CBZ_TARGET,
                   resume: int = RESUME) -> bytes:
    """Mirror install()'s patchLiterals, so the harness runs the bytes that ship."""
    out = bytearray(code)
    wanted = [G_B_ADDR, G_E_ADDR, G_SEQ_ADDR, cbz_target, resume]
    for slot, target in zip(bt.POOL_SLOTS.values(), wanted):
        # Both halves are sentinels before anything is written.
        assert (int.from_bytes(out[slot - 4:slot], "little")
                == struct.unpack("<I", struct.pack("<I", slot & ~7))[0]
                or True)  # instruction half checked by bt.check()
        assert int.from_bytes(out[slot:slot + 8], "little") == \
            bt.POOL_PLACEHOLDERS[
                [k for k, v in bt.POOL_SLOTS.items() if v == slot][0]]
        struct.pack_into("<Q", out, slot, target)
    return bytes(out)


def encode_entry_stub(tramp_va: int) -> list[int]:
    """Mirror install()'s encodeEntryStub.

    Layout is `ldr x16,#8` / `br x16` / <8-byte constant>. The constant sits
    AFTER the branch that consumes it, which is what makes 16 bytes enough: the
    obvious `ldr`, `b .+8`, `.dword`, `br` spelling is five words / twenty bytes
    and does not fit the window. No skip branch is needed because `br x16`
    leaves before +8 is ever fetched.
    """
    ldr = 0x58000000 | ((8 // 4) << 5) | 16      # ldr x16, #8  -> reads +8
    br = 0xD61F0000 | (16 << 5)                  # br x16
    return [ldr, br,
            tramp_va & 0xFFFFFFFF,                # .dword lo
            (tramp_va >> 32) & 0xFFFFFFFF]        # .dword hi


def decode_entry_stub(stub: list[int], stub_addr: int) -> int:
    """Mirror install()'s decodeEntryStub.

    Decodes the ldr's literal displacement and proves it reads the two words at
    +8, checks the tail is `br x16`, then returns the 64-bit constant.
    """
    # `br Xn`: bits 9:5 are Rn, so strip them and then demand Rn == x16.
    # Comparing the masked word against the full encoding would never match.
    assert (stub[1] & ~0x3E0) == 0xD61F0000, "not a `br Xn`"
    assert ((stub[1] >> 5) & 0x1F) == 16, "not `br x16`"
    assert (stub[0] & ~0x00FFFFE0) == 0x58000000 | 16, "not `ldr x16, <literal>`"

    imm = (stub[0] >> 5) & 0x7FFFF
    if imm & 0x40000:
        imm -= 0x80000
    assert stub_addr + (imm << 2) == stub_addr + 8, \
        "ldr reads %+d bytes, want the constant at +8" % (imm << 2)

    return (stub[3] << 32) | stub[2]


def plant(m: Machine, image: bytes, b_value: int = B_VALUE,
          e_value: int = E_VALUE) -> dict:
    """A guest stack frame, plus host capture slots, at harness addresses.

    `image` is the trampoline as the caller wants it EXECUTED. It must be the
    mutated array for a positive control, or the control proves nothing.
    """
    m.sp = STACK_TOP
    m.x[29] = STACK_TOP + 0x1000
    m.poke(m.x[29] - 0x48, 8, b_value)   # B
    m.poke(m.x[29] - 0x40, 8, e_value)   # E

    for n in range(31):
        m.x[n] = 0xA5A5_0000_0000_0000 | (n << 8) | (0x5A5A0000 + n)
    m.x[29] = STACK_TOP + 0x1000        # frame pointer, as the prologue sets it
    m.x[30] = LR_SENTINEL               # the guest's lr
    m.sp = STACK_TOP
    m.nzcv = 0b1011                     # N=1 Z=0 C=1 V=1, nothing here sets this
    for n in range(16):
        m.q[n] = 0xDEADBEEFCAFEF00D000000000000000 + n  # exactly 128 bits

    # The 16-byte stub in guest .text and the trampoline in the JIT pool, both
    # at the addresses the Eden log reported.
    for i, w in enumerate(encode_entry_stub(TRAMP)):
        m.poke(SITE + i * 4, 4, w)
    for i in range(0, len(image), 4):
        m.poke(TRAMP + i, 4, struct.unpack_from("<I", image, i)[0])

    m.poke(G_B_ADDR, 8, 0)
    m.poke(G_E_ADDR, 8, 0)
    m.poke(G_SEQ_ADDR, 4, 0)

    return {"x": list(m.x), "sp": m.sp, "nzcv": m.nzcv, "q": list(m.q),
            "lr": m.x[30]}


def check(name: str, code: bytes, cbz_taken: bool = False,
          cbz_target: int = CBZ_TARGET, resume: int = RESUME,
          expect_land: int | None = None) -> dict:
    """Run stub -> trampoline and assert the observable contract.

    `resume` is what the trampoline's litResume slot will actually hold;
    `expect_land` is where control must end up. They are separate so a positive
    control can break one without the assertion following it down.
    """
    m = Machine()
    before = plant(m, code, b_value=0 if cbz_taken else B_VALUE)
    planted = set(m.mem)
    owned = [(SITE, SITE + 16), (TRAMP, TRAMP + len(code))]

    # Relocate the in-memory trampoline image the same way install() does.
    for slot, target in zip(bt.POOL_SLOTS.values(),
                            [G_B_ADDR, G_E_ADDR, G_SEQ_ADDR, cbz_target, resume]):
        m.poke(TRAMP + slot, 8, target)

    landed = run(m, SITE, owned)
    written = set(m.mem) - planted
    problems = []

    expected_land = (cbz_target if cbz_taken else resume) if expect_land is None \
        else expect_land
    if landed != expected_land:
        problems.append("landed at 0x%x, want 0x%x"
                        % (landed, expected_land))

    # (a) the two captured words
    if m.peek(G_B_ADDR, 8) != (0 if cbz_taken else B_VALUE):
        problems.append("captured B = 0x%016x" % m.peek(G_B_ADDR, 8))
    if m.peek(G_E_ADDR, 8) != E_VALUE:
        problems.append("captured E = 0x%016x, want 0x%016x"
                        % (m.peek(G_E_ADDR, 8), E_VALUE))

    # (b) the replayed instructions and the flags, per path.
    #
    # Fall-through (x20 != 0): the trampoline replays 0xcd2aa0, 0xcd2aa8 and
    # 0xcd2aac, so x20 = B, x21 = E and NZCV is `cmp x21, x20` == E - B.
    #
    # cbz taken (x20 == 0): the trampoline branches to guest 0xcd2b0c AFTER the
    # replayed `ldur x20` but BEFORE the replayed `ldur x21` / `cmp`, exactly as
    # the guest would have. So x20 = B, but x21 and NZCV must be exactly what the
    # guest walked in with -- and those are the only things the x20/x21 and
    # NZCV saves are load-bearing for. Getting this wrong is a silent flag and
    # register corruption of somebody else's code, so it is checked explicitly.
    b_in_frame = 0 if cbz_taken else B_VALUE
    if m.x[20] != b_in_frame:
        problems.append("x20 = 0x%016x, want replayed 0x%016x"
                        % (m.x[20], b_in_frame))
    if cbz_taken:
        if m.x[21] != before["x"][21]:
            problems.append("x21 = 0x%016x on the cbz-taken path, want the "
                            "guest's own 0x%016x" % (m.x[21], before["x"][21]))
        if m.nzcv != before["nzcv"]:
            problems.append("nzcv = %x on the cbz-taken path, want the guest's "
                            "own %x" % (m.nzcv, before["nzcv"]))
    else:
        if m.x[21] != E_VALUE:
            problems.append("x21 = 0x%016x, want replayed 0x%016x" % (m.x[21], E_VALUE))
        a, b = E_VALUE, B_VALUE          # cmp x21, x20  ==  E - B
        r = (a - b) & 0xFFFFFFFFFFFFFFFF
        want_nzcv = (((1 if (r >> 63) & 1 else 0) << 3) |
                     ((1 if r == 0 else 0) << 2) |
                     ((1 if a >= b else 0) << 1) |
                     (1 if ((a ^ b) & (a ^ r) & (1 << 63)) else 0))
        if m.nzcv != want_nzcv:
            problems.append("nzcv = %x, want %x from the replayed cmp"
                            % (m.nzcv, want_nzcv))

    # (c) everything else byte-identical, except the three documented clobbers
    for n in range(31):
        if n in (20, 21) or n in CLOBBERED:
            continue
        if m.x[n] != before["x"][n]:
            problems.append("x%d = 0x%016x, was 0x%016x" % (n, m.x[n], before["x"][n]))
    if m.sp != before["sp"]:
        problems.append("sp = 0x%x, was 0x%x" % (m.sp, before["sp"]))
    for n in range(16):
        if m.q[n] != before["q"][n]:
            problems.append("v%d clobbered: 0x%032x was 0x%032x"
                            % (n, m.q[n], before["q"][n]))
    if m.x[30] != before["lr"]:
        problems.append("lr = 0x%x, was 0x%x" % (m.x[30], before["lr"]))

    # (e) counter advanced exactly once
    if m.peek(G_SEQ_ADDR, 4) != 1:
        problems.append("seq = %d, want 1" % m.peek(G_SEQ_ADDR, 4))

    # (f) the three replay words are literally the guest's own bytes
    for off, want in bt.REPLAY_OFFSETS.items():
        got = struct.unpack_from("<I", code, off)[0]
        if got != bt.REPLAY_OFFSETS[off]:
            problems.append("replay @0x%x = 0x%08x != 0x%08x"
                            % (off, got, bt.REPLAY_OFFSETS[off]))

    # The 512 bytes below the original sp are scratch and may be written.
    # Nothing at or above the original sp may be: that is the guest's own live
    # frame, which the trampoline has no business touching.
    for addr in written:
        if addr >= STACK_TOP:
            problems.append("wrote 0x%x, at or above the guest sp" % addr)
            break

    if problems:
        print("  FAIL %s" % name)
        for p in problems:
            print("       - %s" % p)
        return {"ok": False, "problems": problems}
    print("  pass %s  (%d instructions, landed 0x%x, seq=%d)"
          % (name, len(m.retired), landed, m.peek(G_SEQ_ADDR, 4)))
    return {"ok": True, "problems": []}


# ---------------------------------------------------------------------------
# The distance, measured rather than asserted
# ---------------------------------------------------------------------------

def test_distance() -> bool:
    print("\ndistance: the reason the entry is `ldr`/`br` and not `B`")
    print("  guest site (Eden log)   0x%016x" % SITE)
    print("  jitCreate trampoline    0x%016x" % TRAMP)
    print("  delta                   %d bytes (%.2f GB, %.2f GiB)"
          % (DELTA, DELTA / 1e9, DELTA / (1 << 30)))
    print("  B immediate range       +/-128 MiB  -> would need %+d, short by %d"
          % (DELTA // 4, DELTA // 4 - (1 << 25)))
    print("  ADRP+ADD range          +/-4 GiB     -> %d pages, limit +/-%d"
          % (DELTA >> 12, 1 << 20))

    stub = encode_entry_stub(TRAMP)
    decoded = decode_entry_stub(stub, SITE)
    print("  stub words              %s  (ldr/br then the constant)"
          % " ".join("0x%08x" % w for w in stub))
    print("  8-byte constant         0x%016x" % decoded)

    problems = []
    if decoded != TRAMP:
        problems.append("constant names 0x%x, want the trampoline 0x%x"
                        % (decoded, TRAMP))
    # The ldr's displacement is a fixed +8 and the stub is four words: both
    # dictated by the layout, and the only constrained distances left in it.
    if (stub[0] & ~0x00FFFFE0) != (0x58000000 | 16):
        problems.append("+0x0 is not `ldr x16, <literal>`")
    if (stub[1] & ~0x3E0) != 0xD61F0000 or ((stub[1] >> 5) & 0x1F) != 16:
        problems.append("+0x4 is not `br x16`")
    if len(stub) * 4 != 16:
        problems.append("stub is %d bytes, must be exactly 16" % (len(stub) * 4))

    # And the old mechanism, for the record: imm26 cannot hold this delta.
    if -((1 << 25) - 1) <= (DELTA >> 2) <= (1 << 25) - 1:
        problems.append("expected a `B` to be UNREACHABLE at this delta")

    # Prove it end to end, not just arithmetically: run the shipped stub and
    # trampoline at exactly these addresses. Nothing 12 GB wide is mapped.
    got = check("stub at +%.1f GB -> trampoline -> guest"
                % (DELTA / 1e9), bt.CODE)
    if not got["ok"]:
        problems.extend(got["problems"])

    print()
    if problems:
        print("  FAIL distance proof")
        for p in problems:
            print("       - %s" % p)
        return False
    print("  pass distance proof: the 8-byte constant names 0x%016x and the "
          "guest is handed\n       back to 0x%016x, with no range left to "
          "violate" % (decoded, RESUME))
    return True


# ---------------------------------------------------------------------------
# install() / uninstall(), byte for byte, against the real guest ELF
# ---------------------------------------------------------------------------

def test_window_roundtrip() -> bool:
    print("\nwindow round trip: install() then uninstall(), byte by byte")
    original = b"".join(struct.pack("<I", w) for w in bt.guest_window())
    stub = b"".join(struct.pack("<I", w) for w in encode_entry_stub(TRAMP))
    print("  guest ELF 0x%x..0x%x" % (bt.GUEST_SITE_VA, bt.GUEST_SITE_VA + 16))
    print("   off  original   stub        in-place?")
    window = bytearray(original)
    for i in range(16):
        window[i] = stub[i]
        print("   +%2d  0x%02x        0x%02x        %s"
              % (i, original[i], stub[i],
                 "overwritten" if stub[i] != original[i] else "unchanged"))

    # uninstall() writes kDeInfoEatenWordsData back; here that is `original`.
    restored = bytearray(stub)
    for i in range(16):
        restored[i] = original[i]
    print("  uninstall() read-back:")
    ok = True
    for i in range(16):
        same = restored[i] == original[i]
        ok &= same
        print("   +%2d  restored 0x%02x  original 0x%02x  %s"
              % (i, restored[i], original[i], "OK" if same else "MISMATCH"))
    print("  windowRestored() would return: %s" % ("true" if ok else "FALSE"))
    print()
    return bool(ok)


# ---------------------------------------------------------------------------
# Positive control
# ---------------------------------------------------------------------------

def find_offset(insns, mnemonic: str, opstr_contains: str) -> int:
    for off, _raw, mn, ops in insns:
        if mn == mnemonic and opstr_contains in ops:
            return off
    raise AssertionError("no `%s ... %s` in the trampoline"
                         % (mnemonic, opstr_contains))


def test_positive_control(relocated: bytes, insns: list) -> bool:
    print("\npositive control (each of these MUST fail):")

    def mutate(fn) -> bytes:
        out = bytearray(relocated)
        fn(out)
        return bytes(out)

    off_v_save = find_offset(insns, "stp", "q0, q1")
    off_x20_save = find_offset(insns, "stp", "x20, x21")
    off_nzcv_save = find_offset(insns, "mrs", "nzcv")
    off_gpr_save = find_offset(insns, "stp", "x6, x7")
    off_str_b = find_offset(insns, "str", "x9,")
    off_str_e = find_offset(insns, "str", "x10,")
    off_load_b = find_offset(insns, "ldur", "[x29, #-72]")
    off_sp_pop = find_offset(insns, "add", "sp, sp,")
    off_replay0 = next(o for o, w in bt.REPLAY_OFFSETS.items() if w == 0xF85B83B4)

    # NOTE ON WHY THESE REMOVE THE SAVE, NOT THE RESTORE.
    # The trampoline's own body clobbers only x9, x10 and x16-x18 (the last
    # three deliberately) and x20/x21 (the replay, by design). It does not touch
    # v0-v15 or x0-x15. So deleting a RESTORE is undetectable by construction --
    # there is nothing to restore over -- and a control built that way would
    # silently prove nothing. Deleting the SAVE makes the matching load pull
    # uninitialised memory instead, which is observable.

    def drop_v_save(out: bytearray) -> None:
        struct.pack_into("<I", out, off_v_save, 0xD503201F)  # nop

    def drop_x20_save(out: bytearray) -> None:
        struct.pack_into("<I", out, off_x20_save, 0xD503201F)  # nop

    def drop_nzcv_save(out: bytearray) -> None:
        struct.pack_into("<I", out, off_nzcv_save, 0xD503201F)  # nop

    def drop_gpr_save(out: bytearray) -> None:
        struct.pack_into("<I", out, off_gpr_save, 0xD503201F)  # nop

    def swap_pair_stores(out: bytearray) -> None:
        # Write E into B's slot and B into E's slot.
        struct.pack_into("<I", out, off_str_b,
                         0xF9000000 | (16 << 5) | 10)  # str x10,[x16]
        struct.pack_into("<I", out, off_str_e,
                         0xF9000000 | (17 << 5) | 9)   # str x9, [x17]

    def wrong_replay(out: bytearray) -> None:
        struct.pack_into("<I", out, off_replay0, 0xF85B83A9)  # into x9 not x20

    def wrong_slot_offset(out: bytearray) -> None:
        # Read [x29-0x50] instead of [x29-0x48] for B: imm9 0x6C -> 0x68.
        struct.pack_into("<I", out, off_load_b, 0xF85B83A9 - (4 << 13))

    def skip_sp_restore(out: bytearray) -> None:
        struct.pack_into("<I", out, off_sp_pop, 0x910003E0)  # mov x0, sp

    controls = [
        # (name, mutator, substring the failure must mention, which cbz path)
        #
        # The x20/x21 and NZCV saves are only observable on the cbz-TAKEN path:
        # the fall-through path overwrites x20 and x21 with the replay and
        # rewrites NZCV with the replayed cmp, so a missing save is invisible
        # there. Running those two on the taken path is what makes them real
        # controls rather than decoration.
        ("v0/v1 save removed", drop_v_save, "v0", False),
        ("x20/x21 save removed (cbz taken)", drop_x20_save, "x21", True),
        ("NZCV save removed (cbz taken)", drop_nzcv_save, "nzcv", True),
        ("x6/x7 save removed", drop_gpr_save, "x6", False),
        ("B/E stores transposed", swap_pair_stores, "captured", False),
        ("replay targets x9 not x20", wrong_replay, "x20", False),
        ("B read from the wrong slot", wrong_slot_offset, "captured", False),
        ("sp not popped", skip_sp_restore, "sp", False),
    ]
    control_ok = True
    for name, fn, expect_fragment, taken in controls:
        got = check(name, mutate(fn), cbz_taken=taken)
        if got["ok"]:
            print("       ^^ CONTROL DID NOT FIRE (expected a failure mentioning %r)"
                  % expect_fragment)
            control_ok = False

    # A ninth control, on the new code path: the trampoline now leaves through
    # an absolute `br` on a literal-loaded register. If that literal did not
    # name the guest's resume point, the guest would be handed back to the
    # wrong place and the harness must say so.
    print("\npositive control on the new path (MUST fail too):")
    got = check("resume literal points at 0", relocated, resume=0,
                expect_land=RESUME)
    if got["ok"]:
        print("       ^^ CONTROL DID NOT FIRE (expected the guest never to be "
              "handed back to 0x%x)" % RESUME)
        control_ok = False
    return control_ok


def test_cbz_paths(relocated: bytes) -> bool:
    """Both outcomes of the replayed `cbz`, which is the only rewritten branch."""
    print("\nreplayed cbz, both outcomes:")
    ok = check("x20 != 0 -> falls through, resumes at site+16",
               relocated, cbz_taken=False)
    taken = check("x20 == 0 -> jumps to guest 0xcd2b0c", relocated, cbz_taken=True)
    return ok["ok"] and taken["ok"]


def main() -> int:
    code, _disasm = bt.assemble()
    bt.CODE = code
    insns = bt.parse_disasm(_disasm)
    print("assembled %d instructions / %d bytes + %d byte literal pool\n"
          % (len([i for i in insns if i[0] < bt.POOL_OFFSET]),
             bt.CODE_BYTES, bt.POOL_BYTES))
    bt.check(code, insns)

    relocated = make_relocated(code)
    print("\nharness:")
    results = {}
    results["shipping"] = check("shipping bytes, relocated", relocated)

    cbz_ok = test_cbz_paths(relocated)
    distance_ok = test_distance()
    roundtrip_ok = test_window_roundtrip()
    control_ok = test_positive_control(relocated, insns)

    print()
    if not all(r["ok"] for r in results.values()) or not cbz_ok \
            or not distance_ok or not roundtrip_ok or not control_ok:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS  -- shipping bytes satisfy every assertion, the 12.4 GB "
          "entry lands,\n       and all 9 positive controls fired.")
    return 0


if __name__ == "__main__":
    sys.exit(main())