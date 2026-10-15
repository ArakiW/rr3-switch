import struct, sys
from capstone import *
SO = r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d = open(SO,'rb').read()
assert d[:4]==b'\x7fELF'
is64 = d[4]==2
e_phoff = struct.unpack_from('<Q', d, 0x20)[0]
e_phentsize = struct.unpack_from('<H', d, 0x36)[0]
e_phnum = struct.unpack_from('<H', d, 0x38)[0]
segs=[]
for i in range(e_phnum):
    o=e_phoff+i*e_phentsize
    p_type,p_flags=struct.unpack_from('<II', d, o)
    p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_align=struct.unpack_from('<QQQQQQ', d, o+8)
    if p_type==1:
        segs.append((p_vaddr,p_filesz,p_offset,p_flags,p_memsz))
        print("LOAD vaddr=0x%x filesz=0x%x off=0x%x flags=%d memsz=0x%x"%(p_vaddr,p_filesz,p_offset,p_flags,p_memsz))
def v2o(v):
    for va,fs,off,fl,ms in segs:
        if va<=v<va+fs: return off+(v-va)
    return None
md=Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail=False
def dis(va,n=16,label=""):
    o=v2o(va)
    if o is None:
        print("VA 0x%x NOT MAPPED"%va); return
    code=d[o:o+n*4]
    print("== %s  va=0x%x fileoff=0x%x"%(label,va,o))
    for i in md.disasm(code,va):
        print("   0x%x: %-9s %s"%(i.address,i.mnemonic,i.op_str))
for va in (0x12aefd8,0x12aefdc,0x12af010,0x12af014,0x10f0990,0xab2c30,0xab2c38):
    dis(va,4,"probe")

