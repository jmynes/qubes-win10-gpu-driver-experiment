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
- **M1 IN PROGRESS** — fork LookingGlass **LGIdd**, strip IVSHMEM/LGMP/pipe, build+install, confirm a virtual monitor enumerates.
- M2 first static frame to dom0 over grants · M3 live frames · M4 dynamic resolution · M5 upstream PR.
- Milestones: `docs/plan.md`. Technical results: `docs/findings.md`.

## ⚠️ The load-bearing constraint — verify version claims against MS docs
**Windows 10 22H2 (build 19045) ships IddCx 1.5** (`IddCxGetVersion` `0x1500`) — that is
the ceiling. (1.4 = older 19H1/19H2; the first **Win11-only** IddCx is **1.8**;
Mode2/HDR is **1.10**.) The driver must keep its **effective client version ≤ 1.5**:
compile against newer WDK-NuGet headers but **link a ≤1.5 `iddcxstub`**, set a low
`IDDCX_MINIMUM_VERSION_REQUIRED`, and runtime-guard newer APIs with
`IDD_IS_FUNCTION_AVAILABLE`. A 1.10-client driver fails to load (device problem code
**31** / UMDF host **`0xD000000D`**). MttVDD needs IddCx 1.10 (Mode2/HDR) → Win11-only →
that's why the fork base is **LGIdd**. (An earlier draft mis-stated these as 1.4/1.6 —
always confirm IddCx/WDK version facts against Microsoft docs before writing them.)

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
