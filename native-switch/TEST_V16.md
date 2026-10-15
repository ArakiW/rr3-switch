# RR3 native rehost probe v16 - process-code diagnosis

V16 removes the invalid normal-map fallback and preserves the first failure
from mode 1. It prints:

- process image and stage
- exact Result
- backing memory type/permission/attribute
- target memory type/permission

Copy only:

`dist/rr3-native-rehost-v16/rr3-native-rehost.nro`

If it succeeds, press A and photograph the summary. If it fails, photograph the
detail lines `segdiag` and `procdiag`.

Process stages:

- 20 `svcMapProcessCodeMemory`
- 21 `svcSetProcessMemoryPermission`
- 22 permission query mismatch
- 23 range arithmetic
- 24 mapped-content mismatch

No constructor executes on failure or while 333 platform relocations remain.
