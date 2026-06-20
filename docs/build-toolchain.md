# Build toolchain (M0a): IddCx UMDF driver with VS Build Tools + WDK-via-NuGet

This is the reproducible recipe for the M0a achievement: building an **IddCx
indirect display driver** (UMDF 2.x, user-mode) with **Visual Studio Build
Tools 2022** and the **WDK delivered as a NuGet package** — *no EWDK and no WDK
Visual Studio extension*. Build Tools cannot host the WDK VS extension, so the
legacy `WindowsUserModeDriver10.0` driver platform toolset does not exist on
this machine; the entire trick is making a normal `v143` C++ project consume the
WDK NuGet and link the driver stubs by hand.

The authoritative glue lives in
[`../toolchain/Directory.Build.props`](../toolchain/Directory.Build.props) and
[`../toolchain/Directory.Build.targets`](../toolchain/Directory.Build.targets).
**Those files are the source of truth — this doc describes them; if they ever
disagree, the files win.**

For packaging, signing, install and the runtime version gate (problem code 31 /
UMDF `0xD000000D`), see
[`install-and-debug.md`](./install-and-debug.md). For *why* Win10 caps at IddCx
1.5 and why MttVDD is Win11-only, see [`findings.md`](./findings.md).

> Scope note: M0a built the **unmodified MttVDD skeleton** (`MttVDD.dll`, 746 KB)
> purely to validate the toolchain. MttVDD itself is Win11-only and was abandoned
> as the fork base (see [`findings.md`](./findings.md), F2); the actual driver
> forks LookingGlass LGIdd (F4). The toolchain below is what matters and carries
> forward unchanged.

---

## 1. Prerequisites

All of these are installed in the Windows build+test guest `romhacking-hma-driver`
(Windows 10 Pro 22H2, build 19045). Paths below are the real ones on that guest.

### Visual Studio Build Tools 2022

Installed at:

```
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
```

Required pieces:

- **MSVC v143 toolset** (14.44) + MSBuild
- **Windows 11 SDK 10.0.26100.0** (the base SDK; note it does *not* carry the
  WDK headers — see fix 5 below)
- **ATL** — workload component IDs
  `Microsoft.VisualStudio.Component.VC.ATL` and
  `Microsoft.VisualStudio.Component.VC.ATLMFC`. The driver includes
  `atlbase.h`; without ATL the compile fails (see the debugging table, row
  *ATL*).

The installer bootstrapper is saved in the guest at `C:\dev\vs_buildtools.exe`.
To add a component to an existing install:

```
vs_buildtools.exe modify --installPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" --add <componentId> --quiet --norestart
```

The scripted setup is [`../scripts/guest-setup-buildtools.ps1`](../scripts/guest-setup-buildtools.ps1).

### The WDK, via NuGet (not EWDK)

The WDK is a `PackageReference`, restored on first build:

```
Microsoft.Windows.WDK.x64   version 10.0.26100.6584
```

It depends on `Microsoft.Windows.SDK.CPP.x64`. After restore it extracts to:

```
%USERPROFILE%\.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\
```

The real WDK payload (headers, libs, tools) lives under that package's **`c\`**
subfolder: `c\Include`, `c\Lib`, `c\bin`, `c\tools`.

The package ships `build\native\Microsoft.Windows.WDK.x64.props`, which MSBuild
auto-imports. That props file sets:

