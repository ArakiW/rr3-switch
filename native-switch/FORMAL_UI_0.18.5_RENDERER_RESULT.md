# Formal UI 0.18.5 renderer result

Device result: pressing Y caused Horizon to report that the software closed
because an error occurred.

The copied crash reports correct the initial interpretation. The latest reports
contain only module `hbl` and Result `0xD401` (`InvalidMemoryState`); they do not
contain the 0.18.5 NRO module ID. Therefore the visible failure occurs after the
NRO returns to hbloader. `envSetNextLoad` reuses the same application process,
and the retained four-DSO ProcessCode mappings were not released before hbl
attempted its own NRO unmap/remap operation.

0.18.6's assumed process isolation is invalid and that package must not be used.
0.18.7 instead:

1. reverses all four guest ProcessCode mappings before Y or PLUS may exit;
2. frees backing memory only after successful kernel unmap;
3. blocks handoff and displays the first cleanup Result if any unmap fails;
4. runs the renderer NRO only after hbloader memory state is clean.

No TF-card marker and no guest constructor are involved. Do not retest 0.18.5.
