# RR3 native rehost probe v12 - segmented permission fallback

V12 keeps the v11 loader results and adds a permission fallback tailored to the
real-device result. SVC 0x73 is attempted first. If all ranges are rejected, the
loader uses current-process SVC 0x02: executable LOAD pages remain RX, data and
rodata pages become RW/R, two gaps become None, and six RELRO ranges become R.

Copy only:

`dist/rr3-native-rehost-v12/rr3-native-rehost.nro`

Launch, wait, press **A**, and photograph the summary.

Expected stable loader values remain:

- loader tests `D/S/F/R/G=7/7/7/f/3f`
- `loader done=yes retain=yes RX=yes ready=no invalid=0`
- relocations `161535/3027/658/8928/39/333`
- patch/read-back `174187/174187`
- RELRO/lifecycle/order/plan hashes unchanged from v11

Expected permission result on this device:

`perm=yes`, `p73=yes`, `fb=yes`, `ranges=29/29`, `fail=0`, `p73fail=34`.

If the SVC 0x02 fallback is also rejected, `perm=no` is safe and expected;
photograph the exact ranges/fail line. No constructor runs while `ready=no`.
