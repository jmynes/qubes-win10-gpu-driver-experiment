<#
.SYNOPSIS
  Ensure VS Build Tools 2022 is present in the Windows build/test guest and has
  the components needed to build the Qubes Win10 IddCx UMDF driver with *Build
  Tools only* (no EWDK, no WDK Visual Studio extension).

.DESCRIPTION
  Run this INSIDE the guest (romhacking-hma-driver). It is idempotent: it locates
  an existing Build Tools 2022 install via vswhere, then runs the bootstrapper in
  `modify` mode to add any missing components. It does NOT install the WDK — the
  WDK is pulled per-build as the NuGet package Microsoft.Windows.WDK.x64
  (10.0.26100.6584) by toolchain/Directory.Build.props at restore time, NOT here.

  Components added (the minimal set proven during M0a bring-up):
    * Microsoft.VisualStudio.Component.VC.Tools.x86.x64
        MSVC v143 toolset (14.44) + MSBuild. The driver builds with
        PlatformToolset=v143 (NOT the legacy WindowsUserModeDriver10.0, which
        only exists when the WDK VS extension is installed — Build Tools cannot
        host it; using it yields MSB8020).
    * Microsoft.VisualStudio.Component.Windows11SDK.26100
        The base Windows 11 SDK 10.0.26100.0. Supplies the installed signtool.exe
        and (crucially) ntdll.lib, which the WDK NuGet package does NOT ship but
        the WDF UMDF stub needs (__imp_DbgPrintEx). The WDK *headers/libs*
        (iddcx/wdf/km) still come from NuGet, not from this base SDK.
    * Microsoft.VisualStudio.Component.VC.ATL
    * Microsoft.VisualStudio.Component.VC.ATLMFC
        ATL/ATLMFC. The driver source includes <atlbase.h>; without ATL the
        compile fails with C1083 cannot open atlbase.h.

  The bootstrapper (vs_buildtools.exe) is expected at C:\dev\vs_buildtools.exe
  (saved there during initial setup). Modify-mode invocation:
    vs_buildtools.exe modify --installPath "<path>" --add <id> ... --quiet --norestart

.NOTES
  Honest caveat: this script touches ONLY the build toolchain. It does not set
  test-signing, groupsize, or anything OS-level — those are separate one-time VM
  prep steps documented in docs/install-and-debug.md.
#>
[CmdletBinding()]
param(
    # The Build Tools bootstrapper saved during initial setup.
    [string]$Bootstrapper = 'C:\dev\vs_buildtools.exe',

    # Expected Build Tools 2022 install root (used if vswhere can't find one).
    [string]$ExpectedInstallPath = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
)

$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'

# The component IDs we require. Order is not significant to the installer.
$RequiredComponents = @(
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',  # MSVC v143 + MSBuild
    'Microsoft.VisualStudio.Component.Windows11SDK.26100', # base SDK 10.0.26100.0
    'Microsoft.VisualStudio.Component.VC.ATL',             # atlbase.h (driver)
    'Microsoft.VisualStudio.Component.VC.ATLMFC'           # ATL/MFC pairing
)

Write-Output '== guest-setup-buildtools.ps1 =='

# ---------------------------------------------------------------------------
# 1. Locate an existing Build Tools 2022 install (vswhere ships with any modern
#    VS installer under the fixed Installer path). If none is found we fall back
#    to the expected path; the bootstrapper's `modify` will fail loudly if that
#    install really is absent, which is the correct signal.
# ---------------------------------------------------------------------------
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installPath = $null

if (Test-Path $vswhere) {
    # -products * so Build Tools (not just full VS) is matched; -property
    # installationPath emits just the root path.
    $found = & $vswhere -products '*' -prerelease -property installationPath 2>$null |
        Where-Object { $_ -like '*\2022\BuildTools' } |
        Select-Object -First 1
    if ($found) { $installPath = $found.Trim() }
}

if (-not $installPath) {
    Write-Output "vswhere did not report a BuildTools install; falling back to expected path."
    $installPath = $ExpectedInstallPath
}
Write-Output "Build Tools install path: $installPath"

if (-not (Test-Path $Bootstrapper)) {
    throw "Bootstrapper not found at $Bootstrapper. Save the VS Build Tools 2022 installer there first (it was staged at C:\dev\vs_buildtools.exe during setup)."
}

# ---------------------------------------------------------------------------
# 2. Report which required components are already present, for the log. We do
#    NOT skip the modify call based on this — `modify` is itself idempotent and
#    the authoritative check, and vswhere's component query can lag a fresh
#    install. We just print the current state for the lab notebook.
# ---------------------------------------------------------------------------
if (Test-Path $vswhere) {
    $present = & $vswhere -products '*' -path $installPath -property 'packages' 2>$null
    foreach ($c in $RequiredComponents) {
        $have = $present -match [regex]::Escape($c)
        Write-Output ("  {0}  {1}" -f ($(if ($have) { '[present]' } else { '[ADD   ]' }), $c))
    }
}

# ---------------------------------------------------------------------------
# 3. Run the bootstrapper in modify mode to add the required components. Adding
#    a component that is already installed is a no-op, so this is safe to re-run.
#    --quiet     : no UI (we're driving this over qrexec, no interactive desktop)
#    --norestart : never reboot; the driver work needs an explicit, controlled
#                  reboot only for test-signing, not for toolchain installs.
# ---------------------------------------------------------------------------
$addArgs = @('modify', '--installPath', $installPath)
foreach ($c in $RequiredComponents) { $addArgs += @('--add', $c) }
$addArgs += @('--quiet', '--norestart')

Write-Output "Running: $Bootstrapper $($addArgs -join ' ')"
$p = Start-Process -FilePath $Bootstrapper -ArgumentList $addArgs -Wait -PassThru
$code = $p.ExitCode

# The VS installer uses 0 = success and 3010 = success-but-reboot-required. Any
# other non-zero is a real failure. We treat 3010 as success (we passed
# --norestart deliberately; a reboot, if ever needed, is the operator's call).
if ($code -eq 0) {
    Write-Output "Build Tools modify succeeded (exit 0)."
} elseif ($code -eq 3010) {
    Write-Output "Build Tools modify succeeded but signals reboot required (exit 3010). Not rebooting (--norestart)."
} else {
    throw "vs_buildtools.exe modify failed with exit code $code. Check %ProgramData%\Microsoft\VisualStudio\Packages\_Instances and the installer logs in %TEMP%\dd_*.log."
}

# ---------------------------------------------------------------------------
# 4. Sanity report: confirm the v143 MSBuild and a 26100 SDK are visible. This
#    is informational; the build script (guest-build.ps1) does the real check.
# ---------------------------------------------------------------------------
$msbuild = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
if (Test-Path $msbuild) {
    Write-Output "MSBuild present: $msbuild"
} else {
    Write-Output "WARNING: MSBuild not found at expected path $msbuild — VCTools may not have installed."
}

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
if (Test-Path $sdkRoot) {
    Write-Output "Installed SDK signtool present: $sdkRoot"
} else {
    Write-Output "NOTE: $sdkRoot not found yet — the base Windows 11 SDK 26100 may still be finalizing, or vswhere reported a different SDK build."
}

Write-Output '== done. WDK comes from NuGet at build time (Microsoft.Windows.WDK.x64 10.0.26100.6584); run guest-build.ps1 next. =='
