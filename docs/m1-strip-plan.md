# M1 Strip Plan — forking LGIdd into `driver/`

Concrete, buildable plan to fork `~/driver-dev/LookingGlass/idd/LGIdd` (GPL-2.0-or-later)
into a new `driver/` subtree of this repo, stripped to the **M1 goal only**: build under
our toolchain (VS Build Tools + WDK-NuGet, `PlatformToolset=v143`), test-sign, install
root-enumerated via `devcon`, **load on IddCx 1.5**, and **enumerate one virtual monitor**
in Device Manager. No frame path, no grants, no sinks (that is M2+).

Source of truth for the disposition below: the four cluster maps + direct reads of
`LGIdd.vcxproj` and `LGIdd.inf`. Companion docs: [`findings.md`](findings.md) (F1 load
gate, F3 GO, F4 fork base), [`build-toolchain.md`](build-toolchain.md),
[`install-and-debug.md`](install-and-debug.md), [`plan.md`](plan.md).

The single load-bearing M1 edit: **`CIndirectDeviceContext::InitAdapter()` gates the
entire adapter+monitor lifecycle behind IVSHMEM** —
`CIndirectDeviceContext.cpp:113  if (!m_ivshmem.Init() || !m_ivshmem.Open()) return;`.
The Qubes guest has no IVSHMEM PCI device, so the unmodified driver returns early and
**never enumerates a monitor**. Removing this guard is the make-or-break change.

---

## (A) Minimal file set to copy into `driver/` (keep / stub / drop)

Copy these into `driver/` (flat, mirroring `idd/LGIdd/`), plus a small `compat/` for the
stubbed headers. Everything not listed as **copy** is **not** brought across.

| File | Action | Notes |
|---|---|---|
| `Driver.cpp` / `Driver.h` | **KEEP (SIMPLIFY)** | WDF bootstrap verbatim; stub `g_pipe.Init/DeInit` (Driver.cpp:41,94) → no-op; `CPlatformInfo::Init` (Driver.cpp:57) reduced to page-size only; static `VersionInfo.h`. |
| `Device.cpp` / `Device.h` | **KEEP (core)** | IddCx client config + all `EVT_IDD_CX_*` + the 1.10 runtime gate. Do **not** disturb the version guards (Device.cpp:39-59,130-186,231-248) — they are the F1 pattern that lets it load on 1.5. |
| `CIndirectDeviceContext.cpp` / `.h` | **KEEP lifecycle / STUB sink** | KEEP `QueryIddCxCapabilities`, `PopulateDefaultModes`, `InitAdapter`, `FinishInit`, `ReplugMonitor`, the mode/EDID callbacks, `UpdateMonitorModes`. **EDIT InitAdapter:113** to drop the IVSHMEM guard. **STUB** `SetupLGMP`→`return true`, no-op `DeInitLGMP`/`LGMPTimer`/`PrepareFrameBuffer`/`WriteFrameBuffer`/`FinalizeFrameBuffer`/`SendCursor`/`ResendCursor` (cpp:548-1013). Strip LGMP/IVSHMEM/frame/cursor **members** from `.h` and the `lgmp/host.h`, `common/KVMFR.h`, `CIVSHMEM.h`, `CPostProcessor.h` includes. |
| `CIndirectMonitorContext.cpp` / `.h` | **SIMPLIFY** | `AssignSwapChain` keeps only the `CD3D11Device` (cpp:42); **drop** `CD3D12Device` (cpp:50-64), **drop** `SetupLGMP` (cpp:66-71), and construct the **trimmed** `CSwapChainProcessor` (no dx12 arg). `GetPageSize()` (cpp:49) kept. Drop `m_dx12Device`/`m_swapChain`-dx12 members from `.h`. |
| `CSwapChainProcessor.cpp` / `.h` | **SIMPLIFY (gut body)** | KEEP the class + thread that binds the D3D11 device (`IddCxSwapChainSetDevice`) and drains/releases buffers and exits cleanly. **STUB** `SwapChainNewFrame`→`return true`. **DROP** pools/post-processor/D3D12 copy/`CompletionFunction`/dirty-rect helpers/cursor thread. Remove `CPipeServer.h` include + `g_pipe.SetGPUStatus` (cpp:136). Constructor loses the `dx12Device` parameter. |
| `CD3D11Device.cpp` / `.h` | **KEEP** | Already plain D3D11 BGRA FL11_1 — the one device the swapchain needs and the M2 target. Unchanged. |
| `CEdid.cpp` / `.h` | **KEEP (SIMPLIFY + rebrand)** | Monitor needs an EDID to enumerate (`CIndirectDeviceContext.cpp:108,218-219`). Keep base block + DTDs; **strip CTA HDR/colorimetry/VSDB** (CEdid.cpp:439-501); rebrand mfr ID + name "Looking Glass"→"Qubes" (CEdid.cpp:567). |
| `CSettings.cpp` / `.h` | **KEEP (SIMPLIFY)** | The `DefaultDisplayModes` fallback enumerates with **zero** registry keys. Keep the mode API (`LoadModes`/`GetDisplayModes`/`SetExtraMode`/`GetDefaultRefresh`). `ReadBoolValue`/`ReadStringValue` lose all callers once effects are dropped — keep the methods (harmless) or delete. |
| `CPlatformInfo.cpp` / `.h` | **SIMPLIFY to `GetPageSize`** | Only `GetPageSize()` survives (used at `CIndirectMonitorContext.cpp:49`); reduce `Init()` to the `GetSystemInfo`/`dwPageSize` line, drop UUID/CPU/product-name (they fed only LGMP VMInfo, now gone). |
| `Public.h` | **KEEP (regen GUID)** | `GUID_DEVINTERFACE_LGIdd` → regenerate as `GUID_DEVINTERFACE_QUBESIDD`. |
| `Trace.h` | **KEEP (regen GUID)** | WPP control GUID regenerated; UMDF 2.0 branch not taken (we ship 2.25). |
| LGCommon `CDebug.{cpp,h}` | **COPY** | Lightweight logging used everywhere. Copy into `driver/LGCommon/` (do not glob the whole folder). |
| LGCommon `DefaultDisplayModes.h` | **COPY** | Compiled-in fallback mode table consumed by `CSettings.cpp:23`. |
| `cpp.hint` | **COPY** | Harmless IntelliSense hint. |
| **STUB** `CIVSHMEM.h` (+ optional `.cpp`) | **STUB (header-only)** | Tiny shell so members/callers compile; see (B). Prefer header-only; delete `.cpp` from project. |
| **STUB** `CPipeServer.h` | **STUB (header-only)** | No-op `g_pipe`; see (B). Delete `.cpp` from project. |
| **STUB** `VersionInfo.h` | **STUB (static)** | Two `#define`s; see (D). Replaces the MSBuilder.Git generate target. |

