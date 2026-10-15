# Probe v9 real-device result

Status: **passed on a real Nintendo Switch**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/b296b65f01128161d34d78bc5a65e501.jpg`

Confirmed values:

- Bionic `3ff`, pthread `3ff`, libc++ `ff`, C++ lifecycle `3f`, syscall `7`, stdio `7`, phdr `7`.
- Resolver `265`, table `6cdd4e315206951e`.
- libc++ `160/0` symbols, `162/0` relocations, matched hash `91591e75c02f690d`.
- Image-sized JIT `33572 KiB`, create/exec/close success, both edge stubs executed.
- Retained two-DSO link: relative `144089`, internal `2317`, patched/verified `155658/155658`.
- Main H/X/W/M `211/8842/37/315`; libc++ H/X/W/M `162/0/0/0`.
- Unique H/X/M `265/190/305`, invalid `0`.
- Constructors `181+1`, finalizers `4`, lifecycle targets `186/186`.
- Resolution plan hash `ecee08eba8dfe616`.

No game or libc++ constructors were executed in this probe.
