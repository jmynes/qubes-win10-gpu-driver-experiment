# qubes-win10-gpu-driver-experiment

A Windows-10 IddCx indirect display driver for Qubes OS — a lab notebook and toolkit for building a custom virtual display driver that gives Windows HVM qubes **dynamic resolution** plus a **native grant-based frame path** (Windows `XcGnttab` grants → `MSG_SHMIMAGE` over the qubes-gui vchan), bypassing the emulated qemu-stubdom GUI pipeline.

**Status:** M0a (build toolchain + skeleton build) **DONE** · M0b feasibility gate / risk R1 **DONE = GO** · M1 (fork LGIdd, strip, build, install, confirm a virtual monitor enumerates) **NEXT, not started**.

---

## What this is / why

`qubes-gui-agent-windows` carries a long-standing TODO: a custom WDDM/display driver so Windows qubes get a first-class GUI integration instead of riding the emulated stub-domain framebuffer. This repo is the effort to close that TODO with a modern **IddCx UMDF** (User-Mode Driver Framework) indirect display driver, which is far simpler to build and ship than a kernel WDDM miniport.

Two concrete wins are the goal:

- **Dynamic resolution** — the virtual monitor resizes to follow the dom0 window instead of being pinned to a fixed emulated mode.
- **Native grant frame path** — each frame is copied into a grant-shareable system-RAM buffer, those pages are granted via `XcGnttab`, and the gui-agent ships a `MSG_SHMIMAGE` over the existing qubes-gui vchan. This removes the qemu-stubdom emulated-GUI hop from the frame pipeline.

### Honesty caveat

This driver does **NOT** fix the separate **>2-vCPU redraw corruption** (desktop fragments / redraw trails on Windows HVM qubes with more than two vCPUs). That is an unrelated guest DWM/WARP software-compositor SMP race, already worked around by `bcdedit /set groupsize 2` (the "groupsize-2" fix). That fix lives in a **different repo** (the `qubes-fiddling` ansible) and is out of scope here. The two efforts are independent.

---

## Headline findings

Full detail and provenance in [`docs/findings.md`](docs/findings.md).

- **F1 — Windows 10 is an IddCx 1.5 ceiling.** Win10 2004 through 22H2 (builds 19041 through 19045, including 22H2 / 19045) ship IddCx 1.5 (`IddCxGetVersion` 0x1500); IddCx 1.4 was the ceiling only on the older Win10 18362/18363 (19H1/19H2). IddCx 1.5 is itself a Windows 10 version — the first Windows-11-only IddCx is **1.8** (Win11 21H2 / 22000 = 1.8, 22H2 / 22621 = 1.9, 23H2 / 22631 = 1.10). Proof: a driver built against IddCx 1.10 (effective client 1.10 > framework 1.5) fails to load with **device problem code 31 / UMDF host error `0xD000000D`**. (`IndirectKmd.sys` `10.0.19041.1` only indicates the 2004 servicing branch — which ships 1.5 — and is not by itself proof of any 1.x API level.) The driver must compile against newer headers but keep its effective client version **≤ 1.5** (set a low `IDDCX_MINIMUM_VERSION_REQUIRED` — the minimum IddCx version the driver *requires*, lower = installs on more OSes — and runtime-guard any >1.5 API via `IDD_IS_FUNCTION_AVAILABLE` / `IDD_IS_FIELD_AVAILABLE`).
- **F2 — MttVDD (Virtual-Display-Driver "HDR" edition) is structurally Win11-only.** It uses `IDDCX_MONITOR_MODE2.BitsPerComponent` (IddCx 1.10+ HDR/Mode2), so it requires IddCx 1.10 and cannot load on Win10 (which caps at 1.5). Abandoned as the fork base.
- **F3 — Risk R1 resolved = GO.** Looking Glass's `LGIdd` acquires each frame as a D3D **texture** (on the IddCx 1.10 Buffer2 acquire path it passes `AcquireSystemMemoryBuffer = FALSE`; the legacy acquire just returns a texture with no such option), then GPU-copies it (dirty-rect aware) into a CPU-accessible buffer. `LGIdd`'s `CFrameBufferResource` is *either* a GPU-shared placed resource on a cross-adapter heap (fast path: the GPU copies straight in and completion just advances the write pointer via `FinalizeFrameBuffer`) *or* a CPU-mapped readback buffer needing one extra CPU copy (`WriteFrameBuffer`) — selected by `IsIndirectCopy()`. So the proven Win10 path is **texture → GPU-copy → CPU buffer**, which we copy into a grant-shareable buffer. **Fork base = `LGIdd`** (gnif/LookingGlass, GPL-2.0-or-later) per **F4**.

