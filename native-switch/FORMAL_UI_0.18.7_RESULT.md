# Formal UI 0.18.7 device result

Device report: passed.

Interpreted against the prescribed consolidated sequence, this confirms:

- the normal four-DSO loader still reaches the formal dashboard;
- PLUS completes the reverse ProcessCode cleanup and returns to hbmenu without
  the previous `hbl 0xD401` error;
- the same installed NRO can be launched again after cleanup;
- Y completes the unload-gated renderer NRO handoff and returns to the
  console-only result stage;
- no TF-card recovery marker and no guest constructor are involved.

The loader framework now has device evidence for both directions of its
lifecycle: four-image map/link/permission and four-image unmap/return.
