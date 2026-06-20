<#
.SYNOPSIS
  Package, test-sign, and install the built IddCx UMDF driver in the guest, then
  report the resulting PnP device status / problem code.

.DESCRIPTION
  Run this INSIDE the guest (romhacking-hma-driver), which must already have
  test-signing ON (bcdedit /set testsigning on; reboot — see
  docs/install-and-debug.md). This implements the M0b recipe proven during
  bring-up, with each gotcha called out inline:

    (a) get/make a self-signed code-signing cert CN=QubesIDDTest in
        LocalMachine\My and import it into BOTH LocalMachine\Root and
        LocalMachine\TrustedPublisher.
    (b) stage the built .dll + .inf into C:\dev\pkg.
    (c) substitute the INF's $UMDFVERSION$ token (the v143 build does NOT
        substitute it) with -UmdfVer (default 2.25.0), then run stampinf to
        substitute $ARCH$ and set DriverVer.
    (d) Inf2Cat to generate the .cat (note: Inf2Cat.exe is x86-ONLY).
    (e) signtool sign /fd sha256 /sm /sha1 <thumbprint> the .cat AND the .dll.
    (f) pnputil /add-driver <inf> /install to stage the driver.
    (g) devcon install <inf> <hwid> to create+bind the root-enumerated node.

  Then it prints Get-PnpDevice status and any device problem code.

  HONEST CAVEAT: on Win10 a driver whose effective IddCx client version exceeds
  the OS framework (Win10's ceiling is IddCx 1.5; a 1.10-built driver is the case
  we hit) binds but then shows problem code 31 / UMDF host error 0xD000000D (the
  IddCx version gate). That is a *findings* result, not a script bug — see
  docs/findings.md (F1). This script installs whatever you built; it does not
  police the IddCx version.
#>
[CmdletBinding()]
param(
    # Directory containing the build output and the .inf.
    [Parameter(Mandatory = $true)]
    [string]$ProjDir,

    # Release output subdir (relative to $ProjDir) holding the built .dll.
    [string]$RelDir = 'x64\Release',

    # The .inf filename (looked up under $ProjDir, then $ProjDir\$RelDir).
    [Parameter(Mandatory = $true)]
    [string]$Inf,

    # The root-enumerated hardware ID, e.g. Root\MttVDD. NOTE: the INF hwid line
    # may carry a trailing inline comment ("Root\MttVDD ; TODO ..."); pass ONLY
    # the bare hwid here — do not include the comment.
    [Parameter(Mandatory = $true)]
    [string]$Hwid,

    # UMDF version string substituted for the INF's $UMDFVERSION$ token. The
    # driver links WdfFunctions_02025 = UMDF 2.25, so the INF must say 2.25.0.
    [string]$UmdfVer = '2.25.0',

    # Where to stage the signed package.
    [string]$PkgDir = 'C:\dev\pkg',

    # Cert subject used for the self-signed test code-signing cert.
    [string]$CertSubject = 'CN=QubesIDDTest'
)

$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'

Write-Output '== guest-package-install.ps1 =='

# ---------------------------------------------------------------------------
# Tool paths. EXACT paths proven during bring-up.
#   * stampinf / Inf2Cat / devgen come from the WDK NuGet 'c\bin' | 'c\tools'.
#   * Inf2Cat.exe is x86-ONLY: it lives under c\bin\...\x86, NOT x64.
#   * signtool.exe comes from the INSTALLED base SDK (the WDK NuGet has none).
#   * devcon.exe ships in the VDD control package at C:\vdd\Dependencies.
# ---------------------------------------------------------------------------
$WdkVer   = '10.0.26100.0'
$NuGetPkg = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584\c'

$Stampinf = Join-Path $NuGetPkg "bin\$WdkVer\x64\stampinf.exe"
$Inf2Cat  = Join-Path $NuGetPkg "bin\$WdkVer\x86\Inf2Cat.exe"   # x86-only!
$Signtool = "C:\Program Files (x86)\Windows Kits\10\bin\$WdkVer\x64\signtool.exe"
$Devcon   = 'C:\vdd\Dependencies\devcon.exe'

foreach ($t in @($Stampinf, $Inf2Cat, $Signtool, $Devcon)) {
    if (-not (Test-Path $t)) {
        throw "Required tool not found: $t . (stampinf/Inf2Cat: build once so the WDK NuGet is restored; signtool: install the base SDK via guest-setup-buildtools.ps1; devcon: from the VDD control package at C:\vdd.)"
    }
}

# ---------------------------------------------------------------------------
# Resolve source .dll + .inf.
# ---------------------------------------------------------------------------
if (-not (Test-Path $ProjDir)) { throw "ProjDir not found: $ProjDir" }
$ProjDir = (Resolve-Path $ProjDir).Path
$OutDir  = Join-Path $ProjDir $RelDir
if (-not (Test-Path $OutDir)) { throw "Build output dir not found: $OutDir (build first with guest-build.ps1)" }

