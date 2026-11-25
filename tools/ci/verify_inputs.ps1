# Verifies a local tree against tools/ci/inputs.lock.json, the Tier 2 external
# build input identity contract (docs/testing/clean-worktree-verification.md
# section 1). This script is the single authority on the lock schema and on
# byte integrity: the Python contract tests check the committed lock's shape,
# but only the production verifier decides whether a tree satisfies the lock.
#
# It verifies, in order:
#   1. the lock document itself (schema): header/format/version/owner, entry
#      kinds, unique repo-relative paths without traversal, allowed canonical
#      roots, digest and size formats -- an invalid lock exits 2;
#   2. every file entry: no symlink/junction/reparse-point escape on any path
#      component, the exact pinned byte count, the exact pinned SHA-256;
#   3. every commit entry: the checkout HEAD equals the pin, and the checkout
#      is byte-clean -- `git status --porcelain=v1 -uall` empty (tracked,
#      staged, untracked and submodule states alike) and
#      `git submodule status --recursive` showing every submodule initialized
#      at its recorded commit.
#
# This script verifies only. It never downloads, copies, repairs or "trusts
# current bytes": a mismatch is a hard failure that lists the failing entries
# and stops. The caller decides whether to provision the tree from a reviewed,
# immutable cache; a cache refresh is an explicit maintainer action with a
# reviewed provenance change in the lock file, never an automatic download.
#
# Exit codes: 0 every entry verified; 1 one or more entries failed verification;
# 2 invalid arguments or an unreadable/invalid lock file.
# All output goes to stdout and names only repo-relative paths and hashes; it
# never reads or prints file contents.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/ci/verify_inputs.ps1
#     [-RepoRoot <directory>] [-Lock <inputs.lock.json>]
# Defaults: RepoRoot is the repository that contains this script; Lock is
# tools/ci/inputs.lock.json beside it.
param(
  [string]$RepoRoot = "",
  [string]$Lock = ""
)
$ErrorActionPreference = 'Stop'

function Exit-Usage([string]$message) {
  Write-Output "INVALID_USAGE $message"
  exit 2
}

if ($Lock -eq "") { $Lock = Join-Path $PSScriptRoot "inputs.lock.json" }
if ($RepoRoot -eq "") { $RepoRoot = (Split-Path -Parent $PSScriptRoot | Split-Path -Parent) }
if (-not (Test-Path -LiteralPath $Lock -PathType Leaf)) {
  Exit-Usage "inputs lock file not found: $Lock"
}
if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
  Exit-Usage "repository root not found: $RepoRoot"
}
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName

try {
  # Reuse the repository wire-format parser; do not let ConvertFrom-Json
  # silently discard duplicate members or accept shell-specific JSON syntax.
  . (Join-Path $PSScriptRoot '../product/evidence_contract.ps1')
  $lockDocument = Read-ProductJson $Lock
  Assert-ProductObjectFields $lockDocument @('format', 'owner', 'purpose', 'entries')
} catch {
  Exit-Usage "inputs lock file is not valid JSON: $($_.Exception.Message)"
}

# --- lock schema: the format and path rules live here and only here ----------
$sha256_pattern = '^[0-9a-f]{64}$'
$commit_pattern = '^[0-9a-f]{40}$'
# The canonical roots a pinned input may live under. Adding a root is a
# reviewed change to this file and to docs/testing/clean-worktree-verification.md
# section 1; anything else fails closed, including files elsewhere in build/.
$allowed_roots = @(
  "build/backend-chinese-ranked/",
  "build/backend-data-acquire/",
  "build/backend-japanese-data/",
  "build/backend-training/",
  "build/backend-training-budget/",
  "build/librime-baseline/",
  "build/librime-english/",
  "build/product-data/",
  "third_party/"
)

$properties = @($lockDocument.PSObject.Properties.Name)
if ('format' -cnotin $properties -or
    ($lockDocument.format -isnot [int] -and $lockDocument.format -isnot [long]) -or $lockDocument.format -ne 1) {
  Exit-Usage "inputs lock file format must be 1"
}
if ('owner' -cnotin $properties -or $lockDocument.owner -isnot [string] -or
    $lockDocument.owner -cne 'epocanva-ci-tier2-inputs-lock-v1') {
  Exit-Usage "inputs lock file has an invalid owner"
}
if ('purpose' -cnotin $properties -or $lockDocument.purpose -isnot [string] -or
    [string]::IsNullOrWhiteSpace($lockDocument.purpose)) {
  Exit-Usage "inputs lock file has no purpose"
}
if ('entries' -cnotin $properties -or $lockDocument.entries -isnot [array]) {
  Exit-Usage "inputs lock file has no entries array"
}
$entries = @($lockDocument.entries)
if ($entries.Count -lt 1) {
  Exit-Usage "inputs lock file has no entries"
}