- `WindowsSdkDir` and `WDKContentRoot` → the package `c\` folder
- `WDK_NuGet=true`

What it **does not** do: register the legacy `WindowsUserModeDriver10.0`
platform toolset (that only exists with the WDK VS extension). That single
omission is the reason for almost every fix below.

---

## 2. Required `.vcxproj` settings

The driver project must be a normal `v143` C++ project that *declares* it is a
driver via properties, rather than relying on the absent driver toolset:

| Property | Value | Why |
| --- | --- | --- |
| `PlatformToolset` | `v143` | **Not** `WindowsUserModeDriver10.0` — that toolset is absent under Build Tools (MSB8020). The WDK NuGet props + the standard toolset supply the driver build. |
| `ConfigurationType` | `DynamicLibrary` | A UMDF driver is a user-mode DLL (`MttVDD.dll`, the LGIdd driver `.dll`, etc.). |
| `DriverTargetPlatform` | `Universal` | Universal UMDF driver. |
| `IndirectDisplayDriver` | `true` | Marks it as an IddCx driver for the WDK targets. |
| `WindowsTargetPlatformVersion` | `10.0.26100.0` | Matches the WDK/SDK; set in `Directory.Build.props`. |

It must also define the version macros the include/link paths are built from
(these feed straight into the props' `AdditionalIncludeDirectories` and
`AdditionalDependencies`):

- `IDDCX_VERSION_MAJOR` / `IDDCX_VERSION_MINOR` — the IddCx headers to **compile**
  against (the newer 1.10 headers; the include path resolves to
  `...\um\iddcx\<major>.<minor>\`).
- `UMDF_VERSION_MAJOR` / `UMDF_VERSION_MINOR` — selects the WDF UMDF stub lib
  path (`...\Lib\wdf\umdf\x64\<major>.<minor>\`). The MttVDD skeleton linked
  `WdfFunctions_02025` = **UMDF 2.25**.
- `IDDCX_MINIMUM_VERSION_REQUIRED` — the **minimum** IddCx version the driver
  *requires* of the OS framework. This is a policy choice, not a negotiated
  client version: a lower value installs on more OSes. Microsoft's canonical
  IddCx-1.4 sample driver uses `IDDCX_MINIMUM_VERSION_REQUIRED = 3`, and that is
  the value to follow for a broadly-loadable Win10 driver. Newer-than-minimum
  APIs are reached at runtime via `IDD_IS_FUNCTION_AVAILABLE` /
  `IDD_IS_FIELD_AVAILABLE`. See [`findings.md`](./findings.md) F1 for the
  load-time gate and what happens if the *effective client version* exceeds the
  OS framework version (problem code 31).

> Note the deliberate split between **compiling** and **linking the stub**. The
> `IDDCX_VERSION_*` macros pick the IddCx *headers* you compile against (the newer
> 1.10 headers, so the full surface is visible to the compiler). The props link
> the `iddcxstub.lib` at a **separate stub/minimum version that is decoupled from
> the compile `IDDCX_VERSION_*`** — defaulted to a Win10-loadable level
> (**≤ 1.5**) so the stub's embedded client version stays at or below the
> Windows 10 IddCx framework ceiling (Win10 2004..22H2 ship IddCx **1.5**). That
> linked stub/minimum version is what the load-time gate checks: the driver's
> effective client version must be **≤** the OS IddCx framework version, so a
> client built this way passes on a 1.5 (or lower) framework even though it was
> compiled against 1.10 headers.
>
> We saw the failure mode empirically: linking the **1.10** `iddcxstub.lib`
> embeds client = 1.10, and the driver fails to load on the 1.5 framework
> (problem code 31 / UMDF host `0xD000000D`). Per-version stub swapping is an
> LGIdd-style build detail we observed, not a Microsoft-documented gate
> mechanism — the documented gate is simply *effective client ≤ framework*. For
> the Qubes Win10 driver, keep the linked stub / effective client version
> **≤ 1.5**.

A minimal `.vcxproj` property group pulling all of the above together — these are
the `$()` macros the props in §3 reference, so they must originate here in the
project (except `WindowsTargetPlatformVersion`, which `Directory.Build.props`
supplies; see §3, *and the reader does not add it*):

```xml
<PropertyGroup Label="Globals">
  <ConfigurationType>DynamicLibrary</ConfigurationType>
  <DriverTargetPlatform>Universal</DriverTargetPlatform>
  <IndirectDisplayDriver>true</IndirectDisplayDriver>
  <PlatformToolset>v143</PlatformToolset>
  <TargetVersion>Windows10</TargetVersion>

  <!-- WDF UMDF stub lib selector: ...\Lib\wdf\umdf\x64\2.25\ -->
  <UMDF_VERSION_MAJOR>2</UMDF_VERSION_MAJOR>
  <UMDF_VERSION_MINOR>25</UMDF_VERSION_MINOR>

  <!-- IddCx HEADERS to compile against (full surface): ...\um\iddcx\1.10\ -->
  <IDDCX_VERSION_MAJOR>1</IDDCX_VERSION_MAJOR>
  <IDDCX_VERSION_MINOR>10</IDDCX_VERSION_MINOR>

  <!-- Minimum IddCx the driver REQUIRES of the OS (policy; lower = broader). -->
  <!-- Microsoft's canonical IddCx-1.4 sample uses 3. -->
  <IDDCX_MINIMUM_VERSION_REQUIRED>3</IDDCX_MINIMUM_VERSION_REQUIRED>
