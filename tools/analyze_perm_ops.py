from pathlib import Path
import struct


def align_down(x):
    return x & ~0xFFF


def align_up(x):
    return (x + 0xFFF) & ~0xFFF


PT_LOAD = 1
PT_GNU_RELRO = 0x6474E552
PF_X = 1
PF_W = 2


def perm(flags):
    if flags & PF_X:
        return "RX"
    if flags & PF_W:
        return "RW"
    return "R"


root = Path(r"E:\AI\RealRacing3-switch\reverse\native\arm64")
names = [
    "libRealRacing3.so",
    "libNimble.so",
    "libfmodex.so",
    "libc++_shared.so",
]
total_phdr = 0
total_merged = 0
for name in names:
    data = (root / name).read_bytes()
    assert data[:4] == b"\x7fELF"
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    loads = []
    relro = None
    minva = None
    maxva = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_vaddr, _p_paddr, _p_filesz, p_memsz, _p_align = struct.unpack_from(
            "<QQQQQ", data, off + 16
        )
        if p_type == PT_LOAD:
            loads.append((p_vaddr, p_memsz, p_flags))
            if minva is None or p_vaddr < minva:
                minva = p_vaddr
            maxva = max(maxva, p_vaddr + p_memsz)
        if p_type == PT_GNU_RELRO:
            relro = (p_vaddr, p_memsz)
    pages = align_up(maxva - minva) // 0x1000
    state = [None] * pages
    overlap = 0
    for vaddr, memsz, flags in loads:
        s = (align_down(vaddr) - minva) // 0x1000
        e = (align_up(vaddr + memsz) - minva) // 0x1000
        p = perm(flags)
        for pg in range(s, e):
            if state[pg] is not None:
                overlap += 1
            state[pg] = p
        total_phdr += 1
    if relro:
        vaddr, memsz = relro
        s = (align_down(vaddr) - minva) // 0x1000
        e = (align_up(vaddr + memsz) - minva) // 0x1000
        for pg in range(s, e):
            state[pg] = "R"
        total_phdr += 1
    ops = 0
    page = 0
    while page < pages:
        t = state[page]
        end = page + 1
        while end < pages and state[end] == t:
            end += 1
        if t is not None:
            ops += 1
        page = end
    total_merged += ops
    print(
        f"{name}: min=0x{minva:x} max=0x{maxva:x} pages={pages} "
        f"loads={len(loads)} relro={relro} phdr_ops={len(loads)+(1 if relro else 0)} "
        f"merged={ops} overlap_pages={overlap}"
    )
    for vaddr, memsz, flags in loads:
        print(
            f"  LOAD va=0x{vaddr:x} mem=0x{memsz:x} flags={flags:x} -> {perm(flags)} "
            f"alignD=0x{align_down(vaddr):x} alignU=0x{align_up(vaddr+memsz):x}"
        )
    if relro:
        vaddr, memsz = relro
        print(
            f"  RELRO va=0x{vaddr:x} mem=0x{memsz:x} "
            f"alignD=0x{align_down(vaddr):x} alignU=0x{align_up(vaddr+memsz):x}"
        )

print(f"TOTAL phdr_ops={total_phdr} merged={total_merged}")
