#!/usr/bin/env bash
#
# deploy-to-guest.sh — push files from this repo into the Windows build/test
# guest over qrexec, byte-faithfully, and (optionally) kick off the guest build.
#
# Run this ON the Qubes mgmtvm (AdminVM). It is the dev-loop glue: edit here in
# the git-versioned repo (source of truth) -> deploy into the guest -> build in
# the guest. See ../docs/build-toolchain.md and ../docs/install-and-debug.md.
#
# WHY base64-on-stdin and not base64-as-an-argument:
#   The qrexec command line has a length limit. base64 of any non-trivial file
#   blows past it. So we stream the base64 payload on STDIN and have the guest
#   PowerShell read it with [Console]::In.ReadToEnd(), Convert.FromBase64String,
#   and [IO.File]::WriteAllBytes(). That is exact-bytes (no CRLF translation, no
#   encoding mangling) and has no size ceiling.
#
#   The PowerShell command ITSELF (which is short and fixed) is still passed via
#   -EncodedCommand: we UTF-16LE + base64 the script text so quoting/escaping
#   between bash, qrexec, and PowerShell can never bite us.
#
# USAGE:
#   # push a single file to an explicit guest path:
#   ./deploy-to-guest.sh <qube> <local-path> <guest-path>
#
#   # push a directory tree; each file lands under <guest-dir> mirroring the
#   # tree rooted at <local-dir>:
#   ./deploy-to-guest.sh <qube> <local-dir> <guest-dir>
#
#   # default qube is "romhacking-hma-driver"; you may omit it:
#   ./deploy-to-guest.sh <local-path> <guest-path>
#
#   # after pushing, also run the guest build (invokes guest-build.ps1 in-guest);
#   # --build forwards -ProjDir/-VcxProj through to guest-build.ps1 (which
#   # requires them). They default to C:\dev\LGIdd and LGIdd.vcxproj:
#   ./deploy-to-guest.sh [--build [-ProjDir <dir>] [-VcxProj <proj>]] <...as above...>
#
# EXAMPLES:
#   ./deploy-to-guest.sh toolchain/Directory.Build.props 'C:\dev\LGIdd\Directory.Build.props'
#   ./deploy-to-guest.sh romhacking-hma-driver ./toolchain 'C:\dev\LGIdd'
#   ./deploy-to-guest.sh --build -ProjDir 'C:\dev\LGIdd' -VcxProj 'LGIdd.vcxproj' ./toolchain 'C:\dev\LGIdd'
#   ./deploy-to-guest.sh romhacking-hma-driver ./scripts 'C:\dev\scripts'
#
# This file is tracked in git. After a fresh clone, make it executable:
#   chmod +x scripts/deploy-to-guest.sh
#
set -euo pipefail

# ----------------------------------------------------------------------------
# Defaults / constants
# ----------------------------------------------------------------------------

# Default Windows 10 build+test guest (Win10 Pro 22H2 / build 19045, QWT
# installed, test-signing on). Override with the optional first positional arg.
DEFAULT_QUBE="romhacking-hma-driver"

# Where guest-build.ps1 is expected to live in the guest once deployed, and the
# default build project root in the guest. The --build flag runs guest-build.ps1
# which itself knows the .vcxproj to build (fix 1: build the .vcxproj, not the
# .sln). Adjust GUEST_BUILD_SCRIPT if you stage scripts elsewhere.
GUEST_BUILD_SCRIPT='C:\dev\scripts\guest-build.ps1'

# ----------------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------------

DO_BUILD=0

# Project args forwarded to guest-build.ps1 when --build is given. guest-build.ps1
# resolves a relative -VcxProj against -ProjDir, so these defaults match its own
# defaults and the canonical invocation:
#   guest-build.ps1 -ProjDir "C:\dev\LGIdd" -VcxProj "LGIdd.vcxproj"
BUILD_PROJDIR='C:\dev\LGIdd'
BUILD_VCXPROJ='LGIdd.vcxproj'

