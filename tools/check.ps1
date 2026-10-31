<#
.SYNOPSIS
  Build the tree and run ctest. This is the repo check.

.DESCRIPTION
  Configures build/ on first use, then cmake --build and ctest.
  A multi-config generator (Visual Studio) builds and tests Release.
  A single-config generator (MinGW Makefiles, Ninja) keeps the
  existing CMAKE_BUILD_TYPE. Warnings are errors via the CMake targets.
  Exit code: the build or ctest exit code.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
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