**DROP entirely (not copied):**
`CD3D12Device.{cpp,h}`, `CD3D12CommandQueue.{cpp,h}`, `CPostProcessor.{cpp,h}`,
`effect/CComputeEffect.*`, `effect/CDownsampleEffect.*`, `effect/CHDR16to10Effect.*`,
`effect/CRGB24Effect.*`, `CInteropResource.{cpp,h}`, `CInteropResourcePool.{cpp,h}`,
`CFrameBufferResource.{cpp,h}`, `CFrameBufferPool.{cpp,h}`, the real `CIVSHMEM.cpp` and
`CPipeServer.cpp`, `LGCommon/PipeMsg.h`, the `repos/LGMP` submodule, `vendor/ivshmem/`,
`common/include/common/KVMFR.h`, `LGIddHelper.exe` (the helper service).

---

## (B) Exact stub interfaces (the symbols the rest of the code calls)

### `compat/CIVSHMEM.h` — header-only stub (drop `CIVSHMEM.cpp` from the project)
Callers after the strip: `CIndirectDeviceContext.h:59` holds `CIVSHMEM m_ivshmem;` and
`GetIVSHMEM()` (`.h:159`). With D3D12 dropped, `GetIVSHMEM()` has **no remaining caller**,
so ideally remove the member too. If you keep the member to minimize edits, the stub is:

```cpp
class CIVSHMEM {
public:
  CIVSHMEM()  {}
  ~CIVSHMEM() {}
  bool   Init()    { return true; }   // was: SetupDi enumerate
  bool   Open()    { return true; }   // was: IOCTL map BAR
  void   Close()   {}
  size_t GetSize() { return 0; }
  void*  GetMem()  { return nullptr; }
};
```
For M1 the `InitAdapter:113` guard is **deleted regardless**, so `Init/Open` returning
true is belt-and-suspenders. Removes the `vendor/ivshmem/ivshmem.h` + `SetupAPI` deps.

