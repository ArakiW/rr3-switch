from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False; md.skipdata=True
def dis(lo,hi,label):
    print("== %s [%x..%x]"%(label,lo,hi))
    for i in md.disasm(d[lo:hi],lo):
        print("  0x%x: %-10s %s"%(i.address,i.mnemonic,i.op_str))
dis(0x1cbe1e0,0x1cbe260,"SECOND STORE 0x1cbe218")
print()
dis(0x1cbe1a0,0x1cbe1e8,"...prologue of that function")
print()
dis(0x12af510,0x12af570,"sibling read 0x12af534 in the SAME function")
print()
dis(0x1cf2ad8,0x1cf2b60,"CloudcellResume 0x1cf2ad8")