$dll = Get-ChildItem -Path $OutDir -Filter '*.dll' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $dll) { throw "No .dll found in $OutDir. Did the build succeed?" }
Write-Output "DLL : $($dll.FullName)"

# The .inf may live next to the source (ProjDir) or in the output dir.
$srcInf = $null
foreach ($cand in @((Join-Path $ProjDir $Inf), (Join-Path $OutDir $Inf))) {
    if (Test-Path $cand) { $srcInf = (Resolve-Path $cand).Path; break }
}
if (-not $srcInf) { throw "INF '$Inf' not found in $ProjDir or $OutDir." }
Write-Output "INF : $srcInf"

# ===========================================================================
# (a) Self-signed code-signing cert: get-or-create, then trust it.
# ---------------------------------------------------------------------------
# GOTCHA (i): signtool /s My searches the CURRENTUSER store; a cert in
# LocalMachine yields "No certificates were found". We therefore create the cert
# in Cert:\LocalMachine\My and sign with /sm (machine store) + /sha1 thumbprint.
# We import the cert into LocalMachine\Root (so the chain validates) AND
# LocalMachine\TrustedPublisher (so the unattended driver install does not pop
# the "Windows can't verify the publisher" dialog).
# ===========================================================================
Write-Output "`n-- cert --"
$cert = Get-ChildItem Cert:\LocalMachine\My |
    Where-Object { $_.Subject -eq $CertSubject } |
    Sort-Object NotAfter -Descending | Select-Object -First 1

if (-not $cert) {
    Write-Output "Creating self-signed code-signing cert $CertSubject in LocalMachine\My"
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertSubject `
        -CertStoreLocation 'Cert:\LocalMachine\My' `
        -KeyExportPolicy Exportable
} else {
    Write-Output "Reusing existing cert (thumbprint $($cert.Thumbprint))"
}
$thumb = $cert.Thumbprint
Write-Output "Thumbprint: $thumb"

# Export the public cert and import into Root + TrustedPublisher (idempotent:
# re-importing the same cert just overwrites it).
$cerPath = Join-Path $PkgDir 'QubesIDDTest.cer'
New-Item -ItemType Directory -Force -Path $PkgDir | Out-Null
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
foreach ($store in @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')) {
    Import-Certificate -FilePath $cerPath -CertStoreLocation $store | Out-Null
    Write-Output "Imported cert into $store"
}

# ===========================================================================
# (b) Stage .dll + .inf into the package dir (clean it first so a stale .cat or
#     old token-substituted INF can't be picked up).
# ===========================================================================
Write-Output "`n-- stage -> $PkgDir --"
Get-ChildItem -Path $PkgDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in @('.dll', '.inf', '.cat') } |
    Remove-Item -Force -ErrorAction SilentlyContinue

Copy-Item -Path $dll.FullName -Destination $PkgDir -Force
$stagedInf = Join-Path $PkgDir (Split-Path -Leaf $srcInf)
Copy-Item -Path $srcInf -Destination $stagedInf -Force
Write-Output "Staged: $($dll.Name), $(Split-Path -Leaf $stagedInf)"

# ===========================================================================
# (c) Substitute the $UMDFVERSION$ token, then stampinf.
# ---------------------------------------------------------------------------
# GOTCHA (ii): the v143 build does NOT substitute the INF token
# UmdfLibraryVersion=$UMDFVERSION$ (that substitution is a function of the
# missing WindowsUserModeDriver10.0 toolset). Left as the literal "$UMDFVERSION$"
# the UMDF co-installer fails with 0x57 (ERROR_INVALID_PARAMETER) at
# DIF_INSTALLINTERFACES (visible in C:\Windows\INF\setupapi.dev.log). So we
# replace the literal with the real UMDF version BEFORE stampinf/Inf2Cat.
# stampinf then handles $ARCH$ and writes the DriverVer.
# ===========================================================================
Write-Output "`n-- INF token substitution --"
$infText = [IO.File]::ReadAllText($stagedInf)
if ($infText -match '\$UMDFVERSION\$') {
    $infText = $infText.Replace('$UMDFVERSION$', $UmdfVer)
    [IO.File]::WriteAllText($stagedInf, $infText)
    Write-Output "Replaced `$UMDFVERSION`$ with $UmdfVer"
} else {
    Write-Output 'No $UMDFVERSION$ token present (already substituted?) — leaving INF as-is.'
}

# stampinf: -d * = auto date; -v 1.0.0.0 = version; -a amd64 substitutes $ARCH$.
Write-Output "`n-- stampinf --"
& $Stampinf -f $stagedInf -d '*' -v '1.0.0.0' -a 'amd64'
if ($LASTEXITCODE -ne 0) { throw "stampinf failed (exit $LASTEXITCODE)" }

