<#
.SYNOPSIS
  Fail when a C example in README.md names a cfg field or an ecsp_* symbol
  that the public headers do not declare.

.DESCRIPTION
  Scans fenced ```c blocks only. cfg.<field> and ecsp_* must appear as a
  whole word in include/ecsp/*.h. Prose outside those blocks is a review
  judgement, not this check. Exit 0 when clean.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$readmePath = Join-Path $root 'README.md'
$headerDir = Join-Path $root 'include/ecsp'

if (-not (Test-Path -LiteralPath $readmePath)) {
    Write-Host "FAIL missing README: $readmePath"
    exit 1
}

function Get-CFences {
    param([string]$Text)
    $blocks = New-Object System.Collections.Generic.List[string]
    foreach ($m in [regex]::Matches($Text, '(?s)```c\r?\n(.*?)```')) {
        $blocks.Add($m.Groups[1].Value) | Out-Null
    }
    return $blocks
}

function Get-MissingNames {
    param([string]$Code, [string]$Headers)
    $missing = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    $patterns = @(
        @{ Kind = 'cfg'; Re = '\bcfg\.([A-Za-z_][A-Za-z0-9_]*)' },
        @{ Kind = 'ecsp'; Re = '\b(ecsp_[A-Za-z0-9_]+)\b' }
    )
    foreach ($pat in $patterns) {
        foreach ($m in [regex]::Matches($Code, $pat.Re)) {
            $name = $m.Groups[1].Value
            if ($seen.ContainsKey($name)) { continue }
            $seen[$name] = $true
            if (-not [regex]::IsMatch($Headers, '\b' + [regex]::Escape($name) + '\b')) {
                if ($pat.Kind -eq 'cfg') {
                    $missing.Add("cfg.$name") | Out-Null
                } else {
                    $missing.Add($name) | Out-Null
                }
            }
        }
    }
    return $missing
}

$headerText = ''
foreach ($header in (Get-ChildItem -LiteralPath $headerDir -Filter '*.h' -File)) {
    $headerText += (Get-Content -LiteralPath $header.FullName -Raw -Encoding UTF8)
}

$readme = (Get-Content -LiteralPath $readmePath -Raw -Encoding UTF8) -replace "`r`n", "`n"
$code = ((Get-CFences $readme) -join "`n")
$missing = @(Get-MissingNames $code $headerText)
if ($missing.Count -gt 0) {
    Write-Host ("FAIL README.md names that are not in include/ecsp: {0}" -f ($missing -join ', '))
    exit 1
}

$probe = @(Get-MissingNames "cfg.board_cmd_count = 0;`necsp_not_a_symbol();" $headerText)
if ($probe -notcontains 'cfg.board_cmd_count' -or $probe -notcontains 'ecsp_not_a_symbol') {
    Write-Host 'FAIL README API self-test: a removed field or symbol was accepted'
    exit 1
}

Write-Host 'README API names: OK (self-test: cfg.board_cmd_count and ecsp_not_a_symbol are rejected)'
exit 0
