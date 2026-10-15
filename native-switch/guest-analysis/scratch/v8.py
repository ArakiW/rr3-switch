from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False; md.skipdata=True
print("### stores into [x?, 0x4b0..0x4d0] inside Cloudcell ctor 0x1cbe23c..0x1cbe600")
for i in md.disasm(d[0x1cbe23c:0x1cbe700],0x1cbe23c):
    if i.mnemonic in ("str","strb","strh","stur","sturb","stp","sturp","stxr","stlxr","mstp"):
        if any(("#0x%x]"%o) in i.op_str for o in range(0x4b0,0x4d4,1)):
            print("  0x%x: %-8s %s"%(i.address,i.mnemonic,i.op_str))
print("### whole-binary: stores with disp 0x4c0 (8-byte store covers byte 0x4c1)")
n=0
for i in md.disasm(d[:0x1e19ae0],0):
    if i.id==0: continue
    if i.mnemonic in ("str","stur","stp","strb") and "#0x4c0]" in i.op_str:
        print("  0x%x: %-8s %s"%(i.address,i.mnemonic,i.op_str)); n+=1
        if n>25: break
print("  (capped at 26)")
