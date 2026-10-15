# Probe v10 real-device result

Status: **loader stopped at an internal validation gate**.

Evidence image:

`C:/Users/L/Documents/Tencent Files/474485927/nt_qq/nt_data/Pic/2026-08/Ori/44148817a2f9bcb8e99aa1c10fd52475.jpg`

Passed before the loader session:

- Bionic `3ff`, pthread `3ff`, libc++ `ff`, C++ lifecycle `3f`, syscall `7`, stdio `7`.
- Resolver `270`, table `ed0e63ce1f6e2322`.
- libc++ `160/0` symbols and `162/0` relocations.
- Image-sized JIT `33572 KiB`, create/exec/close success, both edge stubs executed.
- Loader rollback self-test `f`.

Failure:

- `status: loader relocation or lifecycle validation failed`
- phdr self-test remained `0`; dynamic-link self-test reached only bit `1`
  because the loader session was rolled back.

Root cause found in the loader's own RELRO validation: Android GNU RELRO ranges
are page-rounded and may legally span PT_LOAD tail padding. V10 incorrectly
required the entire RELRO range to be contained in one PT_LOAD segment. The
loader correctly refused activation and rolled the session back; no guest
constructor ran. V11 validates RELRO against the complete mapped image instead.