### `compat/CPipeServer.h` — header-only no-op (drop `CPipeServer.cpp` from the project)
Callers: `Driver.cpp:41,94`; `CIndirectDeviceContext.cpp:277,538` (`SetDisplayMode`),
`:781` (`SetCursorPos`); `CSwapChainProcessor.cpp:136` (`SetGPUStatus`, removed when that
file is gutted). Provide a no-op class + the `g_pipe` global so all sites link:

```cpp
#include <cstdint>
class CPipeServer {
public:
  bool Init()   { return true; }
  void DeInit() {}
  void SetCursorPos(uint32_t, uint32_t) {}
  void SetDisplayMode(uint32_t, uint32_t, uint32_t) {}
  void SetGPUStatus(bool) {}
};
extern CPipeServer g_pipe;   // define once (e.g. in Driver.cpp): CPipeServer g_pipe;
```
Removes the `LGCommon/PipeMsg.h` dep. (Match the exact arg types/arities to the real
header before finalizing — adjust if any call site passes different types.)

### `compat/VersionInfo.h` — static stub (replaces the GenerateVersionInfo target)
Callers: `Driver.cpp:26`, `CIndirectDeviceContext.cpp:28` use `LG_VERSION_STR`,
`LG_CURRENT_YEAR`.
```cpp
#define LG_VERSION_STR  "qubes-idd-m1"
#define LG_CURRENT_YEAR 2026
```

### `CIndirectDeviceContext` sink-method stubs (in-place no-ops)
`SetupLGMP(UINT64)` → `return true;`. `DeInitLGMP()`, `LGMPTimer()`,
`PrepareFrameBuffer(...)`, `WriteFrameBuffer(...)`, `FinalizeFrameBuffer(...)`,
`SendCursor(...)`, `ResendCursor()` → empty bodies (none are reached at M1: the only
callers are the gutted swapchain processor). When trimming the `.h`, **change/remove the
`PrepareFrameBuffer` signature** so it no longer references `D12FrameFormat`
(`CIndirectDeviceContext.h:153`) — that is the only coupling pulling `CPostProcessor.h`
(and thus D3D12) into the device context.

### `CSwapChainProcessor` trimmed constructor
Real signature (`CSwapChainProcessor.h:76`):
```cpp
CSwapChainProcessor(IDDCX_MONITOR, CIndirectDeviceContext*, IDDCX_SWAPCHAIN,
  std::shared_ptr<CD3D11Device>, std::shared_ptr<CD3D12Device>, HANDLE);
```
M1 form — **drop the `dx12Device` parameter and member**:
```cpp
CSwapChainProcessor(IDDCX_MONITOR, CIndirectDeviceContext*, IDDCX_SWAPCHAIN,
  std::shared_ptr<CD3D11Device>, HANDLE);
```
Body of the thread keeps: set MM thread characteristics → `m_dx11Device->GetDevice().As(&dxgiDevice)`
→ `IddCxSwapChainSetDevice` → loop on `IddCxSwapChainReleaseAndAcquireBuffer` with the
`E_PENDING`/17 ms/terminate wait → `IddCxSwapChainFinishedProcessingFrame` per new frame
number → on terminate `WdfObjectDelete(m_hSwapChain)`. Remove `m_resPool`/`m_fbPool`/
`m_postProcessor`/`m_dirtyRects`/cursor members and the `>=1.10` `Buffer2` block (Win10
takes the legacy acquire anyway). Update the one call site
(`CIndirectMonitorContext.cpp:73`) to the new arg list.

---

## (C) D3D12 + effects decision: **DROP** (M1 and permanently)

**Drop** `CD3D12Device`, `CD3D12CommandQueue`, `CPostProcessor`, the entire `effect/`
folder, `CInteropResource{,Pool}`, `CFrameBufferResource`, `CFrameBufferPool`, and the
`d3d12.lib` + `d3dcompiler.lib` link deps.

Justification:
- **Monitor enumeration does not need any D3D device.** The monitor is created by the
  IddCx adapter+monitor lifecycle (`InitAdapter`→`FinishInit`→`IddCxMonitorCreate`/
  `IddCxMonitorArrival`) **before** a swapchain exists. A D3D device is only needed once
  Windows assigns a swapchain and we call `IddCxSwapChainSetDevice` — the frame path.
