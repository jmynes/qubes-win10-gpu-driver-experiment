# Install and debug

The package / sign / install recipe for the built driver, and the debugging
procedures for the failures hit during bring-up. Everything here was run on the
test guest `romhacking-hma-driver` (Windows 10 Pro 22H2, build 19045;
test-signing ON; `bcdedit /set groupsize 2` set) and is the proven M0b recipe.

This doc covers the steps *after* the build. For how the `.dll` is built (Build
Tools + WDK-via-NuGet, the toolset / stub-lib fixes), see
[`build-toolchain.md`](build-toolchain.md). For the technical findings that
explain *why* some of these steps matter (notably the IddCx 1.4 ceiling behind
problem code 31), see [`findings.md`](findings.md). For the milestone context,
see [`plan.md`](plan.md).

The whole recipe is automated in
[`../scripts/guest-package-install.ps1`](../scripts/guest-package-install.ps1);
this doc is the human-readable explanation and the troubleshooting reference for
when that script fails.

> The skeleton this was proven against is the MttVDD build from M0a (`MttVDD.dll`,
> root hardware ID `Root\MttVDD`). Per [F2](findings.md#f2--mttvdd-virtual-display-driver-hdr-edition-is-windows-11-only)
> MttVDD is not the fork base, but it was a valid vehicle for proving the
> package/sign/install/load chain end to end. The LGIdd fork (M1) reuses the same
> recipe with its own `.dll` / INF / hardware ID.

---

## Tool locations

These paths are exact and several are non-obvious. `<wdknuget>` below is the
extracted WDK NuGet root:

```
%USERPROFILE%\.nuget\packages\microsoft.windows.wdk.x64\10.0.26100.6584
```

and the real WDK content lives under its `c\` subfolder (so `<wdknuget>\c\bin`,
`<wdknuget>\c\Lib`, `<wdknuget>\c\tools`).

| Tool | Path | Notes |
|------|------|-------|
| `stampinf.exe` | `<wdknuget>\c\bin\10.0.26100.0\x64\stampinf.exe` | x64 dir is fine. |
| `Inf2Cat.exe` | `<wdknuget>\c\bin\10.0.26100.0\x86\Inf2Cat.exe` | **x86 ONLY** — there is *no* `Inf2Cat.exe` in the `x64` bin dir. Use the `x86` path. |
| `signtool.exe` | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe` | From the **installed SDK**, not the WDK NuGet. |
| `devcon.exe` | `C:\vdd\Dependencies\devcon.exe` | From the VDD control package. Used for the root-enumerated install (creates + binds atomically). |
| `devgen.exe` | `<wdknuget>\c\tools\10.0.26100.0\x64\devgen.exe` | From WDK tools. **Not used in the final recipe** — see [gotcha (iii)](#iii-devgen-vs-devcon--the-hardware-id-inline-comment-trap). |

---

## The recipe

Step-by-step. Each `<...>` is a placeholder; substitute the real driver's
`.dll` / `.inf` / hardware ID.

### (a) Code-signing certificate

Make (once) a self-signed code-signing cert, then trust it in both the machine
**Root** and **TrustedPublisher** stores so Windows will accept the catalog
without the publisher-verification dialog:

```powershell
$cert = New-SelfSignedCertificate `
  -Type CodeSigningCert `
  -Subject "CN=QubesIDDTest" `
  -CertStoreLocation Cert:\LocalMachine\My `
  -KeyExportPolicy Exportable

# export, then import into BOTH machine stores:
Export-Certificate -Cert $cert -FilePath C:\dev\QubesIDDTest.cer
Import-Certificate -FilePath C:\dev\QubesIDDTest.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath C:\dev\QubesIDDTest.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

Keep the thumbprint (`$cert.Thumbprint`) — the signing step references the cert
by `/sha1 <thumbprint>` to avoid the store-search trap in
[gotcha (i)](#i-signtool-store-selection--the-publisher-cannot-be-verified-dialog).

### (b) Stage the package

Copy the built `<driver>.dll` and its `<driver>.inf` into a clean package folder
(staging convention: `C:\dev\pkg`).

```powershell
New-Item -ItemType Directory -Force C:\dev\pkg | Out-Null
Copy-Item C:\dev\<project>\x64\Release\<driver>.dll C:\dev\pkg\
Copy-Item C:\dev\<project>\<driver>.inf            C:\dev\pkg\
```

### (c) Substitute the `$UMDFVERSION$` token

The INF line `UmdfLibraryVersion=$UMDFVERSION$` is **not** substituted by the
v143 build (that substitution is a job of the legacy WDK driver toolset, which we
are not using — see [`build-toolchain.md`](build-toolchain.md)). Replace the
literal token by hand with the UMDF version the driver actually links:

```powershell
(Get-Content C:\dev\pkg\<driver>.inf) `
  -replace [regex]::Escape('$UMDFVERSION$'), '2.25.0' |
  Set-Content C:\dev\pkg\<driver>.inf
```

`2.25.0` is correct because the driver links `WdfFunctions_02025` (UMDF 2.25).
Leaving the literal `$UMDFVERSION$` in place causes the install-time failure in
[gotcha (ii)](#ii-unsubstituted-umdfversion--co-installer-0x57-at-dif_installinterfaces).

### (d) `stampinf` — substitute `$ARCH$` and set DriverVer

```
<wdknuget>\c\bin\10.0.26100.0\x64\stampinf.exe -f C:\dev\pkg\<driver>.inf -d * -v 1.0.0.0 -a amd64
```

This substitutes the `$ARCH$` token (to `amd64`) and stamps `DriverVer`. (The
`$UMDFVERSION$` token from step (c) is *not* handled by `stampinf` — that is why
(c) is a separate manual step.)

### (e) `Inf2Cat` — generate the catalog

Note the **x86** path:

```
<wdknuget>\c\bin\10.0.26100.0\x86\Inf2Cat.exe /driver:C:\dev\pkg /os:10_X64
```

This produces `<driver>.cat` in the package folder.

### (f) `signtool` — sign the `.cat` AND the `.dll`

Sign by thumbprint against the **machine** store (`/sm`). Both files must be
signed:

```
signtool sign /fd sha256 /sm /sha1 <thumbprint> C:\dev\pkg\<driver>.cat
signtool sign /fd sha256 /sm /sha1 <thumbprint> C:\dev\pkg\<driver>.dll
```

`signtool` is the installed-SDK one (see tool table). See
[gotcha (i)](#i-signtool-store-selection--the-publisher-cannot-be-verified-dialog)
for why `/sm` + `/sha1` and not `/s My`.

### (g) `pnputil` — stage the driver into the driver store

```
pnputil /add-driver C:\dev\pkg\<driver>.inf /install
```

### (h) `devcon install` — create + bind the root-enumerated device

```
"C:\vdd\Dependencies\devcon.exe" install C:\dev\pkg\<driver>.inf "Root\MttVDD"
```

`devcon install` creates the root-enumerated device node **and** binds the driver
to it atomically. The hardware ID is exactly `Root\MttVDD` (LGIdd will have its
own). Do **not** use `devgen` here — see
[gotcha (iii)](#iii-devgen-vs-devcon--the-hardware-id-inline-comment-trap).

On success the device enumerates. If it shows a yellow-bang **problem code 31**,
go to [gotcha (iv)](#iv-problem-code-31--0xd000000d--the-iddcx-version-gate).

---

## Gotchas

Each of these was hit for real during M0b. They are ordered (i)–(vi).

### (i) `signtool` store selection + the "publisher cannot be verified" dialog

`signtool sign /s My ...` searches the **CurrentUser** `My` store. The cert from
step (a) was created in **LocalMachine\My**, so `/s My` finds nothing and fails:

```
SignTool Error: No certificates were found that met all the given criteria.
```

Fix: sign against the machine store with `/sm`, and select the cert by
`/sha1 <thumbprint>` to be unambiguous (as in step (f)).

Separately: if the **`.cat` is unsigned** (or signed by a cert not trusted in
`TrustedPublisher`), the install throws the GUI dialog:

> *Windows can't verify the publisher of this driver software.*

That is why step (a) imports the cert into **`TrustedPublisher`** as well as
`Root`. Both stores are required: `Root` makes the chain trusted, `TrustedPublisher`
suppresses the publisher prompt.

### (ii) Unsubstituted `$UMDFVERSION$` → co-installer `0x57` at `DIF_INSTALLINTERFACES`

If step (c) is skipped and the literal `$UMDFVERSION$` survives into the staged
INF, the UMDF co-installer fails during install with:

```
0x57  (ERROR_INVALID_PARAMETER)  at DIF_INSTALLINTERFACES
```

This appears in the setup API device log:

```
C:\Windows\INF\setupapi.dev.log
```

Grep that log for `DIF_INSTALLINTERFACES` / `0x57` to confirm. The fix is simply
step (c): replace `$UMDFVERSION$` with `2.25.0` before staging.

### (iii) `devgen` vs `devcon` + the hardware-ID inline-comment trap

`devgen` creates a generic SWD (software-enumerated) device but does **not**
force-install the driver onto it — the node stays **"Generic software device"**
and never binds the driver. Use **`devcon install`** (step (h)) instead: it
creates the device node *and* installs/binds the driver in one atomic operation.

Hardware-ID trap: the INF hardware-ID line carried a trailing inline comment,
e.g.

```
Root\MttVDD ; TODO: edit hw-id
```

The hardware ID is **only** `Root\MttVDD`. Do not pass the `; TODO: edit hw-id`
comment text into `devcon` / the device node — parse off everything from the `;`
onward.

### (iv) Problem code 31 / `0xD000000D` = the IddCx version gate

After a successful bind, the device showed:

```
device problem code 31
UMDF host load error 0xD000000D
```

This is **not** a packaging bug — it is the **IddCx version gate**. The driver
was built/gated against an IddCx version newer than the 1.4 framework that
Windows 10 ships, so the client stub's `IddClientVersionHigherThanFramework`
check rejects the load.

Full explanation and the fix (compile against 1.10 headers but set
`IDDCX_MINIMUM_VERSION_REQUIRED = 4`, link the 1.4 `iddcxstub`, runtime-guard
any >1.4 API):
[**findings.md → F1**](findings.md#f1--windows-10-is-an-iddcx-14-ceiling).

### (v) Test-signing must be ON

The driver cat is signed with a self-signed (not WHQL) cert, so the machine must
be in test-signing mode for it to load:

```
bcdedit /set testsigning on
# then reboot
```

On the test guest this was **already on**, so it was not a blocker during M0b —
but it is a prerequisite on any fresh machine. (Confirm with `bcdedit` and look
for `testsigning Yes`.)

### (vi) Capturing UMDF load failures — the DriverFrameworks-UserMode operational log

To get the real reason behind a UMDF host load failure (e.g. the `0xD000000D` in
gotcha (iv)), enable the WUDF operational channel, then poke the device and read
the log:

```powershell
# 1. enable the channel
wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true

# 2. force the host to reload the driver
"C:\vdd\Dependencies\devcon.exe" restart <hwid>     # e.g. "Root\MttVDD"

# 3. read the events
wevtutil qe Microsoft-Windows-DriverFrameworks-UserMode/Operational /f:text /rd:true
```

This is where the `IddClientVersionHigherThanFramework` rejection (F1) and other
UMDF host errors actually surface; Device Manager only shows the opaque problem
code.

---

## qrexec dev-loop mechanics (mgmtvm ↔ guest)

The scripts drive the guest from the mgmtvm over `qvm-run`. The robust pattern
(used by all the `scripts/*.ps1` wrappers and
[`../scripts/deploy-to-guest.sh`](../scripts/deploy-to-guest.sh)):

**Run a PowerShell command in the guest** — base64-encode the command as
UTF-16LE and pass it as `-EncodedCommand` (sidesteps every quoting problem):

```bash
PS='$ProgressPreference="SilentlyContinue"; <powershell here>'
B64=$(printf %s "$PS" | iconv -t UTF-16LE | base64 -w0)
qvm-run --pass-io --no-gui <qube> "powershell -NoProfile -EncodedCommand $B64"
```

Always set `$ProgressPreference = "SilentlyContinue"` inside the command to
suppress CLIXML progress noise on stdout.

**Push a file mgmtvm → guest, byte-faithfully** — base64 the file content and
stream it on stdin (avoids command-length limits); the guest decodes it:

```bash
# bash side: stream the file in on stdin
base64 -w0 "$local" | qvm-run --pass-io --no-gui <qube> "powershell -NoProfile -EncodedCommand <enc>"
```

where `<enc>` is the encoded form of a PowerShell snippet that does:

```powershell
$ProgressPreference = "SilentlyContinue"
$b64 = [Console]::In.ReadToEnd()
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
[IO.File]::WriteAllBytes($dest, [Convert]::FromBase64String($b64))
```

**Pull a file guest → mgmtvm** — run a PS that emits
`[Convert]::ToBase64String(...)` of the file on stdout, and `base64 -d` it
locally. This pull is a documented ad-hoc pattern, not yet wrapped in a script
(`deploy-to-guest.sh` only pushes):

```bash
qvm-run --pass-io --no-gui <qube> "powershell -NoProfile -EncodedCommand <enc-that-emits-base64>" | base64 -d > "$local"
```

Guest path conventions in use: the build lives at `C:\dev\<project>`, staging at
`C:\dev\pkg`, the Build Tools bootstrapper was saved at `C:\dev\vs_buildtools.exe`.
