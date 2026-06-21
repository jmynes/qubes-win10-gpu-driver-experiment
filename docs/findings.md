# Findings

The load-bearing technical discoveries from M0 (toolchain bring-up + feasibility
gate). These are the facts that constrain every later milestone. Each was
discovered empirically on the test guest `romhacking-hma-driver` (Windows 10 Pro
22H2, build 19045) or read directly from upstream source on the mgmtvm
reference clones.

Sibling docs:

- Build glue and the toolchain fixes: [`build-toolchain.md`](build-toolchain.md)
- Package / sign / install recipe and its gotchas: [`install-and-debug.md`](install-and-debug.md)
- Milestone plan: [`plan.md`](plan.md)

> **Out of scope here:** the separate `>2`-vCPU redraw corruption is a Win10
> DWM/WARP software-compositor SMP race, fixed by `bcdedit /set groupsize 2` in
> the `qubes-fiddling` ansible repo. It is unrelated to the display driver and
> this driver does **not** change it (Win10 forces DWM+WARP regardless of display
> driver). Keep `groupsize 2` set throughout.

---

## ✅ BREAKTHROUGH (2026-06-20) — a custom WDDM display-only driver **DOES** work here; the "FINAL VERDICT" is OVERTURNED

The earlier "⛔ FINAL VERDICT" (further down) concluded a custom WDDM display
driver is **not achievable** on this Qubes/Xen Win10 HVM. **That is wrong.** On a
fresh clone (`romhacking-hma-driver-clone2`, Win10 19045.7417) the KMDOD now:

- **loads, runs DriverEntry/AddDevice/StartDevice**, passes **QueryAdapterInfo/DRIVERCAPS**,
  negotiates the full **VidPn** (`IsSupportedVidPn` → `EnumVidPnCofuncModality` →
  `CommitVidPn`), and **presents continuously** (`PresentDisplayOnly` at steady state);
- is the **active display adapter** — Device Manager `CM_PROB_NONE`, desktop at **1920×1080**,
  `Win32_VideoController` = "Kernel mode display only sample driver".

It owns its **own** grant-shareable system-RAM primary (`MmAllocateContiguousMemory`,
`StartDevice`), so the F3/[`plan.md`](plan.md) grant + dynamic-resolution design carries
straight over. The platform was never the wall.

### Why the prior verdict was wrong (the smoking gun)

The inbox **`BasicDisplay.sys` (10.0.19041.3636) + `BasicRender.sys` are themselves a
WDDM kernel display-only miniport, and they run fine on this exact QEMU stdvga**
(`PCI\VEN_1234&DEV_1111`). So the platform *does* host display-only WDDM adapters. The
prior `problem=43` was **not** a platform rejection — it was two newer-WDK-vs-older-OS
bugs, both in the same class as the IddCx **F1** version-gate (which was never applied to
the KMDOD). The stock *and* modified KMDOD failed identically **because both were built
with the same too-new EWDK 28000**.

### Root cause #1 — DDI/struct version mismatch → `DRIVERCAPS` size check rejects the adapter

Built with the Win11-24H2 EWDK (`WindowsTargetPlatformVersion=10.0.28000.0`),
`DXGKDDI_INTERFACE_VERSION` defaults to `DXGKDDI_INTERFACE_VERSION_WDDM3_2` (`0x11007`),
so `DXGK_DRIVERCAPS` compiles at the **larger Win11 layout**. Win10 19041's dxgkrnl passes
`OutputDataSize = sizeof(its own WDDM-2.7 DXGK_DRIVERCAPS)` — **smaller** — so the sample's
`QueryAdapterInfo(DXGKQAITYPE_DRIVERCAPS)` guard
`if (OutputDataSize < sizeof(DXGK_DRIVERCAPS)) return STATUS_BUFFER_TOO_SMALL;` **fails**,
dxgkrnl rejects the adapter → **device problem 43, right after QueryAdapterInfo, before any
VidPn DDI** (exactly the observed/recorded signature). Confirmed live via the `QbLog`
registry tracer: the trace died at the 2nd `QueryAdapterInfo`.