- **The swapchain needs D3D11 only.** `IddCxSwapChainSetDevice` takes an `IDXGIDevice`
  derived from the **D3D11** device (`CSwapChainProcessor.cpp:84-85,107-110`). The whole
  D3D12 stack exists solely for LGIdd's IVSHMEM-heap zero-copy + HDR/format compute
  shaders — both discarded by the Qubes grant-page + plain-BGRA design (F3).
- **Link safety:** `D3D11CreateDevice`/`CreateDXGIFactory2` resolve via `OneCoreUAP.lib`
  (kept), so dropping `d3d12.lib`/`d3dcompiler.lib` leaves D3D11/DXGI intact.
- **No IddCx-version code lives in the dropped files**, so the cut does not affect 1.5
  loadability.

This is permanent: F3's endgame is D3D11 `CopyResource`→STAGING→`Map`, BGRA only — no
D3D12 interop, no compute post-processing.

---

## (D) `.vcxproj` changes (build under VS Build Tools + WDK-NuGet, v143)

Start from `LGIdd.vcxproj`, rename to `driver/QubesIdd.vcxproj`, and make these edits.
The toolchain `Directory.Build.props`/`.targets` live at `toolchain/` — either copy them
beside the `.vcxproj` or reference via a solution-level `Directory.Build.*` import; they
supply the WDK NuGet `PackageReference`, the iddcx/wdf/km include dirs from
`$(WDKContentRoot)`, the `WdfDriverStubUm.lib` + `iddcxstub.lib` + `ntdll.lib` link, and
the trailing-slash fix.

1. **PlatformToolset → `v143`.** Replace every `<PlatformToolset>WindowsUserModeDriver10.0</PlatformToolset>`
   (vcxproj:92,97,145,158) with `<PlatformToolset>v143</PlatformToolset>` in all four
   config blocks. Keep `ConfigurationType=DynamicLibrary`, `DriverTargetPlatform=Universal`,
   `<WindowsTargetPlatformVersion>10.0.26100.0</WindowsTargetPlatformVersion>`.

2. **Drop the MSBuilder.Git NuGet + VersionInfo git step.**
   - Remove the top import (vcxproj:3) and the `EnsureNuGetPackageBuildImports` error
     target (vcxproj:253-258).
   - Remove the `GenerateVersionInfo` target + `BuildDependsOn` plumbing (vcxproj:259-276)
     and `<None Include="packages.config" />` (vcxproj:24). Delete `packages.config`.
   - Ship the static `compat/VersionInfo.h` from (B) instead.

3. **Drop the LGMP ProjectReference** (vcxproj:247-249) entirely.

4. **Version property values** (per-config `Label="Configuration"` blocks, vcxproj:110-159).
   Following F1 / `Directory.Build.props`:
   - **Compile/header version** — keep `IDDCX_VERSION_MAJOR=1`, `IDDCX_VERSION_MINOR=10`
     (compile against the newest headers; the `Directory.Build.props` include dir uses
     `iddcx\$(IDDCX_VERSION_MAJOR).$(IDDCX_VERSION_MINOR)`). The `AdditionalOptions`
     `/DIDDCX_VERSION_MAJOR=1 /DIDDCX_VERSION_MINOR=10 /DIDDCX_MINIMUM_VERSION_REQUIRED=4`
     stay (after stripping the LGMP/vendor include dirs below).
     - *Risk-reduction alternative:* set both the props and the `/D` to
       `IDDCX_VERSION_MINOR=5`, which compiles out every `#if ...MINOR>=10` block. Loses
       nothing M1 needs; only do this if a 1.10-header compile snag appears.
   - **Stub/client version (the load gate)** — do **not** set in the `.vcxproj`; let
     `Directory.Build.props` default `IDDCX_STUB_VERSION_MAJOR=1` /
     `IDDCX_STUB_VERSION_MINOR=4` (links `iddcx\1.4\iddcxstub.lib`, ≤1.5 ⇒ loads on Win10).
     1.5 also works; 1.4 is the safe default. **Never** link the 1.10 stub (problem code 31).
   - **UMDF version** — keep `UMDF_VERSION_MAJOR=2`, `UMDF_VERSION_MINOR=25`,
     `UMDF_MINIMUM_VERSION_REQUIRED=25` (matches `WdfDriverStubUm.lib` path
     `Lib\wdf\umdf\x64\2.25` and the INF `UmdfLibraryVersion=2.25.0`). Keep
     `_NT_TARGET_VERSION=0xA000005`, `Driver_SpectreMitigation=Spectre`,
     `IndirectDisplayDriver=true`.

