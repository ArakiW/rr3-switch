"""Guest ELF function ranges for libRealRacing3.so.

Strategy: .eh_frame_hdr's binary-search table already stores fully-decoded
absolute `initial_location` values, so we never need the CIE pointer (whose
base is ambiguous in this binary). We only decode `address_range` from each
FDE, which is a plain sdata4 absolute length and is base-independent.

Verified: first FDE decodes to [0x987ff8,+0x10) which abuts .text (0x988000).
"""
import struct
import capstone

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
_d = open(SO, "rb").read()

# ---- sections ----
_e_shoff = struct.unpack_from("<Q", _d, 0x28)[0]
_sh = [struct.unpack_from("<IIQQQQ", _d, _e_shoff + i * struct.unpack_from("<H", _d, 0x3A)[0])
       for i in range(struct.unpack_from("<H", _d, 0x3C)[0])]
_st = _sh[struct.unpack_from("<H", _d, 0x3E)[0]]
_sd = _d[_st[4]:_st[4] + _st[5]]


def _nm(n):
    return _sd[n:_sd.index(b"\0", n)].decode()


SEC = {}
for _s in _sh:
    SEC.setdefault(_nm(_s[0]), _s)


def _va2off(va):
    for i in range(struct.unpack_from("<H", _d, 0x38)[0]):
        o = struct.unpack_from("<Q", _d, 0x20)[0] + i * struct.unpack_from("<H", _d, 0x36)[0]
        if struct.unpack_from("<I", _d, o)[0] == 1:
            p_off, p_va, _, p_fs, p_ms = struct.unpack_from("<QQQQQ", _d, o + 8)
            if p_va <= va < p_va + p_fs:
                return p_off + (va - p_va)
    return None


# ---- .eh_frame_hdr binary search table ----
# layout: version(1) ptr_enc(1) fde_count_enc(1) table_enc(1)
#         eh_frame_ptr(4, pcrel sdata4) fde_count(4) table...
_hdr = SEC[".eh_frame_hdr"]
_cnt_size = 4 if _d[_hdr[4] + 2] == 0x03 else 8
_efp_size = 4                              # eh_frame_ptr_enc is 0x1b -> sdata4
_fde_count = struct.unpack_from("<I" if _cnt_size == 4 else "<Q",
                                _d, _hdr[4] + 4 + _efp_size)[0]
_tbl = _hdr[4] + 4 + _efp_size + _cnt_size
_base = _hdr[3]
FDE = []
for _k in range(_fde_count):
    _i0, _f0 = struct.unpack_from("<ii", _d, _tbl + _k * 8)
    FDE.append((_i0 + _base, _f0 + _base))
FDE.sort()


def _range(va):
    """(start, end) of the function CONTAINING va, or None.

    The containment test is the whole point, and getting it wrong is silent:
    the binary search below only establishes that `va` is at or after this
    FDE's initial_location, so on its own it happily returns a function whose
    body ended long before `va` -- i.e. it treats the gap between two functions
    as a hit. That happened: 0x223aa80 came back as "inside [0x1e17a30,
    0x1e17a60)", which is 35 MB earlier, and a whole line of reasoning was
    built on the answer. `_range` now returns None for anything outside
    [start, start+size), and `_selfcheck()` proves it on a known gap.
    """
    lo, hi, idx = 0, len(FDE) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if va < FDE[mid][0]:
            hi = mid - 1
        elif mid + 1 < len(FDE) and va >= FDE[mid + 1][0]:
            lo = mid + 1
        else:
            idx = mid
            break
    if idx is None:
        return None
    s, fde_pc = FDE[idx]
    o = _va2off(fde_pc)
    if o is None:
        return None
    # FDE: [0]=length [4]=CIE_pointer [8]=initial_location(enc) [8+encsize]=address_range
    size = struct.unpack_from("<i", _d, o + 12)[0]
    if size <= 0 or size > 0x400000:
        return None
    end = s + size
    if not (s <= va < end):          # the gap between functions is NOT a hit
        return None
    return (s, end)