</PropertyGroup>
```

Note `WindowsTargetPlatformVersion` is **not** in this snippet: it is set in
`Directory.Build.props` (§3) and must not be added by the reader.

---

## 3. What `Directory.Build.props` / `Directory.Build.targets` do

These two files are dropped next to the driver `.vcxproj`. MSBuild imports
`Directory.Build.props` *early* (before the project body) and
`Directory.Build.targets` *late* (after it, and crucially **after** the WDK
NuGet props). They encode fixes 2–6 plus the trailing-slash fix. Each fix below
was discovered by an iterative build failure (see §5).

### `Directory.Build.props` (early import)

- **Setup step (not Fix 2) — target platform version.** Sets
  `WindowsTargetPlatformVersion` to `10.0.26100.0` so every driver project picks
  up the matching WDK/SDK without each `.vcxproj` declaring it (the reader does
  not add it to the project). This is a setup convenience, *not* the v143 toolset
  fix. The actual **Fix 2** is the `PlatformToolset=v143` choice in the
  `.vcxproj` (replacing the absent `WindowsUserModeDriver10.0` toolset — see §5,
  row 2); these props exist to make that v143 project consume the WDK.
- **Fix 3 — kill the legacy NuGet resolver.** Sets
  `RestoreProjectStyle=PackageReference` (force the modern restore, not the
  legacy `packages.config` flow) and `ResolveNuGetPackages=false`. The legacy
  `ResolveNuGetPackageAssets` task crashes with *"Sequence contains no
  elements"* / `GiveErrorForMissingFramework` on a native C++ `PackageReference`
  project; disabling it lets the modern restore drive the build.
- **The WDK `PackageReference`** itself:
  `Microsoft.Windows.WDK.x64` `10.0.26100.6584`.
- **Fix 5 — explicit WDK include roots.** Adds, via
  `AdditionalIncludeDirectories`, four directories under `$(WDKContentRoot)`:
  `um\iddcx\$(IDDCX_VERSION_MAJOR).$(IDDCX_VERSION_MINOR)`, `um`, `shared`, `km`.
  This is necessary because `$(WindowsSdkDir)` points at the **base** SDK, which
  has no IddCx/WDF/km headers — without this you get `C1083: cannot open
  IddCx.h`. The IddCx header itself lives at
  `$(WDKContentRoot)Include\10.0.26100.0\um\iddcx\<major>.<minor>\IddCx.h`.
- **Fix 6 — link the driver stub libs by hand.** Adds, via
  `AdditionalDependencies`, three libs the absent driver toolset would normally
  auto-link:
  - the WDF UMDF stub
    `$(WDKContentRoot)Lib\wdf\umdf\x64\<umdfmajor>.<umdfminor>\WdfDriverStubUm.lib`
  - the IddCx stub
    `$(WDKContentRoot)Lib\10.0.26100.0\um\x64\iddcx\<major>.<minor>\iddcxstub.lib`
    — linked at the **stub/minimum** version (decoupled from the compile
    `IDDCX_VERSION_*`, kept ≤ 1.5 for Win10), which is what the load-time gate
    checks; see the version-split note above
  - `ntdll.lib` — resolves `__imp_DbgPrintEx`, which the WDF UMDF stub pulls in.
    The WDK NuGet has no `ntdll.lib`, but the installed SDK does, so it is
    referenced **by name** and the linker finds it on the SDK lib path.

### `Directory.Build.targets` (late import) — the trailing-slash fix (fix 4)

The WDK NuGet props set `$(WindowsSdkDir)` and `$(WDKContentRoot)` to the
package `c\` folder **without a trailing backslash**. A driver `.vcxproj`
(and fix 5 above) builds include paths as `$(WindowsSdkDir)Include\...`, which
without the slash collapses to the broken `...\cInclude\...`. The traditional
WDK always terminates these roots with `\`.

The targets file re-assigns both variables through
`$([MSBuild]::EnsureTrailingSlash(...))` so that, e.g.,
`$(WDKContentRoot)Include\...` resolves to `...\c\Include\...`.

This **must** be in `Directory.Build.targets`, not `.props`: the NuGet props
that set the un-slashed values are imported *after* `.props` but *before*
`.targets`, so only the late import can correct them.

---

## 4. Build command

Use the **Build Tools** MSBuild (not a Developer Prompt from a full VS — there
is no full VS here):

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
```

Build the **`.vcxproj` directly, not the `.sln`** (fix 1 — the MttVDD solution
referenced a stale renamed project `IddSampleDriver.vcxproj` → `MSB3202 file not
found`). Restore first (the WDK NuGet download takes ~1.5 min), then build:

