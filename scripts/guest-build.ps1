<#
.SYNOPSIS
  Build an IddCx UMDF driver in the guest with VS Build Tools 2022 + the WDK
  pulled via NuGet — no EWDK, no WDK Visual Studio extension.

.DESCRIPTION
  Run this INSIDE the guest (romhacking-hma-driver). It:
    1. Ensures toolchain/Directory.Build.props and Directory.Build.targets sit
       next to the .vcxproj (these are the WDK-via-NuGet glue; they MUST import
       for the build to find the IddCx/WDF headers and the stub libs).
    2. Invokes the Build Tools MSBuild with /t:restore FIRST (downloads the WDK
       NuGet, ~1.5 min the first time) then /t:build.
    3. Prints the produced .dll path.

  The two-step (restore then build) matters: restore pulls
  Microsoft.Windows.WDK.x64 10.0.26100.6584 (and its SDK.CPP dependency), whose
  props set WDKContentRoot/WindowsSdkDir to the package 'c' folder. Only after
  that does build have the IddCx headers + stub libs. A single combined
  /t:restore;build can race the props import on a cold cache, so we keep them
  separate.

  The .vcxproj MUST already use PlatformToolset=v143. The legacy
  WindowsUserModeDriver10.0 toolset does not exist under Build Tools (MSB8020).

.NOTES
  Why we build the .vcxproj directly and not the .sln: the original MttVDD .sln
  referenced a stale renamed project (IddSampleDriver.vcxproj) and failed with
  MSB3202 before any compilation. Always target the .vcxproj.
#>
[CmdletBinding()]
param(
    # Directory containing the driver .vcxproj. Defaults to the LGIdd fork root.
    [string]$ProjDir = 'C:\dev\LGIdd',

    # The .vcxproj to build. If relative, it is resolved against $ProjDir.
    # Defaults to the LGIdd project file.
    [string]$VcxProj = 'LGIdd.vcxproj',

    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [string]$Platform = 'x64',

    # Repo toolchain dir inside the guest. deploy-to-guest.sh stages the repo
    # such that the toolchain files are reachable; we copy them next to the
    # project if they are not already there.
    [string]$ToolchainDir = 'C:\dev\toolchain'
)

$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'

Write-Output '== guest-build.ps1 =='

# ---------------------------------------------------------------------------
# Resolve paths.
# ---------------------------------------------------------------------------
if (-not (Test-Path $ProjDir)) { throw "ProjDir not found: $ProjDir" }
$ProjDir = (Resolve-Path $ProjDir).Path

if (-not [System.IO.Path]::IsPathRooted($VcxProj)) {
    $VcxProj = Join-Path $ProjDir $VcxProj
}
if (-not (Test-Path $VcxProj)) { throw "vcxproj not found: $VcxProj" }
$VcxProj = (Resolve-Path $VcxProj).Path
Write-Output "Project dir : $ProjDir"
Write-Output "vcxproj     : $VcxProj"

# ---------------------------------------------------------------------------
# 1. Ensure the WDK-via-NuGet glue is next to the .vcxproj. Directory.Build.props
#    / Directory.Build.targets are auto-imported by MSBuild from the project's
#    directory (and ancestors). They are the authoritative toolchain files in
#    this repo (toolchain/) — we copy, never rewrite, them. The .targets import
#    is what fixes the WDKContentRoot trailing-slash; the .props adds the IddCx
#    include roots + the 3 stub libs and disables the legacy NuGet resolver.
# ---------------------------------------------------------------------------
foreach ($glue in @('Directory.Build.props', 'Directory.Build.targets')) {
    $dst = Join-Path $ProjDir $glue
    if (Test-Path $dst) {
        Write-Output "glue present: $dst"
        continue
    }
    $src = Join-Path $ToolchainDir $glue
    if (-not (Test-Path $src)) {
        throw "Missing toolchain glue: $glue is not next to the project AND not in $ToolchainDir. Deploy the repo 'toolchain/' folder first (see scripts/deploy-to-guest.sh)."
    }
    Copy-Item -Path $src -Destination $dst -Force
    Write-Output "copied glue : $src -> $dst"
}