---

## Repo layout

```
qubes-win10-gpu-driver-experiment/
├── README.md                      # this file
├── LICENSE                        # GPL-2.0-or-later
├── .gitignore
├── toolchain/
│   ├── Directory.Build.props      # WDK-NuGet glue (authoritative — do not rewrite)
│   └── Directory.Build.targets    # late-import fixes (trailing-slash, include roots, stub libs)
├── docs/
│   ├── plan.md                    # the M0..M5 milestone plan
│   ├── findings.md                # F1..F4 technical findings (detail)
│   ├── build-toolchain.md         # how the Build-Tools + WDK-NuGet build works
│   └── install-and-debug.md       # package / sign / install / debug recipe
└── scripts/
    ├── deploy-to-guest.sh         # mgmtvm → guest file push over qrexec
    ├── guest-setup-buildtools.ps1 # install VS Build Tools + ATL in the guest
    ├── guest-build.ps1            # restore + build the .vcxproj in the guest
    └── guest-package-install.ps1  # stamp INF, sign, pnputil/devcon install
```

This repo is the **source of truth** on the Qubes mgmtvm: edit here (git-versioned), deploy to the Windows guest over qrexec, build in the guest. (Previously the build glue was edited ad-hoc inside the guest, which was fragile; this repo fixes that.)

---

## Dev loop / quick-start

The dev host is the Qubes mgmtvm (AdminVM). The Windows build+test guest is the qube **`romhacking-hma-driver`** (Win10 Pro 22H2 / build 19045; Qubes Windows Tools installed; `bcdedit /set groupsize 2` set; test-signing ON).

```bash
# 1. Edit sources + toolchain glue here, on mgmtvm (git-versioned).

# 2. Push this repo into the guest build dir (C:\dev\LGIdd) over qrexec:
#    args: <guest-qube> <local-dir> <guest-dest-dir>
./scripts/deploy-to-guest.sh romhacking-hma-driver ./toolchain "C:\dev\LGIdd"
```

```powershell
# 3. In the guest (romhacking-hma-driver), restore + build:
guest-build.ps1 -ProjDir "C:\dev\LGIdd" -VcxProj "LGIdd.vcxproj"

# 4. Package, sign, and install the driver, then create the device node:
guest-package-install.ps1 -ProjDir "C:\dev\LGIdd" -RelDir "x64\Release" -Inf "LGIdd.inf" -Hwid "Root\LGIdd"
```

Scripts drive the guest via base64-encoded `-EncodedCommand` PowerShell over `qvm-run --pass-io --no-gui` (the encoding avoids all quoting/length problems). See [`docs/install-and-debug.md`](docs/install-and-debug.md) for the qrexec file-push/pull mechanics.

The toolchain itself — building an IddCx UMDF driver with **VS Build Tools + WDK-via-NuGet** (no EWDK, no WDK Visual Studio extension) — is the novel M0a achievement; it took ~13 build iterations to peel toolset → legacy resolver → include roots → WDF header version → ATL → 3 stub libs. The full chain is in [`docs/build-toolchain.md`](docs/build-toolchain.md).

---

## Status / roadmap

| Milestone | What | State |
|-----------|------|-------|
| **M0a** | Build toolchain (Build Tools + WDK NuGet) + skeleton build | **DONE** — MttVDD skeleton built to `MttVDD.dll` (746 KB) |
| **M0b** | Feasibility gate / risk R1 (CPU buffer vs GPU texture?) | **DONE = GO** (F3) |
| **M1** | Fork `LGIdd`, strip IVSHMEM/LGMP/pipe, build, install, confirm a virtual monitor enumerates | **NEXT — not started** |
| **M2** | First static frame to dom0 over the grant path | planned |
| **M3** | Live frames (prototype-complete) | planned |
| **M4** | Dynamic resolution follows the dom0 window | planned |
| **M5** | Cleanup + upstreamable PR to `qubes-gui-agent-windows` | planned |

Full milestone plan: [`docs/plan.md`](docs/plan.md).

---

## License

**GPL-2.0-or-later** (see [`LICENSE`](LICENSE)). This work forks Looking Glass's `LGIdd` (gnif/LookingGlass, GPL-2.0-or-later); the copyleft is inherited from that base.