```
MSBuild.exe <driver>.vcxproj /t:restore
MSBuild.exe <driver>.vcxproj /t:build /p:Configuration=Release /p:Platform=x64 /m
```

The wrapped, base64/qrexec dev-loop version of this is
[`../scripts/guest-build.ps1`](../scripts/guest-build.ps1). Deploy from the
mgmtvm source-of-truth repo with
[`../scripts/deploy-to-guest.sh`](../scripts/deploy-to-guest.sh).

Output of the M0a validation build: `MttVDD.dll`, 746 KB.

A WDF detail worth calling out: a **vendored old WDF 2.15** was too old for the
26100 IddCx headers — it lacked `PFN_WDFDRIVERERRORREPORTAPIMISSING`. The fix is
to point the WDF include at the **WDK NuGet's** WDF (e.g. 2.25) rather than any
vendored copy. (See the debugging table, row *WDF*.)

---

## 5. Debugging table — the ~13 iterations

It took roughly 13 build iterations to peel every layer, in this order:
toolset → legacy resolver → include roots → WDF header version → ATL → the three
stub libs. Each row is symptom → cause → fix.

| # | Symptom (error) | Cause | Fix |
| --- | --- | --- | --- |
| 1 | `MSB3202`: project file not found | Built via `.sln`; the MttVDD solution referenced a stale renamed project `IddSampleDriver.vcxproj` | **Fix 1:** build the `.vcxproj` directly, not the `.sln`. |
| 2 | `MSB8020`: *build tools for WindowsUserModeDriver10.0 cannot be found* | `.vcxproj` requested the legacy driver toolset, which is absent under Build Tools (no WDK VS extension) | **Fix 2:** set `PlatformToolset=v143`; the WDK NuGet props + standard toolset supply the driver build. |
| 3 | `Sequence contains no elements` (in `ResolveNuGetPackageAssets`, `GiveErrorForMissingFramework`) | Legacy NuGet asset resolver chokes on a native C++ `PackageReference` project | **Fix 3:** `ResolveNuGetPackages=false` + `RestoreProjectStyle=PackageReference` (in `Directory.Build.props`). |
| 4 | Include path resolves to `...\cInclude\...` (paths broken) | WDK NuGet props set `WindowsSdkDir`/`WDKContentRoot` without a trailing `\` | **Fix 4:** `EnsureTrailingSlash` both, in `Directory.Build.targets` (late import, after the NuGet props). |
| 5 | `C1083`: cannot open include file `IddCx.h` | `$(WindowsSdkDir)` is the base SDK, which has no WDK/IddCx headers | **Fix 5:** add `$(WDKContentRoot)` `um\iddcx\<ver>`, `um`, `shared`, `km` to `AdditionalIncludeDirectories`. |
| 6 | WDF compile error: missing `PFN_WDFDRIVERERRORREPORTAPIMISSING` | A vendored WDF **2.15** is too old for the 26100 IddCx headers | Repoint the WDF include to the **WDK NuGet** WDF (e.g. 2.25) instead of the vendored copy. |
| 7 | Compile failure on `atlbase.h` | ATL not installed in Build Tools | Add the **ATL** components (`...Component.VC.ATL`, `...Component.VC.ATLMFC`). |
| 8 | `LNK2001`: unresolved `Wdf*` (WDF driver stub) | The driver toolset would auto-link the UMDF stub; under v143 it isn't linked | **Fix 6:** link `Lib\wdf\umdf\x64\<ver>\WdfDriverStubUm.lib`. |
| 9 | `LNK2001`: unresolved `Idd*` (IddCx stub) | Same — IddCx stub not auto-linked | **Fix 6:** link `Lib\10.0.26100.0\um\x64\iddcx\<ver>\iddcxstub.lib`. |
| 10 | `LNK2001`: unresolved `__imp_DbgPrintEx` | The WDF UMDF stub pulls in `DbgPrintEx`; the WDK NuGet has no `ntdll.lib` | **Fix 6:** reference `ntdll.lib` by name (the installed SDK provides it on the lib path). |

(The remaining iterations were re-runs converging on the exact version numbers
and path forms above — the substance is the rows shown.)

After a clean build, continue with packaging, INF token substitution, signing,
and install — including the un-substituted `$UMDFVERSION$` gotcha and the
runtime IddCx version gate (problem code 31 / `0xD000000D`) — in
[`install-and-debug.md`](./install-and-debug.md).