**Fix:** pin the DDI ABI to the OS level. Add to the driver `.vcxproj`
`ClCompile/PreprocessorDefinitions`:

```
DXGKDDI_INTERFACE_VERSION=0xC004      ;# = DXGKDDI_INTERFACE_VERSION_WDDM2_7, this OS's dxgkrnl ABI
```

`d3dukmdt.h` guards its master define with `#if !defined(DXGKDDI_INTERFACE_VERSION)`, so the
predefine **wins** and every version-guarded struct (esp. `DXGK_DRIVERCAPS`) compiles at the
exact layout Win10 passes → the size check passes, the adapter starts. (Numeric values from
the 26100/28000 `d3dukmdt.h`: `WDDM1_3=0x4002`, `WDDM2_0=0x5023`, `WDDM2_7=0xC004`,
`WDDM3_0=0xF003`, `WDDM3_2=0x11007`.) This is the kernel-mode analogue of the IddCx F1
client-version gate — same disease, same cure.

### Root cause #2 — async present path needs a HW present-progress interrupt the stdvga lacks → TDR

With #1 fixed the adapter started and reached `PresentDisplayOnly`, then hit **problem 43
again — but now a TDR**: System log **Event 4113 "Display driver KDODSamp stopped responding
and has been successfully disabled."** The MS sample's `BDD_HWBLT::ExecutePresentDisplayOnly`
**alternates** sync/async (`m_SynchExecution = !m_SynchExecution;`, `blthw.cxx`), so the
**first** present takes the **async worker-thread path**, returns `STATUS_PENDING`, and relies
on `ReportPresentProgress` → `DxgkCbNotifyInterrupt` (a *faked* present-progress interrupt) to
signal completion. The emulated stdvga has **no interrupt path**, so completion never arrives →
dxgkrnl's watchdog times out → TDR.

**Fix:** force synchronous present — `m_SynchExecution = TRUE;` (one line). The sync path does a
straight CPU blt into our framebuffer and returns `STATUS_SUCCESS` (no `ReportPresentProgress`,
no interrupt dependency — see `blthw.cxx` `if (ctx->SynchExecution) { ; }`). This is also exactly
what the Qubes grant path wants (synchronous per-frame copy).

### `QbLog` DDI-trace progression (the empirical proof)

```
before fix #1:  ... QueryChildRelations, QueryAdapterInfo   <-- dies (DRIVERCAPS size check)
after  fix #1:  ... CommitVidPn, SetVidPnSourceVisibility, PresentDisplayOnly  <-- then TDR
after  fix #2:  ... PresentDisplayOnly x N (steady state)   <-- CM_PROB_NONE, active @1920x1080
```

### Mechanics that mattered

- **Build host:** EWDK 28000 ISO mounted at `D:` (`C:\dev\ewdk.iso`); `build-kmdod.cmd`.
- **Binary swap:** `System32\drivers\*.sys` are TrustedInstaller-owned — even elevated admins get
  Access Denied on a direct copy; use `pnputil`, or `takeown`+`icacls`+copy. `pnputil /add-driver
  /install` is a **no-op if the DriverVer isn't newer** ("up-to-date on device").
- **Re-test without a full reboot:** `pnputil /disable-device "<instanceid>"` → swap `.sys` → clear
  `HKLM\SYSTEM\CCS\Services\KDODSamp\Qb` → `pnputil /enable-device` reloads the driver.
- **Signing:** the EWDK auto-test-sign cert (`CN="WDKTestCert user,…"`) must be in **LocalMachine
  Root + TrustedPublisher** (`certutil -addstore -f`) or driver-install shows a publisher prompt;
  test-signing already on.

### Dynamic resolution — PARTIAL (follows the dom0 window to the nearest offered mode)

The gui-agent already drives dynamic resolution: on a dom0 window/host resize it calls
`RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight)` (`gui-agent/main.c:732`) →
`SetVideoMode` → `SelectSupportedMode` (`resolution.c:118`), which **snaps the request to the
nearest mode the driver *enumerated at gui-agent startup*** (`EnumDisplaySettingsW`,
`InitVideoModes`), filtered ≤ the dom0 host screen. So matching quality = how dense the driver's
mode list is.

