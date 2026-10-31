<#
.SYNOPSIS
  Build the tree and run ctest. This is the repo check.

.DESCRIPTION
  First checks that tools/new-board-spec.ps1 can still find every anchor
  in docs/specs/ecsp-spec.md, then that board specs do not invent hardware
  tokens inside common-command sections. Then configures build/ on first
  use, cmake --build, and ctest.
  A multi-config generator (Visual Studio) builds and tests Release.
  A single-config generator (MinGW Makefiles, Ninja) keeps the
  existing CMAKE_BUILD_TYPE. Warnings are errors via the CMake targets.
  Exit code: a doc check, the build, or ctest.
  Wire this script to commits with tools/install-hooks.ps1.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'new-board-spec.ps1') -CheckAnchors
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-board-common-drift.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$build = Join-Path $root 'build'
$cachePath = Join-Path $build 'CMakeCache.txt'

if (-not (Test-Path -LiteralPath $cachePath)) {
    & cmake -S $root -B $build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$cache = Get-Content -LiteralPath $cachePath -Raw -Encoding UTF8
$multi = $cache -match 'CMAKE_GENERATOR:INTERNAL=(Visual Studio|Ninja Multi-Config|Xcode)'

$buildArgs = @('--build', $build)
$testArgs = @('--test-dir', $build, '--output-on-failure')
if ($multi) {
    $buildArgs += @('--config', 'Release')
    $testArgs += @('-C', 'Release')
}

& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& ctest @testArgs
exit $LASTEXITCODE
