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
- **PIVOT → KMDOD** (kernel-mode WDDM display-only miniport): the MS KMDOD loads + runs DriverEntry/AddDevice/StartDevice on this HVM (vs IddCx which never loaded). Fixed a real sample bug (`ExAllocatePool2` POOL_TYPE-vs-POOL_FLAGS → `ExAllocatePoolZero`). See `docs/findings.md` "KMDOD pivot".
- **✅ BREAKTHROUGH (2026-06-20) — the FINAL VERDICT is OVERTURNED; a custom WDDM display-only driver WORKS here.** On clone `romhacking-hma-driver-clone2` the **KMDOD is the live active display @1920×1080** (`CM_PROB_NONE`): it loads, starts, commits a VidPn, and presents continuously. The old `problem=43` was two newer-WDK-vs-older-OS build bugs, **not** the platform (the inbox `BasicDisplay.sys` is itself a working WDDM display-only miniport on this same stdvga). Two fixes — see `docs/findings.md` "✅ BREAKTHROUGH":
  1. **DDI version gate** — built with EWDK 28000 → `DXGKDDI_INTERFACE_VERSION=WDDM3_2` → `DXGK_DRIVERCAPS` compiled too large → `QueryAdapterInfo(DRIVERCAPS)` `OutputDataSize < sizeof` rejection (problem 43 before VidPn). Fix: pin `DXGKDDI_INTERFACE_VERSION=0xC004` (WDDM 2.7) in the `.vcxproj` PreprocessorDefinitions (kernel analogue of the IddCx **F1** gate).
  2. **Async-present TDR** — the sample's first present takes the async path needing a HW present-progress interrupt the stdvga lacks → "stopped responding" (problem 43 after present). Fix: force `m_SynchExecution = TRUE` (`blthw.cxx`) → synchronous CPU blt, `STATUS_SUCCESS`, no interrupt.
  - **✅ COMPLETE (2026-06-20) — dynamic resolution + panel-aware tiling, stable.** The KMDOD now follows the dom0 window **above and below 1080p** (fixed-max contiguous primary via a fallback chain; `bdd_dmm.cxx` advertises a mode grid up to it), and **maximize *and* half-tile respect the xfce4-panel** like the Win7/qvideo qube. User-confirmed working + crash-free. The route to panel-awareness: a `DxgkDdiEscape` + corrector gave true 0px-exact but its per-change **hotplug froze the qube** (DXGI `ACCESS_LOST` churn → WARP/vchan freeze); the **stable** replacement pre-enumerates **8px-stepped heights near each monitor's full height at full + half widths** (no hotplug, ~4px of the work area, biased under so the panel stays visible). See `docs/findings.md`.
  - **Next:** the `XcGnttab` grant path (M2-K, bypass DXGI capture); optional 0px-exact via CCD `SetDisplayConfig` (the escape DDI + `um/` tools are kept dormant). KMDOD source stays out of this GPL repo (MS-PL); only the recipe is committed.
- **⛔ FINAL VERDICT (2026-06) — SUPERSEDED, see above.** Kept for the record. ~~a custom WDDM display driver is NOT achievable on this Qubes/Xen Win10 HVM.~~ All four routes were *thought* walled off, see `docs/findings.md` "FINAL VERDICT":
  1. **IddCx** — UMDF host can't bind the IddCx class extension here (won't load).
  2. **KMDOD on stdvga** — loads + StartDevice OK, then **dxgkrnl silently rejects the adapter** (`problem=43`, Problem Status `0x0`, after QueryAdapterInfo, before any VidPn DDI). The stdvga is a plain VGA device, not a GPU; the **stock unmodified MS KMDOD sample fails identically** (ETW: `End loading DXGKrnl Status` + PNP[411] problem-starting, status 0). Not our code.
  3. **Live KD** to debug that — no transport: libxl rejects `tcp` serial (domain won't build), no KDNET-capable NIC (RTL8139), pty needs a fragile stubdom multi-hop.
  4. **Different virtual GPU** — Qubes `video-model` feature exists, but libxl supports only stdvga/cirrus: `qxl` needs SPICE (absent in Qubes), `virtio` → *"video type virtio is not supported by libxl"*.
  - **Root cause:** the platform gives Windows only a basic VGA device; Windows won't host a real WDDM adapter on it, and the alternatives (a true virtual GPU / live KD) aren't available in this Qubes/Xen build.
  - **Unblock condition (future):** virtio-gpu support in Xen/libxl (2025 upstream patch) → then `viogpudo` + a grant path becomes viable. Until then, the agent's existing DXGI-capture-of-BasicDisplay path is the only workable approach.
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