- **Done:** densified `bdd_dmm.cxx C_SampleSourceMode[]` from 9 coarse modes to ~29 spanning
  640×360..1856×1044 (16:9/16:10/4:3 + intermediates; must stay **sorted ascending by width** —
  `AddSingleSourceMode` `break`s on the first width > the framebuffer max). After rebuild/reload,
  **restart the gui-agent** so `InitVideoModes` re-enumerates. Mode switches reuse the fixed
  1920×1080 primary (a smaller source mode is centered/letterboxed; the desktop *logical* size =
  the source mode, which is what the gui-agent's DXGI duplication captures — so dom0 sees the new
  size). The guest now follows the dom0 window to the nearest of the 29 modes.
- **Note:** can't drive/observe a mode switch from a **qrexec (session-0)** PowerShell —
  `ChangeDisplaySettings` returns success but doesn't affect the interactive desktop, and
  `EnumDisplaySettings` returns empty; `CopyFromScreen` *does* read the real desktop. The live
  path runs in the gui-agent's interactive session, triggered by real dom0 window resizes.
- **For pixel-EXACT match (next):** the gui-agent snaps to enumerated modes, so exact arbitrary
  sizing needs either (a) a driver IOCTL `SetPreferredMode(w,h)` that rewrites `DispInfo`,
  reallocates the primary, and calls `DxgkCbIndicateChildStatus` (disconnect→reconnect) to force
  the OS to re-enumerate that exact mode, **plus** a gui-agent patch to call it before
  `ChangeDisplaySettings` (and not snap); or (b) a very fine mode grid. (a) is the M4-K design.

### Remaining work

1. **Pixel-exact dynamic resolution** — driver `SetPreferredMode` IOCTL + dynamic mode +
   `DxgkCbIndicateChildStatus` re-enum, and the matching gui-agent patch (rebuild
   `qubes-gui-agent-windows`). See above.
2. **Native frame path** (M2-K) — grant the primary's pages to dom0 over `XcGnttab` + IOCTL,
   replacing the gui-agent's DXGI capture.

> **License:** the KMDOD is the MS-PL `video/KMDOD` sample (reference only) — its source stays
> **out of this GPL-2 repo**; only the recipe above (two changes + the build/install mechanics) is
> recorded. The upstreamable Qubes driver is original GPL-2 code studying this plus
> `qxl-wddm-dod`/`viogpudo`.

---

## ⚠️ VERDICT (2026-06) — IddCx cannot load on this HVM; supersedes F1/F4

After taking the IddCx fork all the way to a clean build + correct install, **the
driver will not LOAD on this Qubes HVM, and the cause is fundamental to IddCx on
this emulated platform — not anything fixable in the driver.** This supersedes the
theories in **F1** ("version gate") and **F4** ("LGIdd is Win10-proven"), both
disproven by experiment.

### Every driver-side hypothesis was eliminated

| Hypothesis | Verdict |
|---|---|
| v143 + WDK-NuGet "not a real driver build" | **No** — built with the real `WindowsUserModeDriver10.0` toolset (EWDK 28000); still fails. |
| IddCx version gate (client > OS 1.5) | **No** — the MS sample at IddCx **1.4** fails identically to LGIdd at 1.10. |
| The `IddCx0102` extension declaration | **No** — fails with `UmdfExtensions` removed too. |
| Dynamic CRT | **No** — static-CRT LGIdd build failed; dynamic-UCRT MS build failed. |
| Missing DLL dependency | **No** — `dumpbin`: all deps Win10-present; PE subsystem 10.00. |
| Test-signing / publisher trust | **No** — testsigning on, cert in Root+TrustedPublisher, DLL sig Valid. |
| Missing GPU / render adapter | **No** — Basic *Render* Driver + DxgKrnl + IndirectKmd all healthy/running. |
| INF settings (class/filter/extension) | **No** — fails with a minimal echo-style INF. |

### The decisive experiment

