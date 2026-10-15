# RR3 native rehost probe v13 - segmented CodeMemory mapping

V13 replaces in-place permission changes with native segmented mappings. Each
DSO receives one continuous reserved guest address range. Its pages are merged
into 21 RX/R/RW regions; gaps remain unmapped.

Copy only:

`dist/rr3-native-rehost-v13/rr3-native-rehost.nro`

Launch, wait, press **A**, and photograph the summary.

Stable loader values remain identical to v12. The new expected lines are:

- `loader done=yes retain=yes RX=yes perm=yes ready=no invalid=0`
- `segmap tried=yes ok=yes regions=21/21 fail=0`

The old in-place probes may still report `p73fail=34`; that is expected. Their
failure now triggers the segmented mapper. The final `ranges` count is replaced
with the 21 verified segmented regions.

If segmented mapping fails, the loader prints
`status: segmented CodeMemory mapping failed` and a non-zero segmented failure
count. No constructor executes in either outcome.