# ---------------------------------------------------------------------------
# 2. Locate the Build Tools MSBuild. We deliberately use the Build Tools copy
#    (not any stray MSBuild from a .NET SDK) so the v143 toolset + the WDK NuGet
#    props resolve consistently.
# ---------------------------------------------------------------------------
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    # Fall back to vswhere in case the install path differs.
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $bt = & $vswhere -products '*' -property installationPath 2>$null |
            Where-Object { $_ -like '*\2022\BuildTools' } | Select-Object -First 1
        if ($bt) {
            $cand = Join-Path $bt.Trim() 'MSBuild\Current\Bin\MSBuild.exe'
            if (Test-Path $cand) { $msbuild = $cand }
        }
    }
}
if (-not (Test-Path $msbuild)) {
    throw "Build Tools MSBuild.exe not found. Run scripts/guest-setup-buildtools.ps1 first."
}
Write-Output "MSBuild     : $msbuild"

# ---------------------------------------------------------------------------
# 3a. Restore. Pulls Microsoft.Windows.WDK.x64 10.0.26100.6584 + its
#     Microsoft.Windows.SDK.CPP.x64 dependency into
#     %USERPROFILE%\.nuget\packages\... . First run is ~1.5 min.
#     RestoreProjectStyle=PackageReference / ResolveNuGetPackages=false come from
#     Directory.Build.props so the modern restore drives the build (the legacy
#     ResolveNuGetPackageAssets task crashes on a native C++ PackageReference).
# ---------------------------------------------------------------------------
Write-Output "`n-- restore --"
& $msbuild $VcxProj /t:restore /p:Configuration=$Configuration /p:Platform=$Platform /m /nologo
if ($LASTEXITCODE -ne 0) {
    throw "restore failed (exit $LASTEXITCODE). If this is 'Sequence contains no elements', verify ResolveNuGetPackages=false is set (Directory.Build.props)."
}

# ---------------------------------------------------------------------------
# 3b. Build. PlatformToolset=v143 (from the .vcxproj). The props add the IddCx
#     include dir ($(WDKContentRoot)Include\...\um\iddcx\<maj>.<min>) and link
#     WdfDriverStubUm.lib + iddcxstub.lib + ntdll.lib. /m = parallel.
# ---------------------------------------------------------------------------
Write-Output "`n-- build --"
& $msbuild $VcxProj /t:build /p:Configuration=$Configuration /p:Platform=$Platform /m /nologo
$buildCode = $LASTEXITCODE
if ($buildCode -ne 0) {
    throw "build failed (exit $buildCode). Common culprits: C1083 IddCx.h (include roots / WDKContentRoot trailing slash), MSB8020 (PlatformToolset still WindowsUserModeDriver10.0), unresolved __imp_DbgPrintEx (ntdll.lib missing), or a too-old vendored WDF (repoint to the WDK NuGet WDF, e.g. 2.25)."
}

# ---------------------------------------------------------------------------
# 4. Find and report the produced .dll. The output normally lands under
#    <ProjDir>\x64\<Configuration>\ (or a project-specific OutDir). We search
#    the configuration output tree and report the newest .dll.
# ---------------------------------------------------------------------------
Write-Output "`n-- output --"
$searchRoots = @(
    (Join-Path $ProjDir "$Platform\$Configuration"),
    (Join-Path $ProjDir $Configuration),
    $ProjDir
) | Where-Object { Test-Path $_ } | Select-Object -Unique

$dll = $null
foreach ($root in $searchRoots) {
    $dll = Get-ChildItem -Path $root -Filter '*.dll' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($dll) { break }
}

if ($dll) {
    Write-Output ("DLL: {0}  ({1:N0} bytes, {2})" -f $dll.FullName, $dll.Length, $dll.LastWriteTime)
    # Emit the bare path on the last line so callers can capture it easily.
    Write-Output $dll.FullName
} else {
    Write-Output "WARNING: build reported success but no .dll was found under $($searchRoots -join '; '). Check the project's OutDir."
}

Write-Output '== build done. Next: scripts/guest-package-install.ps1 to sign + install. =='
exit $buildCode