# Pull an optional leading --build flag (allowed anywhere before positionals),
# plus the project args it forwards to guest-build.ps1.
ARGS=()
while [[ $# -gt 0 ]]; do
  a="$1"
  case "$a" in
    --build) DO_BUILD=1 ;;
    -ProjDir) [[ $# -ge 2 ]] || { echo "deploy-to-guest.sh: -ProjDir needs a value" >&2; exit 2; }; BUILD_PROJDIR="$2"; shift ;;
    -ProjDir=*) BUILD_PROJDIR="${a#*=}" ;;
    -VcxProj) [[ $# -ge 2 ]] || { echo "deploy-to-guest.sh: -VcxProj needs a value" >&2; exit 2; }; BUILD_VCXPROJ="$2"; shift ;;
    -VcxProj=*) BUILD_VCXPROJ="${a#*=}" ;;
    -h|--help)
      # Print the usage header (everything between the shebang and `set -e`).
      sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; s/^#$//'
      exit 0
      ;;
    --*)
      echo "deploy-to-guest.sh: unknown flag: $a" >&2
      exit 2
      ;;
    *) ARGS+=("$a") ;;
  esac
  shift
done

# Decide whether a qube name was given. We accept either
#   <qube> <local> <guest>   (3 positionals)
# or
#   <local> <guest>          (2 positionals, qube defaults)
case "${#ARGS[@]}" in
  3)
    QUBE="${ARGS[0]}"
    LOCAL="${ARGS[1]}"
    GUEST="${ARGS[2]}"
    ;;
  2)
    QUBE="$DEFAULT_QUBE"
    LOCAL="${ARGS[0]}"
    GUEST="${ARGS[1]}"
    ;;
  *)
    echo "usage: deploy-to-guest.sh [--build] [<qube>] <local-path> <guest-path>" >&2
    echo "       (default qube: $DEFAULT_QUBE; run with --help for details)"     >&2
    exit 2
    ;;
esac

# Strip any trailing slash from $LOCAL so the directory-walk's "${f#"$LOCAL"/}"
# relative-path computation stays correct (mirrors the GUEST trailing-backslash
# strip done just before the tree walk).
LOCAL="${LOCAL%/}"

if [[ ! -e "$LOCAL" ]]; then
  echo "deploy-to-guest.sh: local path does not exist: $LOCAL" >&2
  exit 1
fi

# ----------------------------------------------------------------------------
# Helper: run a PowerShell command in the guest
# ----------------------------------------------------------------------------
#
# We UTF-16LE + base64 the script text and pass it as -EncodedCommand so no
# layer (bash -> qrexec -> cmd -> powershell) can mangle quoting. We always
# prepend $ProgressPreference='SilentlyContinue' to suppress the CLIXML progress
# stream that otherwise pollutes --pass-io stdout.
#
# Any STDIN given to this function is passed through to the guest process, which
# is how the file payload reaches [Console]::In.ReadToEnd().
#
# Usage:
#   guest_ps '<powershell text>'                 # no stdin payload
#   printf %s "$payload" | guest_ps '<ps text>'  # payload on stdin
guest_ps() {
  local ps="$1"
  local full="\$ProgressPreference='SilentlyContinue'; ${ps}"
  local enc
  enc="$(printf %s "$full" | iconv -t UTF-16LE | base64 -w0)"
  # --pass-io: wire stdin/stdout/stderr through; --no-gui: no GUI prompt.
  qvm-run --pass-io --no-gui "$QUBE" "powershell -NoProfile -EncodedCommand $enc"
}

