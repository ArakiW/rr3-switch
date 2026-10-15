# Formal UI 0.18.0 real-device result

The native UI itself passed:

- fixed Overview, Runtime, Loader and Diagnostics pages rendered correctly;
- navigation worked;
- four core images, zero missing relocations and all deterministic hashes matched;
- all self-tests except phdr/dynamic-link passed (those depend on a retained loader session);
- Mesa probe was not run because loader mapping was gated.

Loader diagnostic:

- process-code mapping succeeded from valid Heap RW backing into an initially
  unmapped target;
- permission ranges `3/4` succeeded before stage 21 returned `0x0000d401`;
- the fourth operation was a RELRO override applied to pages already converted
  from AliasCode to RW.

Conclusion: ModuleCode pages must receive their final permission in one
transition. Version 0.18.1 merges LOAD and RELRO page intent first and submits
11 non-overlapping final ranges. No constructor ran.