# ===========================================================================
# (d) Inf2Cat -> .cat. /os:10_X64 targets Windows 10 x64. Inf2Cat is x86-only.
# ===========================================================================
Write-Output "`n-- Inf2Cat --"
& $Inf2Cat "/driver:$PkgDir" '/os:10_X64'
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat failed (exit $LASTEXITCODE). Check that the staged INF + DLL filenames match the INF's CopyFiles/SourceDisksFiles." }
$cat = Get-ChildItem -Path $PkgDir -Filter '*.cat' | Select-Object -First 1
if (-not $cat) { throw "Inf2Cat produced no .cat in $PkgDir" }
Write-Output "CAT : $($cat.FullName)"

# ===========================================================================
# (e) Sign the .cat AND the .dll.
# ---------------------------------------------------------------------------
# GOTCHA (i) again: /sm makes signtool use the MACHINE store; /sha1 <thumb>
# selects our cert precisely (avoids picking a wrong cert when several exist).
# An UNSIGNED .cat triggers the "Windows can't verify the publisher of this
# driver software" dialog at install — so we sign before pnputil/devcon.
# ===========================================================================
Write-Output "`n-- signtool --"
foreach ($f in @($cat.FullName, (Join-Path $PkgDir $dll.Name))) {
    & $Signtool sign /fd sha256 /sm /sha1 $thumb $f
    if ($LASTEXITCODE -ne 0) { throw "signtool failed on $f (exit $LASTEXITCODE)" }
    Write-Output "signed: $f"
}

# ===========================================================================
# (f) Stage the driver into the driver store.
# ===========================================================================
Write-Output "`n-- pnputil /add-driver /install --"
& pnputil.exe /add-driver $stagedInf /install
$pnpCode = $LASTEXITCODE
# pnputil returns 0 on success, 3010 = success+reboot. Treat both as OK.
if ($pnpCode -notin @(0, 3010)) {
    Write-Output "WARNING: pnputil exited $pnpCode (continuing to devcon; check C:\Windows\INF\setupapi.dev.log)."
}

# ===========================================================================
# (g) Create + bind the root-enumerated device node.
# ---------------------------------------------------------------------------
# GOTCHA (iii): devgen creates a generic SWD device but does NOT force-install
# our driver (it stays "Generic software device"). devcon install creates the
# node AND binds the driver atomically, which is what we want. Pass the BARE
# hwid (no trailing INF comment).
# ===========================================================================
Write-Output "`n-- devcon install --"
Write-Output "devcon install `"$stagedInf`" `"$Hwid`""
& $Devcon install $stagedInf $Hwid
$devconCode = $LASTEXITCODE
if ($devconCode -ne 0) {
    Write-Output "WARNING: devcon install exited $devconCode (a node may still have been created; inspecting status below)."
}

# ===========================================================================
# Report: PnP device status + problem code.
# ---------------------------------------------------------------------------
# A bound device with problem code 31 / UMDF host error 0xD000000D is the IddCx
# version gate (F1): expected when the build targeted IddCx > 1.4 on Win10. To
# capture the UMDF host load failure detail:
#   wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true
#   <DEVCON> restart <hwid>   # then read that operational log
# ===========================================================================
Write-Output "`n-- device status --"
Start-Sleep -Seconds 2  # let PnP settle so Get-PnpDevice reflects the bind
$devs = Get-PnpDevice -InstanceId "$Hwid*" -ErrorAction SilentlyContinue
if (-not $devs) {
    # Fall back to a broad match on the hwid's leaf (e.g. MttVDD).
    $leaf = ($Hwid -split '\\')[-1]
    $devs = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like "*$leaf*" }
}
if ($devs) {
    foreach ($d in $devs) {
        $problem = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
        Write-Output ("InstanceId : {0}" -f $d.InstanceId)
        Write-Output ("Status     : {0}" -f $d.Status)
        Write-Output ("Class      : {0}" -f $d.Class)
        Write-Output ("ProblemCode: {0}" -f $(if ($null -ne $problem) { $problem } else { '(none)' }))
        if ($problem -eq 31) {
            Write-Output "NOTE: problem code 31 (UMDF host error 0xD000000D) = the IddCx version gate. On Win10 (IddCx framework 1.5) this means the driver's effective client version is higher than 1.5 (e.g. a 1.10-built driver). See docs/findings.md F1 (keep the effective IddCx client version <= 1.5: link the matching iddcxstub and keep IDDCX_MINIMUM_VERSION_REQUIRED low)."
        }
    }
} else {
    Write-Output "No device matching '$Hwid' found. devcon may have failed; check C:\Windows\INF\setupapi.dev.log."
}

Write-Output '== package/install done. =='
