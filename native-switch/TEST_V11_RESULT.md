# Probe v11 real-device result

Status: **seven-DSO loader passed; process-level segment permission path rejected**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/77df6a4488f541b392f30b78d7f883d3.jpg`

Confirmed:

- Seven images, `39680 KiB`, retained and executable aliases created.
- LOAD intent `21/7/13/1/0`, TLS `0`.
- Needed `8/32/0`.
- Relocations `161535/3027/658/8928/39/333`.
- Patch/read-back `174187/174187`, invalid `0`.
- RELRO `6`, `1702 KiB`, targets `166249`.
- Lifecycle `365/13/378`, dependency cycles `0`.
- Plan `9362828b4164dbfa`.
- phdr self-test `7`, dynamic-link self-test `3f`.

Permission result:

- process API exposed: yes
- enforced: no
- ranges `0/34`, failures `34`

The environment exposes privileged SVC 0x73, but every permission change on the
CodeMemory slave alias was rejected. The loader remained retained but
`activationReady=no`; no constructor ran. V12 adds a current-process SVC 0x02
fallback that leaves executable pages RX and changes only data/rodata/RELRO
pages.
