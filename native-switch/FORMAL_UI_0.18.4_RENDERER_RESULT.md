# Formal UI 0.18.4 renderer result

Device result:

- ordinary launch and the four-page dashboard remained healthy;
- pressing Y at runtime returned the startup-only instruction without a crash;
- holding Y while launching did not request the renderer mode.

This validates removal of the unsafe runtime console/EGL transition, but shows
that controller state is not a reliable cross-loader launch signal. No Mesa
probe and no guest constructor ran.

0.18.5 replaces held-button sampling with hbloader's in-memory next-load ABI.
Pressing Y schedules the current NRO to restart with `--rr3-renderer`; the fresh
process performs EGL before `consoleInit`. No TF-card marker is used.
