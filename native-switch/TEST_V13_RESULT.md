# Probe v13 real-device result

Status: **final-base relocation pass succeeded; segmented CodeMemory mapping failed**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/53e12da2a3c62624f3de756d7215636b.jpg`

The second relocation pass against reserved segmented guest bases remained
correct: patch/verify `174187/174187`, invalid `0`, RELRO `6`, lifecycle
`378/378`. Failure occurred only while creating or validating one of the 21
native CodeMemory regions. V13 did not expose the region/stage/Result, so V14
adds exact diagnostics and flushes each RW owner before establishing its slave.

No constructor ran; the partial segmented mapping was rolled back.
