import struct
from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False
def dis(lo,hi,label):
    print("== %s [%x..%x]"%(label,lo,hi))
    for i in md.disasm(d[lo:hi],lo):
        print("  0x%x: %-10s %s"%(i.address,i.mnemonic,i.op_str))
dis(0x12aeb20,0x12aefc0,"body before gate (find x23)")
