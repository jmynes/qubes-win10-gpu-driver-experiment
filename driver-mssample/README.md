# driver-mssample — IddCx feasibility endpoint (rebased on the MS sample)

This is the Microsoft `IddSampleDriver` (Windows-driver-samples `video/IndirectDisplay`)
rebranded to Qubes / `Root\QubesIdd`, used as the **minimal, known-good IddCx
baseline** to isolate the load failure after the LGIdd fork (`../driver/`) wouldn't
load. It compiles at IddCx 1.4 (`IDDCX_VERSION_MINOR` property = 4) so the
version-gate theory could be tested directly.

**Result: it does not load either.** See [`../docs/findings.md`](../docs/findings.md)
(the "VERDICT" section): an `iddcxstub`-linked driver cannot load on the
`romhacking-hma-driver` HVM at all — the UMDF host fails to bind the IddCx class
extension on this emulated platform, independent of the driver. This file tree is
kept as the reproduction + the endpoint of the IddCx investigation; the project
pivots to a kernel-mode KMDOD.

Notes:
- `Driver.cpp` carries temporary diagnostic instrumentation (`QLog` → the device's
  `Device Parameters` registry key, since the IddCx UMDF host runs as a
  write-restricted LOCAL SERVICE and cannot write files). Harmless; strip if reused.
- The INF is **UTF-16LE** — edit via an `iconv -f/-t UTF-16LE` round-trip, not raw
  `sed`. Install with the explicit-`AddService=WUDFRd` INF from `../driver/QubesIdd.inf`
  (the sample's `Include=WUDFRD.inf`/`Needs=` form fails `0xe0000219` via devcon here),
  and drop the `[Manufacturer]` `NT$ARCH$.10.0...22000` Win11 gate for Win10.
- Build: EWDK 28000, `WindowsUserModeDriver10.0` toolset (see `../docs/findings.md`).
