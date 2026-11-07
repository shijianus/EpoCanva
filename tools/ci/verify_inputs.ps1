# Verifies a local tree against tools/ci/inputs.lock.json, the Tier 2 external
# build input identity contract (docs/testing/clean-worktree-verification.md
# section 1). Every locked file must exist with the exact pinned byte count and
# SHA-256; the pinned upstream checkout must be at the pinned commit.
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

$raw = Get-Content -LiteralPath $Lock -Raw -Encoding UTF8
try {
  $lockDocument = $raw | ConvertFrom-Json
} catch {
  Exit-Usage "inputs lock file is not valid JSON: $($_.Exception.Message)"
}
$properties = @($lockDocument.PSObject.Properties.Name)
if (-not ($properties -contains 'entries')) {
  Exit-Usage "inputs lock file has no entries array"
}
# PowerShell 5.1 deserializes a one-element JSON array as a scalar; @() makes the
# count and iteration correct for every entry count including zero and one.
$entries = @($lockDocument.entries)
if ($entries.Count -lt 1) {
  Exit-Usage "inputs lock file has no entries"
}

$sha256_pattern = '^[0-9a-f]{64}$'
$commit_pattern = '^[0-9a-f]{40}$'
$failures = New-Object System.Collections.Generic.List[string]
$verified = 0

foreach ($entry in $entries) {
  $kind = $entry.kind
  $path = $entry.path
  if ($null -eq $kind -or $null -eq $path -or "$path" -eq "") {
    Exit-Usage "inputs lock entry is missing 'kind' or 'path'"
  }
  if ($kind -eq "file") {
    if ("$($entry.sha256)" -notmatch $sha256_pattern) {
      Exit-Usage "$path sha256 is not a lowercase 64-hex digest (invalid lock)"
    }
    $absolute = Join-Path $RepoRoot ("$path" -replace '/', '\')
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
      $failures.Add("FAIL file $path MISSING")
      continue
    }
    $item = Get-Item -LiteralPath $absolute
    if ($item.Length -ne [long]$entry.size_bytes) {
      $failures.Add("FAIL file $path SIZE_MISMATCH expected=$($entry.size_bytes) actual=$($item.Length)")
      continue
    }
    $actual = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne "$($entry.sha256)") {
      $failures.Add("FAIL file $path HASH_MISMATCH expected=$($entry.sha256) actual=$actual")
    } else {
      $verified++
      Write-Output "OK file $path"
    }
  } elseif ($kind -eq "commit") {
    if ("$($entry.commit)" -notmatch $commit_pattern) {
      Exit-Usage "$path commit is not a lowercase 40-hex SHA-1 (invalid lock)"
    }
    $checkout = Join-Path $RepoRoot ("$path" -replace '/', '\')
    if (-not (Test-Path -LiteralPath $checkout -PathType Container)) {
      $failures.Add("FAIL commit $path MISSING")
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
    } else {
      $verified++
      Write-Output "OK commit $path"
    }
  } else {
    Exit-Usage "$path unknown entry kind '$kind' (invalid lock)"
  }
}

if ($failures.Count -gt 0) {
  foreach ($failure in $failures) { Write-Output $failure }
  Write-Output "verified $verified of $($entries.Count) entries; $($failures.Count) failed"
  exit 1
}
Write-Output "verified $($entries.Count) of $($entries.Count) entries"
exit 0
