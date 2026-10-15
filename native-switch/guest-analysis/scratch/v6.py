from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False; md.skipdata=True
def dis(lo,hi,label,grep=None):
    print("== %s [%x..%x]"%(label,lo,hi))
    for i in md.disasm(d[lo:hi],lo):
        s="  0x%x: %-10s %s"%(i.address,i.mnemonic,i.op_str)
        if grep is None or grep in s: print(s)
print("### (a) function containing 0x1cbe218 -- any reference to Cloudcell GOT 0x1f6d5d0?")
dis(0x1cbdfa0,0x1cbe23c,"store-holder fn","1f6d")
print("  (prologue / x19 origin):")
dis(0x1cbdfa0,0x1cbe020,"prologue")
print()
print("### (b) 0xab2c30 async-request gate polarity")
dis(0xab2c60,0xab2cc0,"ab2c region")
