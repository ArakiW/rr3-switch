from capstone import *
SO=r"E:\AI\RealRacing3-switch\reverse\native\arm64\libRealRacing3.so"
d=open(SO,'rb').read()
TEXT_END=0x1e19ae0
md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); md.detail=True; md.skipdata=True
MEMM={"ldrb","strb","ldrsb","ldrh","strh","ldr","str","ldrsw","ldarb","stlrb","ldurb","sturb","ldar","ldxr","stxr","ldaxr","stlxr"}
imm_hits=[]; reg_hits=[]; total=0; skipped=0
for i in md.disasm(d[:TEXT_END],0):
    if i.id==0:
        skipped+=1; continue
    total+=1
    if i.mnemonic not in MEMM: continue
    if len(i.operands)<2: continue
    m=i.operands[1].mem
    if m.disp==0x4c1: imm_hits.append((i.address,i.mnemonic,i.op_str))
    elif m.index!=0: reg_hits.append((i.address,i.mnemonic,i.op_str))
print("total decoded real instructions: %d   (skipdata gaps: %d)"%(total,skipped))
print("=== immediate displacement == 0x4c1 : %d"%len(imm_hits))
for h in imm_hits: print("   0x%x: %-8s %s"%h)
print("=== register-offset load/store (blind spot of an imm-only sweep): %d"%len(reg_hits))
for h in reg_hits[:40]: print("   0x%x: %-8s %s"%h)
