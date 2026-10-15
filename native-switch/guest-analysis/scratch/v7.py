from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=False; md.skipdata=True
for i in md.disasm(d[0x1cbe030:0x1cbe0a0],0x1cbe030):
    print("  0x%x: %-10s %s"%(i.address,i.mnemonic,i.op_str))
print("---- search whole binary for stores into the Cloudcell global GOT slot 0x1f6d5d0 ----")
cnt=0
for i in md.disasm(d[:0x1e19ae0],0):
    if i.id==0: continue
    if i.mnemonic in ("str","strb","strh") and "[x22, #0x5d0]" in i.op_str or (i.mnemonic in ("str",) and "#0x5d0]" in i.op_str):
        print("  0x%x: %-10s %s"%(i.address,i.mnemonic,i.op_str)); cnt+=1
print("candidates:",cnt)
