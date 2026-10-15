# Formal UI 0.18.1 real-device result

Status: **core loader framework passed on a real Nintendo Switch**.

Evidence images:

- Overview: `C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/c13a38e05c10b1123625e5544c32658f.png`
- Runtime: `C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/159cb594af1b6e90c99439773693a2ad.png`
- Loader: `C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/f39b25dce1df5424c5c3c5fb0e000979.png`
- Diagnostics: `C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/cc7dd890e429e53d245da62f038a0ce5.png`

Confirmed:

- Core DSO loader OK.
- Segment permissions OK: RX/R/RW/RELRO enforced.
- Guest activation READY.
- Four images, 35156 KiB.
- Needed 24/4/20/0.
- Relative/internal/host/cross/weak/missing
  `145752/3026/731/8899/37/0`.
- Patched/verified `158445/158445`, invalid `0`.
- Lifecycle `184/7/191`, cycles `0`.
- Process-code stage/result `0/0`.
- Segment map mode 1, `4/4`, failures `0`.
- Permission ranges `11/11`, failures `0`.
- Resolver `531 / 594136e24e970742`.
- Plan `999a029e95729174`.
- Every runtime self-test passed.
- Recovery guard clean.

Mesa offscreen renderer remained IDLE in the supplied photos. It can be tested
with Y in the same installed package; no NRO replacement is required.
