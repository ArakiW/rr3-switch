# Dependency constructor audit

Deterministic order:

1. `libc++_shared.so` — 1 constructor, 2 finalizers;
2. `libNimble.so` — 0 constructors, 2 finalizers;
3. `libfmodex.so` — 2 constructors, 1 finalizer;
4. `libRealRacing3.so` — 181 constructors, 2 finalizers (not enabled).

All 184 constructor entries and seven finalizer entries resolve to aligned
addresses inside executable PT_LOAD segments. Dependency cycles: zero.

## Enabled 0.19.0 scope

- libc++ target offset `0x94398`: initializes guarded iostream/locale state,
  calls the internal `DoIOSInit`, and registers its destructor through the
  existing `__cxa_atexit` shim.
- FMOD target offset `0x215FC`: initializes global memory/file state and
  registers two destructors through `__cxa_atexit`.
- FMOD target offset `0x216B8`: initializes internal global channel/DSP state.

The enabled targets do not directly enter JNI, Activity, ARCore, telemetry, or
the RR3 main library. Their external imports are already included in the
zero-missing relocation plan.

0.19.0 hardware attribution showed that the first libc++ target entered
`DoIOSInit` correctly but faulted at offset `0x92F4C` while loading the Android
stack guard from `TPIDR_EL0 + 0x28`; TPIDR_EL0 was zero. This occurs before any
FMOD constructor. 0.19.1 wraps all guest lifecycle calls in `GuestTlsScope`.

## Guard and teardown

Before the first call, one durable snapshot records the resolution-plan hash
and all four runtime image base/size ranges. The snapshot remains until clean
teardown. Dependency finalizers run in reverse image and reverse array order,
then four ProcessCode mappings are unmapped. RR3's 181 constructors remain
explicitly gated.
