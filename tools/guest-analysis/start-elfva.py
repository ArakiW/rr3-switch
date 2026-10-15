"""Recompute guest start_routine ELFVAs from census log and disassemble them.

Key: the `start=` field in guest-thread lines is a HOST pointer. We recover the
guest image base by subtracting the already-computed lr_elfva from lr, then
translate. This is the check the previous pass reported as "start parsed null".
"""
import struct, sys, re
import capstone

SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d = open(SO, "rb").read()

e_phoff = struct.unpack_from("<Q", d, 0x20)[0]
e_phentsize = struct.unpack_from("<H", d, 0x36)[0]
e_phnum = struct.unpack_from("<H", d, 0x38)[0]

segs = []
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_flags = struct.unpack_from("<II", d, off)
    p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = struct.unpack_from("<QQQQQ", d, off + 8)
    if p_type == 1:
        segs.append((p_vaddr, p_offset, p_filesz, p_memsz, p_flags))


def va2off(va):
    for v, o, fs, ms, fl in segs:
        if v <= va < v + fs:
            return o + (va - v)
    return None


def which_seg(va):
    for i, (v, o, fs, ms, fl) in enumerate(segs):
        if v <= va < v + ms:
            return i
    return -1


# ---- FDE table from .eh_frame_hdr (binary search table) ----
e_shoff = struct.unpack_from("<Q", d, 0x28)[0]
e_shentsize = struct.unpack_from("<H", d, 0x3A)[0]
e_shnum = struct.unpack_from("<H", d, 0x3C)[0]
e_shstrndx = struct.unpack_from("<H", d, 0x3E)[0]
sh = []
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    nm, ty, fl, ad, off, sz = struct.unpack_from("<IIQQQQ", d, o)
    sh.append((nm, ty, fl, ad, off, sz))
strtab = sh[e_shstrndx]
strdata = d[strtab[4]:strtab[4] + strtab[5]]


def secname(n):
    e = strdata.index(b"\0", n)
    return strdata[n:e].decode()


fdes = []
for s in sh:
    if secname(s[0]) == ".eh_frame_hdr":
        # version(1) eh_frame_ptr_enc(1) fde_count_enc(1) table_enc(1)
        v, phe, fde_cnt_enc, table_enc = d[s[4]], d[s[4] + 1], d[s[4] + 2], d[s[4] + 3]
        # assume pcrel sdata4 for both pointers and 4-byte count
        sh_addr, s_off = s[3], s[4]
        datadr = s_off + 4
        pcrel = (phe & 0x70) >> 4
        cnt_size = 4 if fde_cnt_enc == 0x03 else 8
        cnt = struct.unpack_from("<I" if cnt_size == 4 else "<Q", d, datadr)[0]
        tbl = datadr + cnt_size
        base = sh_addr
        for k in range(cnt):
            init, fde = struct.unpack_from("<ii", d, tbl + k * 8)
            fdes.append((init + base, fde + base))
        break
fdes.sort()
print("FDE count = %d" % len(fdes))


def fde_containing(va):
    """Return (func_start, fde_start, fde_end) if va lies inside an FDE."""
    lo, hi = 0, len(fdes) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        s = fdes[mid][0]
        if va < s:
            hi = mid - 1
        elif mid + 1 < len(fdes) and va >= fdes[mid + 1][0]:
            lo = mid + 1
        else:
            # va in [fdes[mid][0], fdes[mid+1][0])
            idx = mid
            break
    else:
        return None
    if idx < 0 or idx + 1 >= len(fdes):
        return None
    fs, fde_pc = fdes[idx][0], fdes[idx + 1][1]
    # decode FDE length/pointer to get its range
    p = va2off(fde_pc)
    if p is None:
        return None
    ln = struct.unpack_from("<I", d, p)[0] & 0xFFFFFFFF
    if ln == 0:
        ln = struct.unpack_from("<Q", d, p)[0] & 0xFFFFFFFFFFFFFFFF
        hdr = 12
    else:
        hdr = 8
    if ln == 0xFFFFFFFF:
        return None
    ip = d[p + hdr]
    # CIE augmentation 'R' => fde enc byte follows
    # for our build enc is usually 0x1b (pcrel|sdata4)
    if ip & 0x0F == 0x0B:
        enclen = 4
    else:
        enclen = 8
    lo_pc = struct.unpack_from("<i", d, p + hdr + enclen)[0] + fde_pc
    return (fs, lo_pc, fde_pc + ln)


md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True


def dis(va, n, label):
    off = va2off(va)
    print("=== %s  elfva=0x%x seg=%d off=%s" % (label, va, which_seg(va), hex(off) if off else None))
    if off is None:
        print("  (unmapped)")
        return
    for ins in md.disasm(d[off:off + 4 * n], va):
        print("  0x%09x: %-10s %s" % (ins.address, ins.mnemonic, ins.op_str))
    print()


LOG = sys.argv[1] if len(sys.argv) > 1 else \
    r"E:\AI\RealRacing3-switch\.build\eden-runs\20261004-045627-lr3\rr3-native-rehost.guest-view.log"
txt = open(LOG, encoding="utf-8", errors="replace").read()

kv = lambda line: dict(re.findall(r"(\w+)=(\S+)", line))

print("### log:", LOG)
rows = []
for line in txt.splitlines():
    if "phase=guest-thread ordinal=" not in line:
        continue
    f = kv(line)
    if "lr_elfva" not in f or "start" not in f or "lr" not in f:
        continue
    base = int(f["lr"], 16) - int(f["lr_elfva"], 16)
    start = int(f["start"], 16)
    start_elfva = start - base
    tgt = int(f.get("target", "0"), 16)
    rows.append((int(f["ordinal"]), f.get("lr_elfva"), start_elfva, tgt - base, f.get("lr_img")))
    print("ord=%s lr_elfva=%s -> base=0x%x  start_elfva=0x%x  target_elfva=0x%x (%s)"
          % (f["ordinal"], f["lr_elfva"], base, start_elfva, tgt - base, f.get("lr_img")))

print()
for ordn, lrva, sev, tev, img in rows:
    if img != "libRealRacing3.so":
        print("ord=%s start_elfva=0x%x  NOT in guest image (%s) -- skip\n" % (ordn, sev, img))
        continue
    f = fde_containing(sev)
    print(">>> ordinal=%s start_elfva=0x%x  FDE=%s" % (ordn, sev,
          ("func=0x%x  [0x%x,0x%x)" % (f[0], f[1], f[2])) if f else "NONE"))
    dis(sev, 14, "start_elfva 0x%x" % sev)
    if f and sev != f[0]:
        dis(sev - 0x20, 8, "32B BEFORE 0x%x" % sev)