The MS `IddSampleDriver` DLL (`iddcxstub`-linked) and the WDK `echo` UMDF2 DLL
(no IddCx), installed with the **identical minimal INF** (Class=System, no
`UmdfExtensions`, no `IndirectKmd`, explicit `AddService=WUDFRd`):

```
ROOT\SYSTEM\0001   echo DLL    problem=0    (loads, runs)
ROOT\SYSTEM\0002   IddCx DLL   problem=31   (2007 / 0xD000000D)
```

The **only** difference is the `iddcxstub` linkage. A custom-built, test-signed
UMDF driver loads fine here — *unless* it links IddCx. The UMDF host fails to bind
the IddCx class extension during host setup, **before it even loads `IddCx.dll`**
(loader snaps under cdb attached via IFEO show the host loads its framework DLLs
then `NtTerminateProcess`-exits without ever mapping `IddCx.dll`; `IddCx.dll`
itself loads fine standalone). The exact internal reason is in Microsoft's
WUDFx/IddCx code (no public PDBs), but it is not needed: no driver-side change can
fix a host-side extension-binding failure.

### Consequence — pivot to KMDOD

IddCx is a dead end on `romhacking-hma-driver` as configured. The project pivots
to the documented Plan B: a **kernel-mode own-framebuffer KMDOD** (WDDM
display-only miniport that allocates its primary in grant-shareable system RAM).
The frame-path / grant / dynamic-resolution design (F3 + the integration design in
[`plan.md`](plan.md)) carries over unchanged.

### Toolchain note (what actually builds drivers here)

