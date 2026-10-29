<#
.SYNOPSIS
  Verify the ECSP golden vector table against the protocol spec.

.DESCRIPTION
  Two jobs (docs/specs/ecsp-impl-spec.md section 9):

  1. Recompute CRC16-MODBUS for every vector in test/vectors/vectors.h and
     compare it with the CRC bytes embedded in the frame. Vectors must carry
     real CRC bytes - a placeholder is rejected here.

  2. Scan docs/specs/ecsp-spec.md for example frames ([FA] ... [0D]) and
     assert every required section is covered by at least as many vectors.
     Deleting a vector therefore reports the corresponding spec section as
     missing.

  Exit code: 0 when everything passes, 1 otherwise. Wired into ctest.
#>

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$vectorsPath = Join-Path $root 'test/vectors/vectors.h'
$specPath    = Join-Path $root 'docs/specs/ecsp-spec.md'

# Required coverage for this ticket (issue #2). "Rule" sections describe a
# rule without byte examples and use an explicit minimum; the other sections
# must cover every example frame the spec gives them (count computed below).
# Vectors for sections beyond this list are still CRC-checked but not required
# here; later tickets extend this list as they add those sections' vectors.
$required = [ordered]@{
    '3.2'    = @{ Topic = 'CRC rules';              Rule = $true;  Min = 1 }
    '3.4.1'  = @{ Topic = 'basic request/response'; Rule = $true;  Min = 2 }
    '5.2.1'  = @{ Topic = 'handshake (3 frames)';   Rule = $false }
    '5.2.2'  = @{ Topic = 'version response';       Rule = $false }
    '5.2.5'  = @{ Topic = 'set serial number';      Rule = $false }
    '5.2.6'  = @{ Topic = 'get serial number';      Rule = $false }
    '5.2.10' = @{ Topic = 'self-test report';       Rule = $false }
    '5.2.14' = @{ Topic = 'upgrade report';         Rule = $false }
    '5.2.16' = @{ Topic = 'heartbeat';              Rule = $false }
    '5.2.17' = @{ Topic = 'fault report';           Rule = $false }
    '5.2.18' = @{ Topic = 'fault query';            Rule = $false }
}

function Get-Crc16Modbus {
    param([byte[]]$Bytes)
    $crc = [uint16]0xFFFF
    foreach ($b in $Bytes) {
        $crc = $crc -bxor $b
        for ($i = 0; $i -lt 8; $i++) {
            if (($crc -band 1) -ne 0) {
                $crc = (($crc -shr 1) -bxor 0xA001) -band 0xFFFF
            } else {
                $crc = ($crc -shr 1) -band 0xFFFF
            }
        }
    }
    return $crc
}

function ConvertFrom-HexString {
    param([string]$Text)
    $clean = $Text -replace '[^0-9A-Fa-f]', ''
    if (($clean.Length % 2) -ne 0) {
        throw "odd number of hex digits in vector: '$Text'"
    }
    $bytes = New-Object System.Collections.Generic.List[byte]
    for ($i = 0; $i -lt $clean.Length; $i += 2) {
        $bytes.Add([Convert]::ToByte($clean.Substring($i, 2), 16))
    }
    return $bytes.ToArray()
}

# ---- 1. parse vectors ----

$vectorsText = Get-Content -LiteralPath $vectorsPath -Raw -Encoding UTF8
$vectorRegex = [regex]'ECSP_VECTOR\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*"([0-9A-Fa-f][0-9A-Fa-f ]*)"\s*,\s*"spec\s+([0-9.]+)"\s*\)'

$vectors = @()
foreach ($m in $vectorRegex.Matches($vectorsText)) {
    $vectors += [pscustomobject]@{
        Name    = $m.Groups[1].Value
        Hex     = $m.Groups[2].Value.Trim()
        Section = $m.Groups[3].Value
        Bytes   = (ConvertFrom-HexString $m.Groups[2].Value)
    }
}