5. **Link deps** (all four `<Link><AdditionalDependencies>`, vcxproj:192,207,222,237):
   remove `d3d12.lib;d3dcompiler.lib`. Keep `OneCoreUAP.lib;avrt.lib`. The
   `WdfDriverStubUm.lib`/`iddcxstub.lib`/`ntdll.lib` come from `Directory.Build.props`.

6. **Include-dir cleanup** (all four `<AdditionalIncludeDirectories>`, vcxproj:189,204,219,234):
   remove `..\repos\LGMP\lgmp\include`, `..\vendor`, `..\common\include`. Keep a local
   `LGCommon` entry only if you copy `CDebug.h`/`DefaultDisplayModes.h` into a subfolder
   (`$(ProjectDir)LGCommon`). Add `$(ProjectDir)compat` for the stub headers. The
   iddcx/wdf/km/um/shared dirs are injected by `Directory.Build.props` — do not add them
   here.

7. **Source lists** (`ItemGroup`s at vcxproj:26-77): replace the `LGCommon/*.cpp` /
   `/LGCommon/*.h` **globs** (vcxproj:27,52) with **explicit** `LGCommon\CDebug.cpp` /
   `LGCommon\CDebug.h` / `LGCommon\DefaultDisplayModes.h` (the glob otherwise drags in
   `PipeMsg.h` and any other LGCommon files). Remove the `ClCompile`/`ClInclude` entries
   for every DROP file (D3D12, command queue, post-processor, all `effect\*`, interop
   resource/pool, framebuffer resource/pool, and the real `CIVSHMEM.cpp`/`CPipeServer.cpp`).
   Add `compat\CIVSHMEM.h`, `compat\CPipeServer.h`, `compat\VersionInfo.h` as `ClInclude`.

8. **INF item** (vcxproj:79): rename to `QubesIdd.inf`. Drop `LGIddHelper.exe` references
   (helper service is not part of M1).

---

## (E) The INF — Qubes-branded, Display class, root-enumerated

Note the source `LGIdd.inf` is **UTF-16LE**; author the fork the same way (or let the
WDK tooling re-encode). Derive `driver/QubesIdd.inf` from it with these changes:

- **Class / GUID:** keep `Class=Display`,
  `ClassGuid={4D36E968-E325-11CE-BFC1-08002BE10318}` (the Display class), `ClassVer=2.0`.