The v143 + WDK-NuGet path *compiles* drivers but the real driver build needs the
**EWDK** (Enterprise WDK 28000 ISO, mounted at `D:`; build via
`D:\BuildEnv\SetupBuildEnv.cmd` then
`msbuild /p:PlatformToolset=WindowsUserModeDriver10.0 /p:WindowsTargetPlatformVersion=10.0.28000.0`).
The guest has devkitPro/MSYS2 that shadows `cmd`/`tar` — always use full
`C:\Windows\System32\` paths. This carries over to the KMDOD build.

---

## F1 — Windows 10 is an IddCx 1.5 ceiling

**Claim.** Windows 10 versions 2004 through 22H2 (builds 19041 through 19045,
including 22H2 / build 19045) ship **IddCx 1.5** (`IddCxGetVersion` returns
`0x1500`). The Windows 10 ceiling is **1.5**, not 1.4 — 1.4 was the ceiling only
for the older Windows 10 18362 / 18363 (19H1 / 19H2). IddCx 1.5 is itself a
Windows 10 version (first shipped in Win10 2004); it is **not** Windows 11 only.
The first Windows-11-only IddCx version is **1.8** (Win11 21H2, build 22000). A
driver whose effective load-time client version resolves higher than the OS
framework version (1.5 on this guest) will not load on Windows 10.

### Version -> OS table

| IddCx version | First OS that ships it          |
|---------------|----------------------------------|
| 1.4           | Windows 10 18362 / 18363 (19H1 / 19H2) |
| 1.5           | Windows 10 19041..19045 (2004..22H2, incl. 22H2 / 19045) |
| 1.8           | Windows 11 21H2 (build 22000)   |
| 1.9           | Windows 11 22H2 (build 22621)   |
| 1.10          | Windows 11 23H2 (build 22631)   |

(Compile-time headers from the WDK NuGet go as high as **IddCx 1.10**; the header
version available at build time is independent of what the running OS framework
supports. See the runtime pattern below.)

### Evidence

The kernel-mode IddCx framework component on the test guest:

```
C:\Windows\System32\drivers\IndirectKmd.sys   ->   version 10.0.19041.1
```

The `19041` confirms the 2004 servicing branch, which per Microsoft ships IddCx
**1.5**. Note this version string identifies the servicing branch only; it does
**not** by itself prove a particular IddCx API level, so it is not evidence of
"1.4". The real proof point is the load failure below.

A driver built against the IddCx **1.10** headers *and with effective client
version 1.10* fails to load on this guest. Device Manager shows:

```
device problem code 31
UMDF host load error 0xD000000D
```

The load fails because the client version (1.10) is newer than the OS framework
version (1.5): the load-time gate requires the driver's effective client version
to be `<=` the OS IddCx framework version. Captured via the UMDF operational log
(see [`install-and-debug.md`](install-and-debug.md) for the `wevtutil` /
`devcon restart` capture procedure).

### Consequence — the robust pattern

Do **not** force the effective client version above the OS framework. Use the
LGIdd pattern:

1. **Compile** against the newer headers (1.10 from the WDK NuGet) — fine, this
   only affects what symbols are visible.
2. Set **`IDDCX_MINIMUM_VERSION_REQUIRED`** to the *minimum* IddCx version the
   driver requires. This is a policy choice, not a negotiated client version:
   lower means the driver installs on more OSes (Microsoft's IddCx sample uses
   `3`). It does not by itself raise the effective client version.
3. The load-time gate is simply: the driver's required / effective client version
   must be `<=` the OS framework version. APIs newer than the minimum are reached
   at runtime via `IDD_IS_FUNCTION_AVAILABLE` / `IDD_IS_FIELD_AVAILABLE` — keep
   the effective client version `<= 1.5` for this Win10 guest.
4. Empirically, the linked `iddcxstub.lib` embeds a client version: we observed
   that linking the **1.10** stub embedded client = 1.10 and the driver then
   failed on the 1.5 framework. This is an LGIdd-style build detail we found by
   experiment — it is **not** Microsoft-documented as the gate. To stay within
   the Win10 ceiling, link a stub whose embedded client version is `<= 1.5`:

   ```
   <wdknuget>\c\Lib\10.0.26100.0\um\x64\iddcx\1.5\iddcxstub.lib
   ```

5. **Runtime-guard** any API newer than the effective client version with
   `IDD_IS_FUNCTION_AVAILABLE` / `IDD_IS_FIELD_AVAILABLE` before using it.

Keeping the effective client version `<= 1.5` is the configuration confirmed to
load on Windows 10.

---

## F2 — MttVDD (Virtual-Display-Driver, "HDR" edition) is Windows 11 only

**Claim.** `VirtualDrivers/Virtual-Display-Driver` (MttVDD, MIT) structurally
cannot run on Windows 10, and was therefore abandoned as the fork base.

### Evidence

`Driver.cpp` (around **line 4442**) sets
`IDDCX_MONITOR_MODE2.BitsPerComponent` via the `SDRCOLOUR | HDRCOLOUR` code path.
`IDDCX_MONITOR_MODE2` / `IDDCX_TARGET_MODE2`, `BitsPerComponent`, and HDR10 were
introduced in **IddCx 1.10** (not 1.6 — 1.6 only added
`IddCxSwapChainGetPhysicallyContiguousAddress`). So MttVDD's Mode2 / HDR code
**requires the IddCx 1.10 headers**.

- Building MttVDD against the Win10-compatible headers (without the 1.10 Mode2 /
  HDR types) yields a compile that **exceeds 100 errors**
  (`error count exceeds 100`) — those types simply don't exist below 1.10.
- Building it against IddCx **1.10** so it compiles produces a driver that, by
  F1, **will not load on Win10**: 1.10 > the Win10 framework version 1.5, so the
  load-time gate rejects it.

There is no version of MttVDD that both compiles and loads on Windows 10. The
skeleton was still useful for M0 (it built to `MttVDD.dll`, 746 KB, proving the
toolchain), but it is not the fork base.

> Note: the milestone plan ([`plan.md`](plan.md)) was written before this was
> resolved and still floats MttVDD as a candidate fork base pinned at IddCx 1.5.
> F1 + F2 supersede that: the Win10 framework ceiling is **1.5** (keep the
> effective client version `<= 1.5`), and the fork base is **LGIdd** (F4).

---

## F3 — Risk R1 resolved = GO

**The question (plan risk R1, the biggest one).** When an IddCx swap-chain frame
is acquired, does the framework hand the driver a **CPU-readable system-memory
buffer**, or only a **GPU texture** that needs a staging copy before the CPU can
read it? The plan's reports disagreed, so this was a go/no-go gate on the whole
IddCx approach.

**Answer: it is a GPU texture, and that is fine — the proven Win10 path is
TEXTURE -> GPU-copy -> CPU buffer. GO.**

### Evidence — read from LGIdd source

In `idd/LGIdd/CSwapChainProcessor.cpp`, Looking Glass acquires each frame as a
D3D texture:

- the acquired frame is `buffer.MetaData.pSurface`, cast to `ID3D11Texture2D`;
- the `AcquireSystemMemoryBuffer = FALSE` flag exists **only on the IddCx 1.10
  `Buffer2` acquire path**; the legacy acquire returns a texture with no such
  option. On the 1.10 `Buffer2` path — which *could* request a system buffer —
  they **explicitly set `AcquireSystemMemoryBuffer = FALSE`**.

So the Looking Glass authors, who target Win10, **deliberately do not** use the
system-buffer API. Instead they:

1. GPU-copy the acquired texture (D3D12 `CopyTextureRegion`, dirty-rect aware)
   into `CFrameBufferResource`, then
2. write that buffer out to their sink.

`CFrameBufferResource` is **either** a GPU-shared placed resource on a
cross-adapter heap (fast path: the GPU copies straight in and completion just
advances the write pointer via `FinalizeFrameBuffer`) **or** a CPU-mapped
readback buffer that needs one extra CPU copy (`WriteFrameBuffer`). The choice is
made by `IsIndirectCopy()`. The fast path — GPU writing straight into the shared
buffer, no extra CPU copy — is exactly the optimization we want for the grant
buffer.

### Port implications for Qubes

- Copy the frame into a **page-aligned, grant-shareable system-RAM** buffer (the
  driver's primary), then **`XcGnttab`-grant those pages** and hand grant-refs +
  WxH + dirty-rects to the gui-agent over a private IOCTL. The `IsIndirectCopy`
  fast path maps onto "GPU writes straight into the granted buffer."
- We can likely **drop LGIdd's D3D12 + compute-shader post-processing** — that
  machinery exists for HDR / format conversion, which Qubes does not need. We
  only need BGRA. A plain **D3D11 `CopyResource` -> STAGING texture
  (`CPU_ACCESS_READ`) -> `Map`** is sufficient and is structurally identical to
  the per-frame readback the current DXGI agent path already does.

This is one software copy per frame in the worst case; acceptable, and the
direction the plan already budgeted for (R6, dirty-rects + persistent granted
primary).

---

## F4 — Fork base = LGIdd

**Decision.** Fork `gnif/LookingGlass`, subtree `idd/LGIdd/` (GPL-2.0-or-later).
Rationale: it is **Win10-proven**, loads under the F1 pattern (effective client
version `<= 1.5`),
and it **already solves the per-frame copy-out** (F3). LGIdd is the architecture
reference *and* the code base; MttVDD (F2) is not viable.

### Rip out

- **CIVSHMEM** — the IVSHMEM sink. Replaced by the `XcGnttab` grant path.
- **LGMP** — the Looking Glass shared-memory protocol (a git submodule at
  `repos/LGMP`). Not needed.
- **CPipeServer** — the named-pipe control channel. Replaced by a private IOCTL
  the gui-agent reads.

### Graft

- The **`XcGnttab` grant path** (grant the primary's pages with
  `XcGnttabPermitForeignAccess2`, as the existing Qubes agent does).
- A **private IOCTL** carrying grant-refs + WxH + dirty-rects to the gui-agent.

### Keep

- IddCx adapter / monitor lifecycle and the `EVT_IDD_CX_*` callbacks.
- The swap-chain frame-extraction thread (the F3 acquire -> copy machinery).
- Mode-list / EDID plumbing.
- The root-enumerated INF.

### Dependencies to handle when building

- the **LGMP** submodule (removed with CIVSHMEM/LGMP, but its presence affects the
  initial checkout/build wiring);
- the **`LGCommon`** folder;
- the **`vendor/`** folder;
- **`d3d12.lib`** + **`d3dcompiler.lib`** (drop if we go pure-D3D11 per F3);
- **`effect/*.cpp`** compute shaders (the post-processing we expect to drop);
- the **`MSBuilder.Git`** NuGet package, used only to stamp a `VersionInfo.h` —
  can be **stubbed** (provide a static header) rather than carried.

Apply the F1 load pattern (compile against the 1.10 headers, set
`IDDCX_MINIMUM_VERSION_REQUIRED` as a minimum-required policy, keep the effective
client version `<= 1.5` by linking a stub whose embedded client `<= 1.5`,
runtime-guard newer APIs) to the fork so it loads on the Win10 test
guest. Build glue: [`build-toolchain.md`](build-toolchain.md). Install / sign /
debug: [`install-and-debug.md`](install-and-debug.md).

---

## KMDOD pivot — feasibility gate = GO

After the VERDICT (IddCx is blocked here), the project pivoted to a **kernel-mode
display-only WDDM miniport (KMDOD)** with its own framebuffer. The feasibility gate
— which the IddCx effort never had — **passed.**

### Result

Built the Microsoft `video/KMDOD` sample in the EWDK (kernel-mode
`WindowsKernelModeDriver10.0` toolset → `SampleDisplay.sys`) and installed it on a
throwaway clone (`romhacking-hma-driver-clone`). The KMDOD INF matches PCI display
class `CC_0300`, so it **replaces the Basic Display Adapter** on the QEMU stdvga
(`PCI\VEN_1234&DEV_1111`); a reboot is required for the swap to take effect.
Recovery if it fails: `qvm-run` (qrexec) is display-independent, so the driver can
be ripped out even with a dead screen — and the clone is disposable.

After fixing one real bug (below), the KMDOD **loads, runs `DriverEntry`,
`AddDevice`, and `StartDevice` — including `DxgkCbAcquirePostDisplayOwnership`
succeeding (so this HVM *does* expose a POST framebuffer)** — reaching device
problem code **43** (`CM_PROB_FAILED_POST_START`: a *later* display DDI fails
post-start; opaque in the event logs, status 0). This is categorically further
than IddCx (which never loaded at all). **The WDDM display-only model is viable on
this HVM.**

### The MS KMDOD sample bug (`ExAllocatePool2` vs `POOL_TYPE`)

The sample's `operator new` (`memory.cxx`) calls
`ExAllocatePool2(PoolType, Size, BDDTAG)`, but `ExAllocatePool2`'s first argument
is **`POOL_FLAGS`, not `POOL_TYPE`**. The callers pass `NonPagedPoolNx` (POOL_TYPE
`0x200`), which as a POOL_FLAG is a reserved bit → the allocation returns NULL →
`STATUS_NO_MEMORY` (`0xC0000017`) → `AddDevice` fails (problem 31). Fix: use
`ExAllocatePoolZero` (which takes `POOL_TYPE`), exactly as the sample's own comment
recommends.

### Next — M1-K (own-framebuffer) → M2-K (grant)

Replace the POST-framebuffer `StartDevice` (`bdd.cxx:80-88`) with our **own
grant-shareable system-RAM primary** + a fixed BGRA mode +
`DxgkDdiPresentDisplayOnly`→our-buffer copy (this also resolves the sample's
`problem=43`, which lives in its POST path). Then **M2-K** grants that buffer to
dom0 over `XcGnttab` — the F3 / [`plan.md`](plan.md) frame-path and the
dynamic-resolution design (M4-K) carry over unchanged.

> **License:** the MS KMDOD is **MS-PL** (reference only, not GPL-shippable). The
> upstreamable Qubes driver is **original GPL-2** code, studying the MS sample plus
> `qxl-wddm-dod` / `viogpudo` patterns.

---

## ⛔ FINAL VERDICT — a custom WDDM display driver is NOT achievable on this Qubes/Xen Win10 HVM

> **🚫 SUPERSEDED / WRONG (2026-06-20).** This verdict is **overturned** — see
> "✅ BREAKTHROUGH" at the top of this file. Route #2 (KMDOD) **works**: the
> `problem=43` was *not* a platform rejection but two newer-WDK-vs-older-OS build
> bugs (a `DXGK_DRIVERCAPS` struct-size mismatch and an async-present TDR), both
> now fixed; the KMDOD is the live active 1920×1080 display on the test clone. The
> error below was concluding "platform wall" while overlooking that the inbox
> `BasicDisplay.sys` is itself a working WDDM display-only miniport on the same
> stdvga, and never applying the IddCx **F1** version-gate lesson to the kernel
> driver. Kept for the record (routes #1/#3/#4 analysis is still accurate).

After exhaustive investigation, **all four viable routes are walled off by the platform**, not by our code:

### 1. IddCx (user-mode indirect display) — won't load
The UMDF host cannot bind the IddCx class extension on this emulated HVM. Any
`iddcxstub`-linked driver fails (device problem **31** / host `0xD000000D`) while a
non-IddCx UMDF driver loads with the identical INF. Toolchain/version/CRT/signing/
INF/render-adapter all eliminated. (See the earlier VERDICT section.)

### 2. KMDOD (kernel display-only, on the QEMU stdvga) — adapter silently rejected
The KMDOD **loads, runs DriverEntry, AddDevice, StartDevice, QueryChildRelations,
QueryAdapterInfo** — all succeed — and then **dxgkrnl silently rejects the adapter**:
device problem **43** (`CM_PROB_FAILED_POST_START`), **Problem Status `0x0`**, *before
any VidPn DDI is called*. Confirmed via Kernel-PnP + dxgkrnl ETW (the dxgkrnl trace is
34k unformatted profiler/vsync events — no readable rejection reason). Crucially, the
**stock, unmodified Microsoft `video/KMDOD` sample fails identically** — so this is the
platform, not our driver. Root: the **stdvga (`VEN_1234&DEV_1111`) is a plain VGA
device, not a GPU**; Windows hosts only the inbox Basic Display Adapter on it and
rejects a real WDDM adapter. (Removing the leftover IddCx adapter, ruling out the mode
list, and the `ExAllocatePoolZero` fix were all done; none changes the rejection.)

### 3. Live kernel debugging (to see *why* dxgkrnl rejects it) — no transport
- `tcp` serial → **libxl refuses to build the domain**.
- `pty` serial → starts, but the pty is in the **stubdomain**; reaching a WinDbg means a
  fragile stubdom→`xl console`→socat→cross-qube→pipe multi-hop.
- KDNET → the only NIC is **RTL8139** (not KDNET-capable) and the Xen device-model
  offers no KDNET-supported NIC.

### 4. A different virtual GPU (qxl / virtio-gpu) — not offered by libxl
Qubes exposes a first-class `video-model` feature, but this Xen/libxl only supports
`stdvga`/`cirrus`/`none`:
- `qvm-features <vm> video-model qxl` → domain fails to build (qxl requires SPICE,
  which Qubes does not run).
- `qvm-features <vm> video-model virtio` → `unsupported configuration: video type
  virtio is not supported by libxl`.

### Root cause + unblock condition
The platform gives Windows **only a basic VGA device**, on which Windows will not host a
real WDDM adapter — and the two ways out (a true virtual GPU, or live KD to engineer
around the rejection) are **both unavailable in this Qubes/Xen build**. The unblock
condition is upstream: **virtio-gpu support in Xen/libxl** (the 2025 patch series) →
then `viogpudo` (the proven virtio-gpu WDDM display-only driver) + a `XcGnttab` grant
path becomes viable, and the entire frame-path/dynamic-resolution design in
[`plan.md`](plan.md) carries straight over. **Until then, the agent's existing
DXGI-capture-of-BasicDisplay path is the only workable approach**, and that upstream
`TODO: custom WDDM driver` stays blocked for a concrete, documented reason.

### What this effort produced (durable value)
A working EWDK driver build/sign/deploy pipeline over qrexec; a kernel registry-logging
tracer (`QbLog`) and an ETW-capture harness for headless WDDM debugging; the precise
failure signature of every route; and a concrete spec of what a future Qubes Win10
display driver needs (a virtio-gpu-capable Xen + `viogpudo` + the grant path).
