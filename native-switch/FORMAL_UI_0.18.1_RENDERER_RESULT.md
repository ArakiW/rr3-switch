# Formal UI 0.18.1 renderer action result

The Y action was blocked before EGL initialization because it required a
recovery marker at a fixed subdirectory path that was not writable/present:

`Could not create the recovery marker; renderer probe blocked.`

No renderer code and no guest code ran. Version 0.18.2 makes the native
offscreen Mesa probe read-only and independent of recovery markers. Persistent
markers are reserved for future guest-constructor actions and use a stable path
directly under `sdmc:/switch/`.