def _selfcheck():
    """Prove _range rejects a gap and accepts a real entry.

    A checker that cannot fail proves nothing, so this deliberately feeds it
    addresses it is known to get wrong: a data-section address (past the last
    FDE entirely) and an address one past a known function's end.
    """
    ok = True
    known_start = 0x12aea7c
    r = _range(known_start)
    if r is None or r[0] != known_start:
        print("SELFCHECK FAIL: render function entry 0x%x not found" % known_start)
        ok = False
    else:
        # A gap is the gap BETWEEN two FDEs, not "one past a function end":
        # immediately after a function the next one often begins, and that
        # address genuinely belongs to it. Pick a real hole instead.
        i = FDE.index(FDE[next(k for k, v in enumerate(FDE) if v[0] == known_start)])
        nxt = FDE[i + 1][0] if i + 1 < len(FDE) else None
        if nxt is not None and nxt - r[1] > 8:
            gap = r[1] + (nxt - r[1]) // 2
            if _range(gap) is not None:
                print("SELFCHECK FAIL: _range accepted 0x%x, inside the gap "
                      "[0x%x,0x%x)" % (gap, r[1], nxt))
                ok = False
    if _va2off(0x223aa80) is None:      # data segment, never code
        if _range(0x223aa80) is not None:
            print("SELFCHECK FAIL: _range accepted a DATA address as code")
            ok = False
    if ok:
        print("SELFCHECK PASS: _range accepts entries, rejects gaps and data")
    return ok


def func_start(va):
    lo, hi, best = 0, len(FDE) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if FDE[mid][0] <= va:
            best = FDE[mid][0]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


MD = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
MD.detail = True


def insns(va, n=None):
    """Disassemble the function containing va. `n` only ever SHRINKS the result:
    an explicit n must never widen the scan past the FDE range."""
    r = _range(va)
    if r is None:
        start, end = va, va + (n or 16) * 4
    else:
        start, end = r
    cap = (end - start) // 4
    if n is not None:
        cap = min(cap, n) if n >= 0 else cap
    o = _va2off(start)
    if o is None or cap <= 0:
        return []
    return list(MD.disasm(_d[o:o + 4 * cap], start))


# ---- symbol names from .dynsym ----
NAMES = {}
for _sec in (".dynsym", ".symtab"):
    s = SEC.get(_sec)
    if not s:
        continue
    for o in range(s[4], s[4] + s[5], 24):
        st_name = struct.unpack_from("<I", _d, o)[0]
        st_value = struct.unpack_from("<Q", _d, o + 8)[0]
        st_size = struct.unpack_from("<Q", _d, o + 16)[0]
        if st_value and st_name < len(_sd) and b"\0" in _sd[st_name:st_name + 256]:
            NAMES.setdefault(st_value, (_sd[st_name:_sd.index(b"\0", st_name)].decode("utf-8", "replace"), st_size))


def name(va):
    return NAMES.get(va, (None, 0))[0]


# ---- PLT slot -> imported symbol, via GOT (NOT via .rela.plt index order) ----
# The .rela.plt index -> PLT slot mapping is NOT plt_base + i*16 in this binary;
# doing that silently produced plausible-but-wrong names (0x1e18f30 came out as
# "listen" when it is "poll"). A PLT stub is `adrp x16,page ; ldr x17,[x16,#off]
# ; br x17`, so resolving the GOT slot the stub actually loads is authoritative.
_GOT = {}
for _n, _s in SEC.items():
    if not _n.startswith(".rela"):
        continue
    _ds, _st = SEC[".dynsym"], SEC[".dynstr"]
    for _o in range(_s[4], _s[4] + _s[5], 24):
        _r_off, _r_info = struct.unpack_from("<QQ", _d, _o)
        _so = _ds[4] + (_r_info >> 32) * 24
        _p = _st[4] + struct.unpack_from("<I", _d, _so)[0]
        _GOT.setdefault(_r_off, _d[_p:_d.index(b"\0", _p)].decode("utf-8", "replace"))