if ($vectors.Count -eq 0) {
    Write-Host 'ERROR: no vectors parsed from test/vectors/vectors.h'
    exit 1
}

# ---- 2. parse spec (sections + example-frame counts) ----

$specText = Get-Content -LiteralPath $specPath -Raw -Encoding UTF8
$sectionExists = @{}
$exampleCount = @{}
$currentSection = $null

foreach ($line in ($specText -split "`n")) {
    if ($line -match '^(#{1,6})\s+([0-9]+(?:\.[0-9]+)*)\s') {
        $currentSection = $matches[2]
        $sectionExists[$currentSection] = $true
    }
    if ($line -match '\[FA\]') {
        if ($null -ne $currentSection) {
            if ($exampleCount.ContainsKey($currentSection)) {
                $exampleCount[$currentSection] = $exampleCount[$currentSection] + 1
            } else {
                $exampleCount[$currentSection] = 1
            }
        }
    }
}

# ---- 3. CRC + structure check on every vector ----

$failures = 0

foreach ($v in $vectors) {
    $b = $v.Bytes
    $n = $b.Count
    if ($n -lt 9) {
        Write-Host "FAIL [$($v.Name)] frame too short ($n bytes)"
        $failures++
        continue
    }
    if ($b[0] -ne 0xFA) {
        Write-Host "FAIL [$($v.Name)] bad frame header (expected FA)"
        $failures++
    }
    if ($b[$n - 1] -ne 0x0D) {
        Write-Host "FAIL [$($v.Name)] bad frame tail (expected 0D)"
        $failures++
    }
    $frameLen = [int]$b[1] -bor ([int]$b[2] -shl 8)
    if ($frameLen -ne $n) {
        Write-Host "FAIL [$($v.Name)] length field $frameLen != actual $n bytes"
        $failures++
    }
    $cover = [byte[]]$b[1..($n - 4)]
    $calc = Get-Crc16Modbus $cover
    $embedded = [int]$b[$n - 3] -bor ([int]$b[$n - 2] -shl 8)
    if ($calc -ne $embedded) {
        Write-Host ("FAIL [{0}] CRC mismatch: embedded 0x{1:X4}, recomputed 0x{2:X4}" -f $v.Name, $embedded, $calc)
        $failures++
    }
}

# ---- 4. coverage check ----

$vectorCount = @{}
foreach ($v in $vectors) {
    if ($vectorCount.ContainsKey($v.Section)) {
        $vectorCount[$v.Section] = $vectorCount[$v.Section] + 1
    } else {
        $vectorCount[$v.Section] = 1
    }
}

foreach ($sec in $required.Keys) {
    $req = $required[$sec]
    $vecs = 0
    if ($vectorCount.ContainsKey($sec)) { $vecs = $vectorCount[$sec] }
    if ($req.Rule) {
        if ($vecs -lt $req.Min) {
            Write-Host "MISSING spec $sec ($($req.Topic)): want >= $($req.Min) vector(s), got $vecs"
            $failures++
        }
    } else {
        $exp = 0
        if ($exampleCount.ContainsKey($sec)) { $exp = $exampleCount[$sec] }
        if ($exp -eq 0) {
            Write-Host "ERROR spec $sec ($($req.Topic)): no example frame found in the spec to cover"
            $failures++
        } elseif ($vecs -lt $exp) {
            Write-Host "MISSING spec $sec ($($req.Topic)): spec has $exp example frame(s), vectors have $vecs"
            $failures++
        }
    }
}

foreach ($v in $vectors) {
    if (-not $sectionExists.ContainsKey($v.Section)) {
        Write-Host "DANGLING vector $($v.Name): spec $($v.Section) does not exist in the spec"
        $failures++
    }
}

# ---- summary ----

if ($failures -ne 0) {
    Write-Host ''
    Write-Host "check-vectors: $failures problem(s) found"
    exit 1
}

Write-Host "check-vectors: OK ($($vectors.Count) vectors, all CRCs valid, coverage complete)"
exit 0
