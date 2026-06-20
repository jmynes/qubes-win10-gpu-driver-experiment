# CLAUDE.md — qubes-win10-gpu-driver-experiment

Guidance for Claude Code. Read this first; it's the operational orientation. Depth lives in `docs/`.

## What this is
A **Windows 10 IddCx indirect display driver for Qubes OS**, closing the
`qubes-gui-agent-windows` "TODO: custom WDDM driver". Goal: dynamic resolution + a
native frame path that grants the guest framebuffer to dom0 (Windows `XcGnttab*` →
`MSG_SHMIMAGE` over the qubes-gui vchan), bypassing the emulated qemu-stubdom GUI.
**This repo is the source of truth** (toolchain glue, scripts, docs, and from M1 on
the forked driver). It does **not** fix the separate >2-vCPU redraw race (that's the
`bcdedit /set groupsize 2` WARP/DWM fix, tracked elsewhere).

## Status
- **M0a DONE** — build toolchain works (VS Build Tools + WDK-via-NuGet, no EWDK).
- **M0b DONE** — risk R1 resolved = **GO** (texture → GPU-copy → CPU buffer; the IddCx system-buffer API is not needed).
- **IddCx ABANDONED (2026-06)** — proven it cannot LOAD on this HVM: an `iddcxstub`-linked UMDF driver fails (the host can't bind the IddCx class extension on this emulated platform); a non-IddCx UMDF driver loads fine with the identical INF. NOT the toolchain/version/CRT/signing/INF/env — all eliminated. **Read the VERDICT in `docs/findings.md`.**
- Toolchain that actually builds drivers here: **EWDK 28000** (`WindowsUserModeDriver10.0` toolset), NOT the v143 + WDK-NuGet path.
- **PIVOT → KMDOD — feasibility gate = GO** (kernel-mode WDDM display-only miniport): the MS KMDOD loads + runs DriverEntry/AddDevice/StartDevice + acquires a POST framebuffer on this HVM (vs IddCx which never loaded). Fixed a real sample bug (`ExAllocatePool2` POOL_TYPE-vs-POOL_FLAGS → `ExAllocatePoolZero`). See `docs/findings.md` "KMDOD pivot". Next: **M1-K own-framebuffer** (own grant-RAM primary + `DxgkDdiPresentDisplayOnly`→our buffer) → M2-K grant to dom0. The frame-path/grant/dynamic-resolution design carries over.
- `driver/` (LGIdd fork) + `driver-mssample/` (MS-sample baseline) are kept as the IddCx reproduction/endpoint. Milestones: `docs/plan.md`. Results: `docs/findings.md`.

## ⚠️ IddCx is blocked on this HVM (the load-bearing finding)
The earlier "keep effective client version ≤ 1.5 / link a ≤1.5 `iddcxstub`" theory was
**DISPROVEN**: the MS sample at IddCx 1.4 *and* the LGIdd fork at 1.10 BOTH fail to load
(device problem **31** / UMDF host **`0xD000000D`**), and the decisive test shows that
*any* `iddcxstub`-linked DLL fails while a non-IddCx UMDF DLL loads with the identical
INF. The UMDF host cannot bind the IddCx class extension on this emulated Qubes HVM —
fundamental, not a driver-side knob (toolchain/version/CRT/signing/INF/render-adapter
all eliminated). Full evidence + eliminations: `docs/findings.md` (the VERDICT section).
The project pivots to a kernel-mode **KMDOD**. (IddCx version facts for reference: Win10
2004..22H2 = IddCx 1.5; first Win11-only = 1.8; Mode2/HDR = 1.10 — but the version is
*not* why it fails here.)

## Environment
- **Dev host:** this Qubes **mgmtvm** (AdminVM). Author + `git` here.
- **Build/test guest:** the qube **`romhacking-hma-driver`** (Win10 Pro 22H2 / 19045;
  Qubes Windows Tools; `groupsize 2`; **test-signing ON**). Build under `C:\dev\…`, staging `C:\dev\pkg`.
- **Reference upstreams (read-only)** at `~/driver-dev/`: `LookingGlass` (`idd/LGIdd` = fork
  base), `Virtual-Display-Driver` (MttVDD — Win11-only, do not fork), `qubes-gui-agent-windows`.

## Dev loop
Edit on mgmtvm → push to guest → build/install in guest:
```bash
./scripts/deploy-to-guest.sh romhacking-hma-driver ./<dir> "C:\dev\LGIdd"   # qrexec push
# then, in the guest:
guest-build.ps1 -ProjDir "C:\dev\LGIdd" -VcxProj "LGIdd.vcxproj"
guest-package-install.ps1 -ProjDir "C:\dev\LGIdd" -RelDir "x64\Release" -Inf "LGIdd.inf" -Hwid "Root\LGIdd"
```
Run guest PowerShell via base64 **UTF-16LE `-EncodedCommand`** over
`qvm-run --pass-io --no-gui <qube>` (avoids quoting/length problems); always set
`$ProgressPreference='SilentlyContinue'`. Mechanics in `docs/install-and-debug.md`.

## Toolchain (no EWDK)
`toolchain/Directory.Build.{props,targets}` let a driver `.vcxproj` build under VS Build
Tools + the `Microsoft.Windows.WDK.x64` NuGet. The `.vcxproj` must use
`PlatformToolset=v143` (not `WindowsUserModeDriver10.0`). Version levers:
`IDDCX_VERSION_*` = compile/header version (may be 1.10); **`IDDCX_STUB_VERSION_*`** = the
linked stub / client version the load-gate checks (**keep ≤1.5; default 1.4**);
`UMDF_VERSION_*`. Recipe + the ~13 debugged build failures: `docs/build-toolchain.md`.

## Install gotchas (all real, hit during M0)
- `signtool` → use `/sm` (machine store) or `/sha1 <thumb>`; `/s My` searches CurrentUser → unsigned cat → publisher-unverified prompt.
- INF `UmdfLibraryVersion=$UMDFVERSION$` is **not** auto-substituted under v143 → set it to `2.25.0`, else the UMDF co-installer fails `0x57`.
- Root-enumerated driver: `pnputil /add-driver` only **stages**; `devcon install <inf> <hwid>` creates the device node + binds. (`devgen` alone leaves a generic SWD device.)
- Capture UMDF load failures: `wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true` then `devcon restart <hwid>`.

## Conventions
- License **GPL-2.0-or-later** (forks LGIdd; targets qubes-gui-agent-windows). Preserve LGIdd copyright headers in forked files.
- Don't commit build outputs / NuGet packages / certs / `.pfx` (see `.gitignore`).
- Persist durable cross-session facts to the mgmtvm memory (`~/.claude/.../memory/`), not just here.
