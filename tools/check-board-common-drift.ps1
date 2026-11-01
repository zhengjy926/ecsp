<#
.SYNOPSIS
  Fail when a board spec invents hardware detail inside a common-command section.

.DESCRIPTION
  For each docs/specs/boards/*-spec.md, walk 5.2.1 .. 5.2.18. Any
  hardware token (Sleep, Stop, USARTn, PAx, …) that appears in the board
  section but not in the matching section of docs/specs/ecsp-spec.md is drift:
  link semantics stay in the common spec; board files only override what the
  board owns. Exit 0 when clean.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$commonPath = Join-Path $root 'docs/specs/ecsp-spec.md'
$boardsDir = Join-Path $root 'docs/specs/boards'

if (-not (Test-Path -LiteralPath $commonPath)) {
    Write-Host "FAIL missing common spec: $commonPath"
    exit 1
}

$commonText = (Get-Content -LiteralPath $commonPath -Raw -Encoding UTF8) -replace "`r`n", "`n"

# Tokens that belong in board/product instantiation, not as silent additions
# to a forked copy of a common command clause.
$tokenPatterns = @(
    '\bSleep\b',
    '\bStop\b',
    'USART\d+',
    '\bPA\d+\b',
    '\bPB\d+\b',
    '\bPC\d+\b',
    '\bAF\d+\b',
    '扇区\s*\d+',
    '\bHSE\b',
    '\bPLL\b',
    '\bSYSCLK\b'
)

function Find-Heading {
    param([string]$Doc, [string]$Title)
    $m = [regex]::Match($Doc, '(?m)^#{1,6} ' + [regex]::Escape($Title))
    if (-not $m.Success) { return $null }
    return $m.Value
}

function Get-SectionBody {
    param([string]$Doc, [string]$Heading)
    $i = $Doc.IndexOf($Heading)
    if ($i -lt 0) { return $null }
    $start = $i + $Heading.Length
    $rest = $Doc.Substring($start)
    $m = [regex]::Match($rest, '(?m)^#{1,6} ')
    if ($m.Success) {
        return $rest.Substring(0, $m.Index)
    }
    return $rest
}

function Get-TokenHits {
    param([string]$Body)
    $hits = New-Object System.Collections.Generic.List[string]
    if ([string]::IsNullOrEmpty($Body)) { return $hits }
    foreach ($pat in $tokenPatterns) {
        foreach ($m in [regex]::Matches($Body, $pat, 'IgnoreCase')) {
            $hits.Add($m.Value) | Out-Null
        }
    }
    return $hits
}

$failed = 0
$boardFiles = @()
if (Test-Path -LiteralPath $boardsDir) {
    $boardFiles = @(Get-ChildItem -LiteralPath $boardsDir -Filter '*-spec.md' -File)
}

foreach ($board in $boardFiles) {
    $boardText = (Get-Content -LiteralPath $board.FullName -Raw -Encoding UTF8) -replace "`r`n", "`n"
    for ($n = 1; $n -le 18; $n++) {
        $title = "5.2.$n "
        $heading = Find-Heading $boardText $title
        if ($null -eq $heading) { continue }
        $commonHeading = Find-Heading $commonText $title
        $boardBody = Get-SectionBody $boardText $heading
        $commonBody = $null
        if ($null -ne $commonHeading) { $commonBody = Get-SectionBody $commonText $commonHeading }
        if ($null -eq $commonBody) {
            Write-Host ("FAIL {0}: has {1} but common spec does not" -f $board.Name, $title.Trim())
            $failed = 1
            continue
        }
        $boardHits = @(Get-TokenHits $boardBody)
        if ($boardHits.Count -eq 0) { continue }
        $commonHits = @(Get-TokenHits $commonBody)
        $commonSet = @{}
        foreach ($h in $commonHits) { $commonSet[$h.ToLowerInvariant()] = $true }
        $extra = @()
        foreach ($h in $boardHits) {
            if (-not $commonSet.ContainsKey($h.ToLowerInvariant())) {
                $extra += $h
            }
        }
        $extra = @($extra | Select-Object -Unique)
        if ($extra.Count -gt 0) {
            Write-Host ("FAIL {0} {1}: hardware tokens not in common section: {2}" -f `
                $board.Name, $title.Trim(), ($extra -join ', '))
            $failed = 1
        }
    }
}

if ($failed -ne 0) {
    Write-Host 'board-spec common-command drift: FAIL'
    exit 1
}

# Self-test: a Sleep token only on the board side of 5.2.11 must fail.
$boardProbe = Get-ChildItem -LiteralPath $boardsDir -Filter '*-spec.md' -File | Select-Object -First 1
if ($null -ne $boardProbe) {
    $orig = (Get-Content -LiteralPath $boardProbe.FullName -Raw -Encoding UTF8) -replace "`r`n", "`n"
    $heading = Find-Heading $orig '5.2.11 '
    $body = $null
    if ($null -ne $heading) { $body = Get-SectionBody $orig $heading }
    if ($null -ne $body -and $body.IndexOf('Sleep') -lt 0) {
        $injected = $orig.Insert($orig.IndexOf($heading) + $heading.Length, "`nSleep probe`n")
        $probeHits = @(Get-TokenHits (Get-SectionBody $injected $heading))
        $commonHeading = Find-Heading $commonText '5.2.11 '
        if ($null -eq $commonHeading) {
            Write-Host 'FAIL board-spec drift self-test: common spec has no 5.2.11'
            exit 1
        }
        $commonHits = @(Get-TokenHits (Get-SectionBody $commonText $commonHeading))
        $commonSet = @{}
        foreach ($h in $commonHits) { $commonSet[$h.ToLowerInvariant()] = $true }
        $wouldFail = $false
        foreach ($h in $probeHits) {
            if (-not $commonSet.ContainsKey($h.ToLowerInvariant())) { $wouldFail = $true }
        }
        if (-not $wouldFail) {
            Write-Host 'FAIL board-spec drift self-test: Sleep-only board token was accepted'
            exit 1
        }
    }
}

Write-Host 'board-spec common-command drift: OK (self-test: board-only Sleep is rejected)'
exit 0
