# Probe v12 real-device result

Status: **loader passed; SVC 0x02 can query RX pages but cannot mutate the
CodeMemory slave mapping**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/0a14418907a3cf7025444c8a788fe9fc.jpg`

Confirmed stable loader values:

- all core and loader self-tests passed
- seven images, `39680 KiB`, retained, RX aliases active
- dependency, relocation, RELRO, lifecycle, order and plan hashes match v11
- invalid `0`

Permission result:

- SVC 0x73: 34 failures
- SVC 0x02 fallback used
- ranges `7/29`, failures `22`
- the seven unchanged executable pages queried as RX
- all 14 non-X LOAD changes, two gap changes, and six RELRO changes were rejected

Conclusion: permissions cannot be mutated in-place on the CodeMemory slave.
The next mapper must reserve one contiguous guest address range and use separate
CodeMemory objects for merged RX, R, RW, and guard intervals.
