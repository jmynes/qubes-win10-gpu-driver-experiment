# qubes-win10-gpu-driver-experiment

A **working** Windows-10 WDDM **display-only (KMDOD)** driver for Qubes OS HVM qubes — a lab
notebook and toolkit that closes the long-standing `qubes-gui-agent-windows` *"TODO: custom WDDM
driver"*. The guest desktop gets **dynamic resolution that follows the dom0 window** (above and below
1080p) with **panel-aware maximize and tiling**, on the emulated QEMU stdvga that the project had
previously written off as incapable of hosting a real WDDM adapter.

> **Status (2026-06-20): IT WORKS.** A custom kernel-mode WDDM display-only miniport is the live
> active display on a Qubes/Xen Win10 22H2 HVM (`CM_PROB_NONE`): it loads, starts, commits a VidPn,
> presents continuously, follows the dom0 window's size **above and below 1080p**, and its
> **maximize and half-tile (XFCE Super+↑ / Super+←→) respect the xfce4-panel** like the Win7/qvideo
> qube — **stable, no freeze**. User-confirmed. This **overturns** the earlier "FINAL VERDICT" that a
> custom WDDM display driver is not achievable on this platform.
>
> Remaining: **M2-K** — the native `XcGnttab` grant frame-path (bypass the gui-agent's DXGI capture).

---

## The headline: the "impossible" verdict was wrong

An earlier investigation concluded *"a custom WDDM display driver is NOT achievable on this Qubes/Xen
Win10 HVM"* — the MS `KMDOD` sample loaded and started but `dxgkrnl` rejected the adapter with device
**problem 43**, and the conclusion was that the QEMU stdvga is *"a plain VGA device, not a GPU"* so
Windows won't host a WDDM adapter on it.

**That root cause was wrong.** The smoking gun: the inbox **`BasicDisplay.sys` is itself a WDDM
kernel display-only miniport, and it runs fine on this exact stdvga** — so the platform clearly *can*
host one. `problem 43` was two **newer-WDK-vs-older-OS build bugs**, both fixable:

1. **DDI / struct-size version gate.** Built with the Win11-24H2 EWDK, `DXGKDDI_INTERFACE_VERSION`
   defaults to `WDDM3_2` (`0x11007`), so `DXGK_DRIVERCAPS` compiles at the larger Win11 layout; Win10
   19045's `dxgkrnl` passes a *smaller* `OutputDataSize`, so the sample's
   `QueryAdapterInfo(DRIVERCAPS)` `OutputDataSize < sizeof(...)` guard rejects → adapter fails
   **before any VidPn DDI** (the exact observed signature). *Fix:* pin
   `DXGKDDI_INTERFACE_VERSION=0xC004` (WDDM 2.7, this OS's `dxgkrnl` ABI) in the `.vcxproj`. This is
   the kernel-mode analogue of the IddCx client-version gate (**F1**) the prior work had already
   proven — but never applied to the kernel driver.
2. **Async-present TDR.** The sample's first present takes an async path that needs a hardware
   present-progress interrupt the emulated stdvga lacks → *"display driver stopped responding"*
   (`problem 43` after the first present). *Fix:* force synchronous present (`m_SynchExecution = TRUE`
   in `blthw.cxx`) → a synchronous CPU blt that returns `STATUS_SUCCESS`, no interrupt dependency.

Both stock and modified KMDOD failed identically *because both were built with the same too-new EWDK*
— a fact the prior verdict read as "it's the platform," when it was the toolchain.

Full evidence, the live `QbLog` DDI traces, and every elimination: [`docs/findings.md`](docs/findings.md).

---

## What works, and how

The driver is a **kernel-mode WDDM display-only miniport** derived from the Microsoft `video/KMDOD`
sample. It owns its own **page-aligned, grant-shareable system-RAM primary** (a fixed-max contiguous
buffer); the OS composites the desktop into it via `DxgkDdiPresentDisplayOnly`. The existing
`qubes-gui-agent-windows` captures that desktop with **DXGI Desktop Duplication** (unchanged) and
ships it to dom0 — **no gui-agent rebuild was needed**.

- **Dynamic resolution, above & below 1080p.** The primary is allocated once at the largest size a
  contiguous allocation succeeds for (`{3840×2160, 2560×1600, 2560×1440, 1920×1200, 1920×1080}`
  fallback chain); `bdd_dmm.cxx` advertises a mode grid up to that cap. The gui-agent's existing
  `RequestResolutionChange → SelectSupportedMode → ChangeDisplaySettings` path then snaps the guest
  to the dom0 window size — which previously couldn't grow past the old fixed 1920×1080 primary.
- **Panel-aware maximize & tiling.** A maximize/tile requests *full-or-half width × (full height −
  panel)*, so the driver pre-enumerates **8px-stepped heights near each monitor's full height, at
  full *and* half monitor widths** (`QbMon[]` in `bdd_dmm.cxx`). The gui-agent's own snap then lands
  **within ~4px** of the work area (biased *under* → the xfce4-panel stays visible), matching the
  Win7/qvideo qube.

### The one that got away (kept dormant)

A `DxgkDdiEscape` + a tiny user-mode corrector (in [`um/`](um/)) gave **true 0px-exact** resolution
*without* rebuilding the gui-agent — but `SetPreferredMode`'s per-change synthetic monitor **hotplug**
(`DxgkCbIndicateChildStatus` FALSE→TRUE) makes the gui-agent's DXGI duplication lose its surface
(`DXGI_ERROR_ACCESS_LOST`) and re-init, which trips the known WARP/vchan freeze after ~3 changes
(needs `qvm-kill`). And *without* the hotplug, `ChangeDisplaySettings` rejects any non-enumerated
mode (`DISP_CHANGE_BADMODE`) — so per-pixel exactness and stability are mutually exclusive via that
API on this stack. The escape DDI and the `um/` tools are kept **dormant**; the stable fine-height
grid is the shipped answer. (A 0px-exact-*and*-stable path may exist via the CCD `SetDisplayConfig`
API — unverified; see `docs/findings.md`.)

---

## Honesty caveats

- This driver does **NOT** fix the separate **>2-vCPU redraw corruption** (a guest DWM/WARP
  software-compositor SMP race). That is worked around by `bcdedit /set groupsize 2`, lives in a
  different repo (the `qubes-fiddling` ansible), and is out of scope here. Keep `groupsize 2` set.
  **Empirically confirmed (2026-06-20):** with the working KMDOD active on a 16-vCPU qube, flipping
  `groupsize` off + rebooting brings the corruption right back — our display-*only* driver doesn't
  touch the WARP render path, so the race is unchanged. (Different bug from the vchan freeze that
  M2-K partially mitigates.)
- The **driver binary's source is the MS `video/KMDOD` sample (MS-PL)** and therefore stays **out of
  this GPL repo** — only the *recipe* (the exact changes + build/install mechanics) is committed, in
  `docs/findings.md`. The working source lives on the build/test guest (`C:\dev\kmdod`). The original
  GPL-2 code here is the `um/` escape tooling.
- The currently-running build is **hot-swapped on a disposable Win10 clone** for fast iteration; an
  upstreamable Qubes driver would be original GPL-2 code (studying the MS sample plus
  `qxl-wddm-dod`/`viogpudo`) wired into `qubes-gui-agent-windows`.

---

## Repo layout

```
qubes-win10-gpu-driver-experiment/
├── README.md                      # this file
├── CLAUDE.md                      # operational orientation (read first)
├── LICENSE                        # GPL-2.0-or-later
├── docs/
│   ├── findings.md                # THE record: the breakthrough, the recipe, every fix + trace
│   ├── plan.md                    # the original (IddCx-era) milestone plan — partly superseded
│   ├── m1-strip-plan.md           # LGIdd strip plan (IddCx route)
│   ├── build-toolchain.md         # VS Build Tools + WDK-NuGet build glue (IddCx route)
│   └── install-and-debug.md       # package / sign / install / debug recipe + qrexec mechanics
├── um/                            # ORIGINAL GPL-2 user-mode tooling for the (dormant) escape path
│   ├── qb_escape.h                # shared UM<->KM escape struct
│   ├── set-res.c                  # one-shot exact-resolution test tool (D3DKMTEscape)
│   └── qb-resd.c                  # log-tailing resolution corrector daemon
├── driver/                        # LGIdd IddCx fork — the ABANDONED route, kept as the reproduction
├── driver-mssample/               # MS IddCx sample baseline (the IddCx-won't-load endpoint)
├── toolchain/                     # Directory.Build.{props,targets} — WDK-NuGet glue (IddCx route)
└── scripts/
    ├── deploy-to-guest.sh         # mgmtvm → guest byte-faithful file push over qrexec (base64-stdin)
    ├── guest-build.ps1            # build a .vcxproj in the guest
    ├── guest-package-install.ps1  # stamp INF, sign, pnputil/devcon install
    └── guest-setup-buildtools.ps1 # install VS Build Tools in the guest
```

This repo is the **source of truth** on the Qubes mgmtvm: author + `git` here, deploy to the Windows
guest over qrexec, build in the guest.

---

## Toolchain & dev loop

The driver that actually builds + loads here needs the **EWDK 28000** (`WindowsKernelModeDriver10.0`
toolset, mounted ISO) — *not* the v143 + WDK-NuGet path (that only compiles, and was for the
abandoned IddCx route). The `um/` tools build with `cl` + the EWDK's UM SDK headers/libs.

- **Dev host:** the Qubes **mgmtvm** (AdminVM).
- **Build/test guest:** a Win10 Pro 22H2 / 19045 qube with Qubes Windows Tools, `groupsize 2`, and
  test-signing ON. Build under `C:\dev\…`. Run guest PowerShell via base64 UTF-16LE
  `-EncodedCommand` over `qvm-run --pass-io --no-gui <qube>` (avoids all quoting/length problems).

```bash
# push a file/dir into the guest over qrexec (byte-faithful, base64-on-stdin)
./scripts/deploy-to-guest.sh <guest-qube> ./<dir> "C:\dev\..."
```

Install gotchas, signing (`/sm` machine store; cert into Root + TrustedPublisher), the `pnputil
/disable-device → swap .sys → /enable-device` hot-reload loop, and the `QbLog` registry DDI tracer
are documented in [`docs/install-and-debug.md`](docs/install-and-debug.md) and the findings.

---

## Status / roadmap

| Milestone | What | State |
|-----------|------|-------|
| IddCx route | UMDF indirect display driver | **ABANDONED** — the UMDF host can't bind the IddCx class extension on this HVM (won't load); kept as `driver/` + `driver-mssample/` |
| **KMDOD load** | kernel WDDM display-only miniport loads + starts + presents | **✅ DONE** — two build-bug fixes (DDI version pin + sync present) cleared `problem 43`; overturns the "FINAL VERDICT" |
| **Dynamic resolution** | guest follows the dom0 window, above & below 1080p | **✅ DONE** |
| **Panel-aware** | maximize *and* half-tile respect the xfce4-panel, stable | **✅ DONE** (user-confirmed) |
| **M2-K** | native `XcGnttab` grant frame-path (bypass DXGI capture) | **NEXT** |
| Upstream | original GPL-2 driver + PR to `qubes-gui-agent-windows` | planned |

> Note: [`docs/plan.md`](docs/plan.md) predates this and is written around the IddCx route; treat
> `docs/findings.md` as the authoritative current record.

---

## License

**GPL-2.0-or-later** (see [`LICENSE`](LICENSE)). The `um/` tooling and any future driver code here are
original GPL-2. The KMDOD reference source is Microsoft's MS-PL sample and is **not** vendored — only
its recipe is documented.
