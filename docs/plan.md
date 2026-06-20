# Build the Win10 Qubes virtual display driver (close the qubes-gui-agent-windows "TODO: custom WDDM driver")

> **Note (written before M0; superseded on two points — see [`findings.md`](findings.md)):** This plan predates the first build milestone. Two decisions changed: **(1) toolchain** — the build now uses **VS Build Tools + the WDK via NuGet**, not EWDK/qubes-builderv2; **(2) fork base** — we fork **LookingGlass `LGIdd`**, not MttVDD (MttVDD requires **IddCx 1.10** and is therefore Win11-only, so it can't load on Win10). The plan's **IddCx version is right** and stands: Win10 22H2 ships **IddCx 1.5**.

## Context

`qubes-gui-agent-windows` has shipped no Windows 10 display driver — the only one Qubes ever had is the Win7 **XDDM `qvideo`** (release4.0 branch), and XDDM was removed in Win8 so it can't be ported. On Win10 the agent is a pure **user-mode capture** program (`gui-agent/capture.c`, DXGI Desktop Duplication) hard-wired to the Microsoft Basic Display Adapter. The README lists `TODO: custom WDDM driver (maybe some time in the future)` — no stub, branch, or PR exists.

We want to build it as a **full, upstreamable driver**. It delivers three things: **(a) dynamic resolution** (guest desktop follows the dom0 window at the exact size, restoring Win7/qvideo parity the MBDA path only approximates); **(b) a native frame path** that ships frames agent→gui-daemon over the qubes-gui grant/`MSG_SHMIMAGE` vchan, **bypassing the emulated qemu-stubdom GUI pipeline** whose vchan drops `libvchan_is_eof` under load (the freeze, #9847/#10932) — a real but *partial* mitigation; **(c)** closes the upstream TODO.

**It does NOT fix the >2-vCPU redraw corruption** — that's a Win10 DWM/WARP software-composition SMP race, already solved by `bcdedit /set groupsize 2`, which must stay set. Win10 forces DWM+WARP regardless of display driver, so this driver leaves the redraw race unchanged. This caveat is a mandatory banner in the eventual PR.

**Test VM:** `romhacking-hma-driver` (existing Win10 22H2 clone, QWT installed, `groupsize 2` set). Built via the existing `qubes-builder-lab` (model the component config on `~/builder-stubdom.yml`). Full design detail in `warm-nibbling-origami-agent-a13098166346816a0.md`.

## Approach: IddCx Indirect Display Driver (UMDF, user-mode)

**Chosen: IddCx** (Indirect Display Driver, UMDF 2.x, Session 0), headers pinned to Win10 22H2 = **IddCx 1.5**, with runtime `IddCxGetVersion` detection for 1.6+ paths.

Why IddCx over KMDOD/full-WDDM:
- Only model that runs **GPU-less with no invented PCI/GPU device** (root-enumerated, software-only). The MS KMDOD sample borrows a POST/VESA framebuffer this HVM lacks **and** is MS-PL (GPL-2-incompatible — can't vendor into the agent). Full WDDM needs a fabricated GPU.
- **User-mode is the decisive win:** the Qubes Xen grant API on Windows (`XcGnttab*` over `xeniface`) is a **user-mode** call — the exact call `capture.c:385` already makes — so the UMDF driver grants pages directly with **no kernel grant code**, and a driver bug doesn't bugcheck the qube (reboot-light dev loop). Test-signing only (no SecureBoot).

**Fallback (documented, not started):** an *own-framebuffer* KMDOD (allocates its own system-RAM primary à la `qxl-dod`/`viogpudo`, not the POST-borrowing MS sample) — only if the M0 experiment proves IddCx can't yield a usable CPU frame.

## Integration design (report-verified against source)

**Frame path — keep the transport, swap the source.** The existing chain is buffer-source-agnostic and stays **verbatim**:
```
XcGnttabPermitForeignAccess2 (capture.c:385) → SendScreenGrants (main.c:1153)
  → MSG_WINDOW_DUMP / WINDOW_DUMP_TYPE_GRANT_REFS (send.c:40) → per-dirty-rect MSG_SHMIMAGE
```
Only the **frame source** changes: the IddCx driver allocates its primary in **page-aligned grant-shareable system RAM**, copies each composited frame in, **grants those pages itself** (same `XcGnttabPermitForeignAccess2`), and hands grant-refs + WxH + dirty-rects to the agent over a private **IOCTL**. (Page count safely < `MAX_GRANT_REFS_COUNT`.) Grant-ownership split — **A1 driver-grants/agent-ships** (preferred) vs A2 shared-section — decided empirically in M2/M3.

**Dynamic resolution.** The agent chain `MSG_CONFIGURE → HandleConfigure → RequestResolutionChange → SetVideoMode → SelectSupportedMode → ChangeDisplaySettings` is ~90% there. **Verified gap:** `resolution.c SelectSupportedMode` (line 118) only picks an existing OS-enumerated mode and filters out `> g_HostScreen*` (line 130), so exact follow is impossible with a fixed mode list. Fix: driver advertises arbitrary WxH as a real mode (`IDDCX_ADAPTER_FLAGS_ALL_TARGET_MODES_MONITOR_COMPATIBLE` + `IddCxMonitorUpdateModes`); agent calls a new `DriverSetPreferredMode(w,h)` IOCTL in `SetVideoMode` (resolution.c:156) **before** selection. Re-grant on mode change reuses the existing `case 5` capture-error re-init (main.c:1400) which already re-sends grants.

**Minimal agent change** (behind a `qvm-feature` flag; DXGI stays the default fallback):
1. New backend `gui-agent/capture-idd.c` — opens the IDD device, gets grant-refs by IOCTL instead of DXGI. The `DesktopImageInSystemMemory` gate at `capture.c:176-183` and `MapDesktopSurface` at `capture.c:372` are **not on this path** (sidestepped, not "relaxed"). `CAPTURE_CONTEXT`, `SendScreenGrants`, dirty-rect→`MSG_SHMIMAGE`, teardown stay identical.
2. `DriverSetPreferredMode(w,h)` IOCTL hook in `resolution.c SetVideoMode` (line 156).
3. Backend selector in `StartFrameProcessing`/`CaptureInitialize` (main.c:1135), DXGI fallback if IDD absent.

## Skeleton to fork

- **Fork base: `VirtualDrivers/Virtual-Display-Driver` (MttVDD)** — MIT (GPL-2-shippable), full source, IddCx, EDID/mode-list config (dynamic resolution nearly free). Bare MS `video/IndirectDisplay` sample is the minimal baseline underneath if you want the smallest auditable start.
- **Architecture reference (study, port selectively): `gnif/LookingGlass` LGIdd** — already IddCx→copy swap-chain frames→shared-memory sink + resolution helper; the Qubes port swaps its IVSHMEM sink for `XcGnttab*` and its pipe helper for the gui-agent. **LGIdd already solved the per-frame copy-out incl. a staging fallback** (= risk R1).
- **KEEP:** IddCx adapter/monitor lifecycle + `EVT_IDD_CX_*`, mode-list/EDID plumbing, the swap-chain frame-extraction thread, root-enum INF, `Direct3DDevice` DXGI enumeration pointed at **WARP** (Basic Render Driver).
- **RIP OUT:** the no-op present path, the GUI control app, HDR/multimon extras, LGIdd's IVSHMEM + pipe.
- **ADD:** grant-shareable system-RAM primary + per-dirty-rect copy, `XcGnttab*` grant/revoke, the IOCTL surface, mode list driven by the agent.
- **Reject:** MS `video/KMDOD` (POST framebuffer + MS-PL), `parsec-vdd` (closed binary). Study release4.0 `qvideo/` only for the control-IOCTL/Display-class-INF *patterns* (don't port XDDM code).

## Critical files

**New driver subtree** `display-driver/` + `vs2022/qubes-idd/` (`Driver.cpp`, `qubes-idd.inf`, `qubes-idd.vcxproj`, Package project → `.sys/.inf/.cat`). Link `IddCx` + `xencontrol` (already cross-built as `qubes-windows-tools-cross/VS2019/xencontrol.vcxproj`; `xeniface-8.2.2`).
**New agent files:** `gui-agent/capture-idd.c` (+ small `driver-ctl.c` IOCTL wrapper).
**Modified agent files:** `gui-agent/resolution.c` (`SetVideoMode` ~line 156 + relax the `>host` filter at line 130 for driver modes), `gui-agent/main.c` (backend selector at ~1135). `capture.c`/`send.c` unchanged.
**Build wiring:** add the driver to `vs2022/gui-agent-windows.sln` (or its own `.sln`); add artifacts to `.qubesbuilder` `vm.windows.build`/`vm.windows.bin`; add a `gui-agent-windows` component entry to the builder config (model on `~/builder-stubdom.yml`).

## Build / sign / dev loop

- **EWDK ≥ 10.0.19041** (IddCx 1.5 floor), driven by `qubes-builderv2`'s `build_windows` plugin exactly as the agent builds today (`build.cmd` → `build-sln.ps1 -testsign`). Build a Universal UMDF driver (`PlatformToolset v143`, `TargetPlatformVersion 10.0`).
- **Signing = test-signing only** (no SecureBoot): build side `-testsign` self-signs `.sys/.cat`; guest side `bcdedit /set testsigning on` + `certutil -addstore -f TrustedPublisher qubes-idd.cer` — the same mechanism `auto-qwt/trust-certificates.bat` already uses.
- **Dev loop:** build on the lab qube → stream `.sys/.inf/.cat` + new `gui-agent.exe` into **`romhacking-hma-driver`** via qrexec → `pnputil /add-driver qubes-idd.inf /install` → debug via DbgPrint/WinDbg over a Qubes serial/named pipe (log `IddCxGetVersion` + system-buffer result at D0 entry) → restart the gui-agent service on agent changes.

## Milestones (each with DONE criterion)

- **M0 — toolchain + skeleton + FEASIBILITY GATE** *(go/no-go on the whole approach).* Build the **unmodified** MttVDD/MS sample, test-sign, install on `romhacking-hma-driver`; confirm a WARP adapter + virtual monitor enumerate; **measure** whether the frame arrives as a CPU system buffer (`IddCxSwapChainInSystemMemory`) or only a D3D texture needing a WARP staging copy. **DONE:** monitor in Device Manager + a *measured* answer to R1. **Gate:** usable CPU frame → proceed on IddCx; else → KMDOD fallback.
- **M1 — Qubes IDD installs at a fixed mode.** Fork into `display-driver/`, wire into `.sln`/`.qubesbuilder`/builder, re-INF, build+sign. **DONE:** test-signed Qubes IDD installs via qrexec→pnputil, shows in Device Manager; agent still on DXGI.
- **M2 — first STATIC frame to dom0.** Copy committed frame → grantable primary → grant → IOCTL → `capture-idd.c` relays refs. In-guest PNG dump first for pixel correctness. **DONE:** one correct static IDD frame in the dom0 gui-daemon window (right size/stride/format).
- **M3 — live frames.** Per-frame copy + dirty-rect → `MSG_SHMIMAGE`, steady-state acquire/grant/ship, DXGI disabled. **DONE:** live Win10 desktop updating in the dom0 window via the IDD path at interactive rate. *(Natural "prototype, stop here if not worth it" point — the thesis is proven.)*
- **M4 — dynamic resolution.** Driver advertises arbitrary modes + `DriverSetPreferredMode` hook + re-grant on change. **DONE:** resizing the dom0 window snaps the guest to the **exact** matching resolution, frames continuing.
- **M5 — cleanup + upstreamable PR.** Cursor→qubes-gui cursor msgs, hot-plug, default-OFF behind the `qvm-feature`, DXGI fallback, **soak under `groupsize 2` with no regression**, license headers. **DONE:** clean PR vs `qubes-gui-agent-windows` closing the TODO, with the honesty banner.

## Risks

- **R1 (biggest) — does IddCx hand system RAM or a GPU texture?** Reports disagree (doc says system-buffer API is "Server 2022 min" but the table maps 1.5→22H2 client). **M0 measures it on the real HVM.** Both paths coded: happy path = direct system-buffer; fallback = `CopyResource`→`STAGING`(CPU_READ)→`Map` (LGIdd's proven path; one extra software copy/frame, structurally identical to today's DXGI readback).
- **R2 — Session-0 surface egress.** De-risk: grant *inside the driver* (A1) so only small grant-ref arrays cross via IOCTL.
- **R3 — exact-resolution follow** (the `SelectSupportedMode` filter). De-risk: driver-advertised exact mode + the `DriverSetPreferredMode` hook (M4).
- **R4 — IddCx version on 22H2 client.** De-risk: runtime `IddCxGetVersion` branch; never hard-require 1.6+ without a 1.5 fallback.
- **R6 — per-frame copy/grant perf.** De-risk: dirty-rects only; persistent granted primary, re-grant only on resize; measure at M3.
- **R7 — license boundary.** De-risk: self-contained `display-driver/` subtree, MIT notice + GPL-2 headers; never vendor MS-PL.

## Verification (on `romhacking-hma-driver`, `groupsize 2` kept throughout)

- **M0:** unmodified sample installs; Device Manager shows the virtual monitor; WinDbg log shows the WARP adapter and a definitive system-buffer-vs-texture answer.
- **M1:** Qubes IDD installs via qrexec→pnputil, in Device Manager, no bugcheck, agent still on DXGI.
- **M2:** in-guest PNG of the IDD primary pixel-correct; dom0 window shows one correct static IDD frame.
- **M3:** live desktop updating in dom0 over the IDD path (DXGI disabled); typing/window-moves prompt; no grant leaks over a multi-minute soak (`XcGnttabRevokeForeignAccess` balanced).
- **M4:** drag-resize the dom0 window → guest snaps to exact WxH across several cycles; `ChangeDisplaySettings` succeeds on the synthesized mode.
- **M5:** multi-hour soak under `groupsize 2`, no new instability; cursor correct; clean DXGI fallback when the IDD is removed; default-OFF until the `qvm-feature` is set.

## Effort

~**3–4 months focused** for a full upstreamable result (M0 1–2wk, M1 1–2wk, M2 2–3wk, M3 ~2wk, M4 ~2wk, M5 2–3wk); ~double for a first-time WDDM/IddCx dev; +~1 month if forced onto the KMDOD fallback. A bare PoC through M3–M4 with clean reuse is the short end (~6–10 weeks).
