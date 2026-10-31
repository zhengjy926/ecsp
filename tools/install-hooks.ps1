<#
.SYNOPSIS
  Point this clone's Git hooks at .githooks (runs tools/check.ps1 on commit).

.DESCRIPTION
  Sets core.hooksPath to .githooks for this repository only. Does not change
  global git config. Re-run after a fresh clone.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$hooks = Join-Path $root '.githooks'
if (-not (Test-Path -LiteralPath (Join-Path $hooks 'pre-commit'))) {
    throw "missing $hooks/pre-commit"
}

Push-Location $root
try {
    & git config core.hooksPath .githooks
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host 'git hooks: core.hooksPath=.githooks (pre-commit runs tools/check.ps1)'
} finally {
    Pop-Location
}