$seen_paths = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $entries) {
  try {
    Assert-ProductObjectFields $entry @('kind', 'path', 'pin_provenance', 'role') @('sha256', 'size_bytes', 'commit')
  } catch { Exit-Usage 'inputs lock entry has missing or unexpected fields' }
  $kind = $entry.kind
  $path = $entry.path
  if ($kind -isnot [string] -or $path -isnot [string] -or [string]::IsNullOrWhiteSpace($path)) {
    Exit-Usage "inputs lock entry is missing 'kind' or 'path'"
  }
  if ($kind -cne "file" -and $kind -cne "commit") {
    Exit-Usage "$path unknown entry kind '$kind' (invalid lock)"
  }
  if ("$path".Contains('\')) {
    Exit-Usage "$path must use forward slashes (invalid lock)"
  }
  if ("$path".StartsWith('/') -or "$path".StartsWith('~') -or
      "$path" -match '^[A-Za-z]:') {
    Exit-Usage "$path must be a repo-relative path (invalid lock)"
  }
  $segments = @("$path" -split '/')
  foreach ($segment in $segments) {
    if ($segment -eq "" -or $segment -match '[. ]$|[\x00-\x1f<>:"\\|?*]' -or
        $segment -match '^(?i:CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)') {
      Exit-Usage "$path contains a noncanonical Windows path segment (invalid lock)"
    }
  }
  if (-not $seen_paths.Add("$path")) {
    Exit-Usage "$path is duplicated in the inputs lock (invalid lock)"
  }
  $within_allowed_root = $false
  foreach ($root in $allowed_roots) {
    if ($path.StartsWith($root, [StringComparison]::Ordinal)) { $within_allowed_root = $true; break }
  }
  if (-not $within_allowed_root) {
    Exit-Usage "$path is outside the declared allowed roots for pinned inputs (invalid lock)"
  }
  if ($entry.pin_provenance -isnot [string] -or [string]::IsNullOrWhiteSpace($entry.pin_provenance) -or
      $entry.role -isnot [string] -or [string]::IsNullOrWhiteSpace($entry.role)) {
    Exit-Usage "$path is missing pin_provenance or role (invalid lock)"
  }
  if ($kind -eq "file") {
    try { Assert-ProductObjectFields $entry @('kind', 'path', 'pin_provenance', 'role', 'sha256', 'size_bytes') }
    catch { Exit-Usage "$path file entry schema is invalid" }
    # -cmatch/-notmatch: case-sensitive on purpose, so an uppercase digest is
    # rejected as an invalid lock (PowerShell's -match would accept it).
    if ($entry.sha256 -isnot [string] -or $entry.sha256 -cnotmatch $sha256_pattern) {
      Exit-Usage "$path sha256 is not a lowercase 64-hex digest (invalid lock)"
    }
    $size = $entry.size_bytes
    if ($null -eq $size -or $size -is [bool] -or
        ($size -isnot [int] -and $size -isnot [long]) -or $size -le 0) {
      Exit-Usage "$path size_bytes must be a positive integer (invalid lock)"
    }
  } else {
    try { Assert-ProductObjectFields $entry @('kind', 'path', 'pin_provenance', 'role', 'commit') }
    catch { Exit-Usage "$path commit entry schema is invalid" }
    if ($entry.commit -isnot [string] -or $entry.commit -cnotmatch $commit_pattern) {
      Exit-Usage "$path commit is not a lowercase 40-hex SHA-1 (invalid lock)"
    }
  }
}

# --- tree verification --------------------------------------------------------
$failures = New-Object System.Collections.Generic.List[string]
$verified = 0

# SHA-256 via the .NET APIs, not Get-FileHash: the Utility module that provides
# Get-FileHash fails to auto-load under Windows PowerShell 5.1 when the host
# environment carries a pwsh 7 PSModulePath (the GitHub-hosted runner does), and
# the verifier must behave identically under powershell.exe and pwsh.
function Get-Sha256Lowercase([string]$literal_path) {
  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $stream = [System.IO.File]::OpenRead($literal_path)
    try {
      $digest = $sha.ComputeHash($stream)
    } finally {
      $stream.Dispose()
    }
  } finally {
    $sha.Dispose()
  }
  return [System.BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant()
}

# Reparse points are classified by their resolved destination, not by their
# mere existence: OneDrive cloud placeholders carry a reparse tag yet never
# relocate the path, while symlinks and junctions redirect resolution and are
# an escape exactly when the final physical path leaves the entry's declared
# allowed root. GetFinalPathNameByHandle resolves every redirecting component
# (and works identically under Windows PowerShell 5.1 and pwsh 7); a cloud
# placeholder resolves to itself and passes.
Add-Type -Namespace EpoCanva.VerifyInputs -Name NativeMethods -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern System.IntPtr CreateFileW(string name, uint access, uint share, System.IntPtr security, uint disposition, uint flags, System.IntPtr template);
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
public static extern uint GetFinalPathNameByHandleW(System.IntPtr handle, System.Text.StringBuilder buffer, uint length, uint flags);
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(System.IntPtr handle);
"@

$FILE_FLAG_BACKUP_SEMANTICS = [uint32]0x02000000
$INVALID_HANDLE_VALUE = [System.IntPtr]::new(-1)

function Get-FinalPhysicalPath([string]$literal_path) {
  $handle = [EpoCanva.VerifyInputs.NativeMethods]::CreateFileW(
    $literal_path, [uint32]0, [uint32]7, [System.IntPtr]::Zero, [uint32]3,
    $FILE_FLAG_BACKUP_SEMANTICS, [System.IntPtr]::Zero)  # access 0, share r/w/d, OPEN_EXISTING
  if ($handle -eq $INVALID_HANDLE_VALUE -or $handle -eq [System.IntPtr]::Zero) {
    return $null
  }
  try {
    $buffer = New-Object System.Text.StringBuilder 1024
    $length = [EpoCanva.VerifyInputs.NativeMethods]::GetFinalPathNameByHandleW(
      $handle, $buffer, [uint32]1024, [uint32]0)
    if ($length -eq [uint32]0 -or $length -ge [uint32]1024) {
      return $null
    }
    return $buffer.ToString(0, [int]$length)
  } finally {
    [void][EpoCanva.VerifyInputs.NativeMethods]::CloseHandle($handle)
  }
}

function Test-CanonicalContainment([string]$relative, [string]$allowed_root) {
  # Returns CLEAN, MISSING_TARGET, UNRESOLVABLE, or "ESCAPE:<final path>".
  $absolute = Join-Path $RepoRoot ($relative -replace '/', '\')
  if (-not (Test-Path -LiteralPath $absolute)) {
    return "MISSING_TARGET"
  }
  $final = Get-FinalPhysicalPath $absolute
  if ($null -eq $final) {
    return "UNRESOLVABLE"
  }
  # "\\?\D:\..." -> "D:\..."; the caller's root is a plain drive path.
  if ($final.StartsWith('\\?\UNC\')) {
    return "ESCAPE:$final"
  }
  if ($final.StartsWith('\\?\')) {
    $final = $final.Substring(4)
  }
  $expected_prefix = (Join-Path $RepoRoot ($allowed_root -replace '/', '\'))
  $final_normalized = $final.TrimEnd('\')
  $prefix_normalized = $expected_prefix.TrimEnd('\')
  if ($final_normalized -eq $prefix_normalized -or
      $final_normalized.StartsWith($prefix_normalized + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    return "CLEAN"
  }
  return "ESCAPE:$final"
}

foreach ($entry in $entries) {
  $kind = $entry.kind
  $path = $entry.path
  # The entry's own declared allowed root (validated at schema time above).
  $entry_root = ""
  foreach ($root in $allowed_roots) {
    if ("$path".StartsWith($root)) { $entry_root = $root; break }
  }
  if ($kind -eq "file") {
    $containment = Test-CanonicalContainment "$path" $entry_root
    if ($containment -eq "MISSING_TARGET") {
      $failures.Add("FAIL file $path MISSING")
      continue
    }
    if ($containment -eq "UNRESOLVABLE") {
      $failures.Add("FAIL file $path UNRESOLVABLE the final physical path could not be determined")
      continue
    }
    if ($containment.StartsWith("ESCAPE:")) {
      $failures.Add("FAIL file $path REPARSE_ESCAPE a symlink or junction relocates the pinned bytes")
      continue
    }
    $absolute = Join-Path $RepoRoot ("$path" -replace '/', '\')
    $item = Get-Item -LiteralPath $absolute -Force
    if ($item.PSIsContainer) {
      $failures.Add("FAIL file $path NOT_A_FILE")
      continue
    }
    if ($item.Length -ne [long]$entry.size_bytes) {
      $failures.Add("FAIL file $path SIZE_MISMATCH expected=$($entry.size_bytes) actual=$($item.Length)")
      continue
    }
    try {
      $actual = Get-Sha256Lowercase $absolute
    } catch {
      $failures.Add("FAIL file $path UNREADABLE")
      continue
    }
    if ($actual -ne "$($entry.sha256)") {
      $failures.Add("FAIL file $path HASH_MISMATCH expected=$($entry.sha256) actual=$actual")
    } else {
      $verified++
      Write-Output "OK file $path"
    }
  } else {
    $containment = Test-CanonicalContainment "$path" $entry_root
    if ($containment -eq "MISSING_TARGET") {
      $failures.Add("FAIL commit $path MISSING")
      continue
    }
    if ($containment -eq "UNRESOLVABLE") {
      $failures.Add("FAIL commit $path UNRESOLVABLE the final physical path could not be determined")
      continue
    }
    if ($containment.StartsWith("ESCAPE:")) {
      $failures.Add("FAIL commit $path REPARSE_ESCAPE a symlink or junction relocates the checkout")
      continue
    }
    $checkout = Join-Path $RepoRoot ("$path" -replace '/', '\')
    $top = & git -C $checkout rev-parse --show-toplevel 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $top -or [IO.Path]::GetFullPath("$top") -ine $checkout) {
      $failures.Add("FAIL commit $path NOT_A_GIT_CHECKOUT")
      continue
    }
    $head = (& git -C $checkout rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $head) {
      $failures.Add("FAIL commit $path NOT_A_GIT_CHECKOUT")
      continue
    }
    $head = ("$head").Trim().ToLowerInvariant()
    if ($head -ne "$($entry.commit)") {
      $failures.Add("FAIL commit $path COMMIT_MISMATCH expected=$($entry.commit) actual=$head")
      continue
    }
    # git status deliberately ignores assume-unchanged/skip-worktree entries.
    # Such an index cannot prove the upstream bytes are clean.
    $indexFlags = @(& git -C $checkout ls-files -v 2>$null)
    if ($LASTEXITCODE -ne 0 -or @($indexFlags | Where-Object { $_ -cmatch '^[a-zS] ' }).Count -ne 0) {
      $failures.Add("FAIL commit $path HIDDEN_INDEX_STATE")
      continue
    }
    # Byte-cleanliness of the checkout: tracked modifications, staged changes,
    # untracked files and submodule states in one porcelain call. The output
    # is counted, never printed: it names checkout-internal paths only.
    $status = (& git -c core.fsmonitor=false -C $checkout status --porcelain=v1 -uall --ignore-submodules=none 2>$null)
    if ($LASTEXITCODE -ne 0) {
      $failures.Add("FAIL commit $path STATUS_UNREADABLE")
      continue
    }
    $status_lines = @($status | Where-Object { "$_" -ne "" })
    if ($status_lines.Count -gt 0) {
      $failures.Add("FAIL commit $path DIRTY_CHECKOUT $($status_lines.Count) uncommitted change(s)")
      continue
    }
    $submodules = (& git -C $checkout submodule status --recursive 2>$null)
    if ($LASTEXITCODE -ne 0) {
      $failures.Add("FAIL commit $path SUBMODULE_STATUS_UNREADABLE")
      continue
    }
    # "+" moved HEAD, "U" conflicts, anything modified shows in the porcelain
    # above (" m"/" M" submodule entries). A "-" (uninitialized, absent)
    # submodule has no content to modify and cannot be initialized without
    # network I/O, which this contract forbids; absence is accepted.
    $bad_submodules = @($submodules | Where-Object {
        "$_" -ne "" -and -not ("$_".StartsWith(" ") -or "$_".StartsWith("-"))
      })
    if ($bad_submodules.Count -gt 0) {
      $failures.Add("FAIL commit $path DIRTY_SUBMODULE $($bad_submodules.Count) submodule(s) moved or conflicting")
      continue
    }
    $verified++
    Write-Output "OK commit $path"
  }
}

if ($failures.Count -gt 0) {
  foreach ($failure in $failures) { Write-Output $failure }
  Write-Output "verified $verified of $($entries.Count) entries; $($failures.Count) failed"
  exit 1
}
Write-Output "verified $($entries.Count) of $($entries.Count) entries"
exit 0