- **Hardware id / device node:** root-enumerated. Set the `[Standard.NT$ARCH$]` line to a
  single chosen hwid:
  ```
  %DeviceName% = QubesIdd_Install, Root\QubesIdd
  ```
  (drop the second non-`Root\` `LGIdd` line). This is the `Hwid` passed to
  `devcon install <inf> Root\QubesIdd`.
- **Rename install sections** `LGIdd_Install*` → `QubesIdd_Install*`, service `LGIdd` →
  `QubesIdd`, group id `LGIddGroup` → `QubesIddGroup`, catalog `LGIdd.cat` → `QubesIdd.cat`,
  binary `LGIdd.dll` → `QubesIdd.dll`. Drop the `LGIddHelper` `AddService` +
  `LGIddHelper_ServiceInstall` + the helper copy/source entries.
- **Keep** `AddReg "UpperFilters"=…"IndirectKmd"` and the `WUDF DeviceGroupId` reg, the
  `WUDFRd` service install, and:
  ```
  [QubesIdd_Install.NT.Wdf]
  UmdfService      = QubesIdd, QubesIdd_Install
  UmdfServiceOrder = QubesIdd
  UmdfKernelModeClientPolicy = AllowKernelModeClients

  [QubesIdd_Install]
  UmdfLibraryVersion = 2.25.0
  ServiceBinary      = %12%\UMDF\QubesIdd.dll
  UmdfExtensions     = IddCx0102
  ```
- **`$UMDFVERSION$` → `2.25.0` (literal).** Under v143 the `$UMDFVERSION$` token is **not**
  auto-substituted (M0 install gotcha; co-installer fails `0x57`). Hard-code `2.25.0`.
- **`UmdfExtensions = IddCx0102`** — keep; this is the IddCx UMDF extension binding (the
  encoded extension version string, unchanged from upstream).
- **Strings:** rebrand `ManufacturerName="Qubes OS"`,
  `DeviceName="Qubes Indirect Display Device"`, disk name, etc.
- **DriverVer:** give a real date/version (the WDK `stampinf`/inf2cat path expects one).

Install (root-enumerated, per CLAUDE.md): `pnputil /add-driver` only **stages** →
`devcon install QubesIdd.inf Root\QubesIdd` creates + binds the device node.

---

## (F) Build order + the 3 riskiest unknowns

**Build order (smallest reversible steps first):**
1. `mkdir driver/ driver/LGCommon driver/compat`; copy the KEEP/SIMPLIFY `.cpp/.h` set
   (A), `CDebug.*`, `DefaultDisplayModes.h`, `cpp.hint`. Write the three `compat/` stubs (B).
2. Copy `LGIdd.vcxproj`→`driver/QubesIdd.vcxproj` and `LGIdd.inf`→`driver/QubesIdd.inf`;
   place/import `toolchain/Directory.Build.{props,targets}`.
3. Apply the `.vcxproj` edits (D): v143, drop NuGet/LGMP/VersionInfo target, drop
   d3d12/d3dcompiler, clean include dirs + source lists, version props.
4. Apply the source strips: delete the IVSHMEM guard (`CIndirectDeviceContext.cpp:113`);
   swap real `CIVSHMEM.h`/`CPipeServer.h` for the `compat/` stubs; stub the
   LGMP/frame/cursor methods; trim `CIndirectDeviceContext.h` members + the
   `D12FrameFormat`/`CPostProcessor.h` include; simplify `CIndirectMonitorContext`
   (D3D11-only, drop SetupLGMP) and `CSwapChainProcessor` (gut body, drop dx12 param);
   reduce `CPlatformInfo` to `GetPageSize`; rebrand `CEdid` + `Public.h`/`Trace.h` GUIDs.
5. Deploy to the guest (`scripts/deploy-to-guest.sh romhacking-hma-driver ./driver "C:\dev\QubesIdd"`)
   and `guest-build.ps1` → resolve compile/link errors (expect a few orphaned references).
6. `guest-package-install.ps1 … -Inf QubesIdd.inf -Hwid "Root\QubesIdd"`; test-sign; install.
7. Confirm a virtual monitor enumerates in Device Manager; on failure capture the UMDF
   operational log (`wevtutil` + `devcon restart Root\QubesIdd`) per
   `install-and-debug.md`.

**Top 3 riskiest unknowns to validate first:**
1. **Does the monitor enumerate once the IVSHMEM guard is gone?** The whole M1 win hinges
   on `InitAdapter`→`FinishInit` reaching `IddCxMonitorArrival` without IVSHMEM. Watch
   `IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE` (CIndirectDeviceContext.cpp:128 — Windows
   sometimes refuses to enumerate without it) and the FP16 retry-without-flag fallback
   (cpp:161-170). **Validate by code-reading the lifecycle end-to-end before building**,
   then by the actual Device Manager check. This is the make-or-break.
2. **Effective client version ≤ 1.5 on the real guest.** F1 says a 1.10 client → problem
   code 31 / `0xD000000D`. We *believe* `iddcxstub 1.4` + `IDDCX_MINIMUM_VERSION_REQUIRED=4`
   + runtime guards loads, but it must be re-confirmed for *this* fork (compile at 1.10,
   link the 1.4 stub). First post-install check: it **loads** (no code 31).
3. **The strip compiles cleanly under v143 without LGMP/KVMFR/vendor.** `LGMP_Q_FRAME_LEN`,
   `KVMFR_*`, and `D12FrameFormat` thread through headers (`CIndirectDeviceContext.h`,
   the now-dropped pool files). Risk: a residual include/type from a KEEP file still
   references a dropped header. Mitigate by trimming `CIndirectDeviceContext.h` members +
   includes in lockstep with the `.cpp` stubs, and replacing any surviving slot-count with
   a local `#define` (e.g. 2) rather than `LGMP_Q_FRAME_LEN`.