# ----------------------------------------------------------------------------
# Helper: push one local file to one absolute guest path (byte-faithful)
# ----------------------------------------------------------------------------
#
# The guest-side PowerShell:
#   1. reads the entire base64 payload from stdin,
#   2. ensures the destination's parent directory exists,
#   3. WriteAllBytes the decoded bytes (no newline translation).
#
# $dst is a Windows path (e.g. C:\dev\LGIdd\Driver.cpp). We embed it as a
# single-quoted PowerShell literal, so backslashes are preserved verbatim and a
# guest path must not itself contain a single quote (Windows paths here never
# do).
push_file() {
  local src="$1" dst="$2"

  # Guard against single quotes in the guest path (would break the PS literal).
  if [[ "$dst" == *\'* ]]; then
    echo "deploy-to-guest.sh: guest path contains a single quote, unsupported: $dst" >&2
    exit 1
  fi

  local ps
  ps="\$b64=[Console]::In.ReadToEnd();"
  ps+="\$dst='${dst}';"
  ps+="\$dir=Split-Path -Parent \$dst;"
  ps+="if(\$dir -and -not (Test-Path \$dir)){New-Item -ItemType Directory -Force -Path \$dir | Out-Null};"
  ps+="[IO.File]::WriteAllBytes(\$dst,[Convert]::FromBase64String(\$b64));"
  ps+="Write-Output \"wrote \$dst (\$(([Convert]::FromBase64String(\$b64)).Length) bytes)\""

  # Stream the file's base64 (single line, -w0) into the guest on stdin.
  base64 -w0 "$src" | guest_ps "$ps"
}

# ----------------------------------------------------------------------------
# Deploy: single file or directory tree
# ----------------------------------------------------------------------------

if [[ -f "$LOCAL" ]]; then
  # Single file -> the guest path is taken literally as the destination file.
  echo ">> push file: $LOCAL  ->  [$QUBE] $GUEST"
  push_file "$LOCAL" "$GUEST"

elif [[ -d "$LOCAL" ]]; then
  # Directory -> walk every regular file and mirror the relative tree under the
  # guest directory. We compute each file's path relative to $LOCAL, convert
  # the relative '/'-separators to Windows '\', and join onto $GUEST.
  #
  # Strip any trailing slash from $GUEST so we don't produce "dir\\sub".
  GUEST="${GUEST%\\}"
  echo ">> push tree: $LOCAL/  ->  [$QUBE] $GUEST\\"

  # -print0 / read -d '' to survive spaces and odd chars in local filenames.
  while IFS= read -r -d '' f; do
    rel="${f#"$LOCAL"/}"             # path relative to the source root
    rel_win="${rel//\//\\}"          # POSIX '/' -> Windows '\'
    dst="${GUEST}\\${rel_win}"       # absolute guest destination
    echo "   $rel  ->  $dst"
    push_file "$f" "$dst"
  done < <(find "$LOCAL" -type f -print0)

else
  echo "deploy-to-guest.sh: local path is neither a file nor a directory: $LOCAL" >&2
  exit 1
fi

# ----------------------------------------------------------------------------
# Optional: kick off the guest build after deploying
# ----------------------------------------------------------------------------
#
# Delegates to guest-build.ps1 in the guest (it owns the Build Tools MSBuild
# invocation: /t:restore then /t:build /p:Configuration=Release /p:Platform=x64
# /m). We surface its exit code so a build failure fails this script too.
if [[ "$DO_BUILD" -eq 1 ]]; then
  echo ">> build: invoking $GUEST_BUILD_SCRIPT -ProjDir '$BUILD_PROJDIR' -VcxProj '$BUILD_VCXPROJ' in [$QUBE]"
  for v in "$GUEST_BUILD_SCRIPT" "$BUILD_PROJDIR" "$BUILD_VCXPROJ"; do
    if [[ "$v" == *\'* ]]; then
      echo "deploy-to-guest.sh: build arg contains a single quote, unsupported: $v" >&2
      exit 1
    fi
  done
  # Run the build script in-guest, forwarding the mandatory -ProjDir/-VcxProj
  # (guest-build.ps1 requires them). -File would also work, but we keep the
  # EncodedCommand path for one consistent invocation mechanism. We embed the
  # paths as single-quoted PowerShell literals so backslashes survive verbatim.
  # The trailing 'exit $LASTEXITCODE' propagates MSBuild's result out through
  # qvm-run.
  guest_ps "& '${GUEST_BUILD_SCRIPT}' -ProjDir '${BUILD_PROJDIR}' -VcxProj '${BUILD_VCXPROJ}'; exit \$LASTEXITCODE"
fi

echo ">> done."