def plt_name(va):
    """Resolve a PLT stub address to its imported symbol, or None if internal."""
    o = _va2off(va)
    if o is None:
        return None
    page = addi = None
    for i in MD.disasm(_d[o:o + 12], va):
        if i.mnemonic == "adrp":
            page = int(i.op_str.split("#")[1], 16)
        elif i.mnemonic == "add" and page is not None:
            addi = int(i.op_str.split("#")[1].rstrip("!]"), 16)
            break
    if page is None or addi is None:
        return None
    return _GOT.get(page + addi)


def sym(va):
    """Best-effort symbol for a call target: dynsym name, else PLT import, else None."""
    n = name(va)
    if n:
        return n
    return plt_name(va)


def calls(va, limit=4000):
    """Unique direct bl targets inside the function containing va."""
    seen, out = set(), []
    for i in insns(va, limit):
        if i.mnemonic == "bl" and i.op_str.startswith("#"):
            t = int(i.op_str[1:], 16)
            if t not in seen:
                seen.add(t)
                out.append((i.address, t))
    return out


def _is_epilogue(addr):
    """True if `addr` starts a return sequence (pops, then ret).

    Counting these as back edges is how a whole investigation went wrong: a
    compiler-emitted `b.<cond> -> epilogue` after a stack-canary check reads
    exactly like a loop back edge, so every "N backward branches" number came
    out inflated. A backward branch to an epilogue is a RETURN, not a loop.
    """
    o = _va2off(addr)
    if o is None:
        return False
    ops = [i.mnemonic for i in list(MD.disasm(_d[o:o + 48], addr))[:8]]
    if "ret" not in ops:
        return False
    ret_i = ops.index("ret")
    if ret_i == 0:
        return True
    return any(m in ("ldp", "ldr", "add") for m in ops[:ret_i])


def back_branches(va, limit=4000, include_epilogue=False):
    """Backward branches inside the function containing va.

    A branch whose target is an epilogue is EXCLUDED by default: that is a
    return, not a loop. include_epilogue=True gives the raw count, which is
    only useful for auditing this filter against a known example.
    """
    r = _range(va)
    if not r:
        return []
    s, e = r
    out = []
    for i in insns(va, limit):
        if i.op_str.startswith("#") and i.mnemonic[0] == "b" and i.mnemonic != "bl":
            t = int(i.op_str.split(",")[-1][1:], 16)
            if s <= t <= i.address:
                if not include_epilogue and _is_epilogue(t):
                    continue
                out.append((i.address, i.mnemonic, t))
    return out


def dump(va, n=20, label=""):
    o = _va2off(va)
    print("--- %s 0x%x ---" % (label, va))
    if o is None:
        print("  unmapped")
        return
    for i in MD.disasm(_d[o:o + 4 * n], va):
        print("  0x%09x: %-10s %s" % (i.address, i.mnemonic, i.op_str))


if __name__ == "__main__":
    import sys
    print("FDE count = %d" % len(FDE))
    r = _range(0x988000 - 8)
    print("sanity: 0x%08x -> %s" % (0x988000 - 8,
          "[0x%x,0x%x) sz=0x%x" % (r[0], r[1], r[1] - r[0]) if r else "NONE"))
    print()
    targets = [0x12aea7c, 0x1c699d0, 0x1c69a98, 0x12e15e8, 0x12e1600, 0x12e13ac,
               0x12e13ec, 0x1406f94, 0x12ab500, 0x12b4824, 0x12b3614, 0x12b43a0,
               0x1404a38, 0x146ce80, 0x13fb2d0, 0x1c69928, 0x146cfa4]
    for t in targets:
        r = _range(t)
        print("0x%09x  %-30s  is_func_start=%s  %s" % (
            t,
            ("[0x%x,0x%x) sz=0x%x" % (r[0], r[1], r[1] - r[0])) if r else "NO RANGE",
            r is not None and r[0] == t,
            name(t) or ""))
    for a in sys.argv[1:]:
        va, _, lbl = a.partition(":")
        dump(int(va, 16), 20, lbl)
