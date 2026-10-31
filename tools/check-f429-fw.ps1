<#
.SYNOPSIS
  Cross-build board/f429/fw when an ARM GNU toolchain is on PATH.

.DESCRIPTION
  Parent #13 does not require a cross compiler in CI. When arm-none-eabi-gcc
  is available locally (or on a runner that installed one), this script runs
  the firmware Makefile so a Windows-only mkdir or link error fails the repo
  check. Missing toolchain prints one skip line and exits 0.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $root 'board\f429\fw'

$gcc = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host 'f429 firmware: skip (arm-none-eabi-gcc not on PATH)'
    exit 0
}

$make = Get-Command mingw32-make -ErrorAction SilentlyContinue
if (-not $make) {
    $make = Get-Command make -ErrorAction SilentlyContinue
}
if (-not $make) {
    Write-Error 'arm-none-eabi-gcc is on PATH but neither mingw32-make nor make was found'
    exit 1
}

Write-Host ("f429 firmware: {0} -C {1}" -f $make.Name, $fw)
& $make.Source -C $fw
exit $LASTEXITCODE
