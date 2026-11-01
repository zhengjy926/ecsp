<#
.SYNOPSIS
  Verify the ECSP golden vector table against the protocol spec.

.DESCRIPTION
  Four jobs (docs/specs/ecsp-impl-spec.md section 9):

  1. Recompute CRC16-MODBUS for every vector in test/vectors/vectors.h and
     compare it with the CRC bytes embedded in the frame. Vectors must carry
     real CRC bytes - a placeholder is rejected here.

  2. Scan docs/specs/ecsp-spec.md for example frames ([FA] ... [0D]) and
     assert every required section is covered by at least as many vectors.
     Deleting a vector therefore reports the corresponding spec section as
     missing.

  3. A command section whose timing sentence only says "same as section X"
     fails when X declares a first-answer ceiling other than this command's
     category ceiling in table 7-1. A sentence that states its own ceiling
     is not that kind of pointer.

  4. Recompute CRC for concrete [FA] ... [0D] frames under
     docs/specs/boards/. A bracket that is not hex (CRC16, P, SYNC) skips
     that frame. Section coverage stays on the generic spec.

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
    '4.1.2'  = @{ Topic = 'slave power-up report';  Rule = $true;  Min = 1 }
    '5.2.1'  = @{ Topic = 'handshake (3 frames)';   Rule = $false }
    '5.2.2'  = @{ Topic = 'version response';       Rule = $false }
    '5.2.3'  = @{ Topic = 'set hardware version';   Rule = $false }
    '5.2.4'  = @{ Topic = 'get hardware version';   Rule = $false }
    '5.2.5'  = @{ Topic = 'set serial number';      Rule = $false }
    '5.2.6'  = @{ Topic = 'get serial number';      Rule = $false }
    '5.2.7'  = @{ Topic = 'system reset';           Rule = $false }
    '5.2.10' = @{ Topic = 'self-test report';       Rule = $false }
    '5.2.11' = @{ Topic = 'set low-power mode';     Rule = $false }
    '5.2.14' = @{ Topic = 'upgrade report';         Rule = $false }
    '5.2.16' = @{ Topic = 'heartbeat';              Rule = $false }
    '5.2.17' = @{ Topic = 'fault report';           Rule = $false }
    '5.2.18' = @{ Topic = 'fault query';            Rule = $false }
    '7.2.2'  = @{ Topic = 'slave heartbeat trigger'; Rule = $true; Min = 1 }
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

function Test-FrameBytes {
    param([byte[]]$Bytes, [string]$Label)
    $msgs = New-Object System.Collections.Generic.List[string]
    $n = $Bytes.Count
    if ($n -lt 9) {
        $msgs.Add("FAIL [$Label] frame too short ($n bytes)")
        return $msgs.ToArray()
    }
    if ($Bytes[0] -ne 0xFA) {
        $msgs.Add("FAIL [$Label] bad frame header (expected FA)")
    }
    if ($Bytes[$n - 1] -ne 0x0D) {
        $msgs.Add("FAIL [$Label] bad frame tail (expected 0D)")
    }
    $frameLen = [int]$Bytes[1] -bor ([int]$Bytes[2] -shl 8)
    if ($frameLen -ne $n) {
        $msgs.Add("FAIL [$Label] length field $frameLen != actual $n bytes")
    }
    $cover = [byte[]]$Bytes[1..($n - 4)]
    $calc = Get-Crc16Modbus $cover
    $embedded = [int]$Bytes[$n - 3] -bor ([int]$Bytes[$n - 2] -shl 8)
    if ($calc -ne $embedded) {
        $msgs.Add(("FAIL [{0}] CRC mismatch: embedded 0x{1:X4}, recomputed 0x{2:X4}" -f $Label, $embedded, $calc))
    }
    return $msgs.ToArray()
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
    foreach ($msg in @(Test-FrameBytes $v.Bytes $v.Name)) {
        if ($msg) {
            Write-Host $msg
            $failures++
        }
    }
}

# Concrete board-spec examples. Placeholders (CRC16, P, SYNC) are skipped.
# Section coverage stays on the generic spec above.
$boardFrames = 0
$boardDir = Join-Path $root 'docs/specs/boards'
if (Test-Path -LiteralPath $boardDir) {
    $frameRx = [regex]'\[FA\](?:\s*\[[^\]]+\])*\s*\[0D\]'
    foreach ($file in (Get-ChildItem -LiteralPath $boardDir -Filter '*.md' -File)) {
        $lineNo = 0
        foreach ($raw in [IO.File]::ReadAllLines($file.FullName)) {
            $lineNo++
            foreach ($m in $frameRx.Matches($raw)) {
                $groups = [regex]::Matches($m.Value, '\[([^\]]+)\]')
                $bytes = New-Object System.Collections.Generic.List[byte]
                $concrete = $true
                foreach ($g in $groups) {
                    $inner = $g.Groups[1].Value.Trim()
                    if ($inner -notmatch '^[0-9A-Fa-f]{2}(?:\s+[0-9A-Fa-f]{2})*$') {
                        $concrete = $false
                        break
                    }
                    foreach ($h in ($inner -split '\s+')) {
                        $bytes.Add([Convert]::ToByte($h, 16))
                    }
                }
                if (-not $concrete) { continue }
                $boardFrames++
                $label = '{0}:{1}' -f $file.Name, $lineNo
                foreach ($msg in @(Test-FrameBytes $bytes.ToArray() $label)) {
                    if ($msg) {
                        Write-Host $msg
                        $failures++
                    }
                }
            }
        }
    }
}

# A flipped CRC byte must be reported. This keeps the board-frame check
# from turning into a no-op if Test-FrameBytes stops comparing CRC.
$crcProbe = [byte[]](0xFA, 0x09, 0x00, 0x03, 0x30, 0x02, 0x9D, 0xC0, 0x0D)
$crcProbe[$crcProbe.Length - 3] = $crcProbe[$crcProbe.Length - 3] -bxor 0xFF
$crcProbeHit = $false
foreach ($hit in @(Test-FrameBytes $crcProbe 'crc-self-test')) {
    if ($hit -match 'CRC mismatch') { $crcProbeHit = $true }
}
if (-not $crcProbeHit) {
    Write-Host 'FAIL board-frame CRC self-test: a flipped CRC byte was accepted'
    $failures++
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

# ---- 5. timing sentences that only point at another section ----

function ConvertTo-Milliseconds {
    param([string]$Number, [string]$Unit)
    $value = [int]$Number
    if ($Unit -eq 's') {
        return $value * 1000
    }
    return $value
}

function Get-SpecSections {
    param([string]$Text)
    $sections = New-Object System.Collections.Generic.List[object]
    $current = $null
    foreach ($raw in ($Text -split "`n")) {
        $line = $raw.TrimEnd("`r")
        if ($line -match '^(#{1,6})\s+([0-9]+(?:\.[0-9]+)*)\s') {
            $sectionId = $Matches[2]
            $cmd = $null
            if ($line -match '0x([0-9A-Fa-f]{2})') {
                $cmd = $Matches[1].ToLower()
            }
            $current = [pscustomobject]@{
                Id       = $sectionId
                Cmd      = $cmd
                Category = $null
                Lines    = New-Object System.Collections.Generic.List[string]
            }
            $sections.Add($current)
            continue
        }
        if ($null -eq $current) { continue }
        if ($line -match '命令类别[：:]\s*(.+)$') {
            $current.Category = $Matches[1].Trim()
        }
        elseif ($line -match '^\|\s*((?:特殊会话|获取|设置|动作|事件)类[^\|]*)\s*\|') {
            # Dense §5.2 meta table: first cell is the command category.
            if (-not $current.Category) {
                $current.Category = $Matches[1].Trim()
            }
        }
        $current.Lines.Add($line)
    }
    # §5.2 不再重复类别；从总表 5-1 按命令码回填，供时序继承检查使用。
    $catByCmd = @{}
    foreach ($sec in $sections) {
        foreach ($line in $sec.Lines) {
            if ($line -match '^\|\s*0x([0-9A-Fa-f]{2})\s*\|[^|]*\|[^|]*\|\s*((?:特殊会话|获取|设置|动作|事件)类[^|]*)\s*\|') {
                $catByCmd[$Matches[1].ToLower()] = $Matches[2].Trim()
            }
        }
    }
    foreach ($sec in $sections) {
        if (-not $sec.Category -and $sec.Cmd -and $catByCmd.ContainsKey($sec.Cmd)) {
            $sec.Category = $catByCmd[$sec.Cmd]
        }
    }
    return $sections
}

function Get-TableCeilings {
    param($Sections)
    $byLabel = @{}
    $byCmd = @{}
    $inTable = $false
    foreach ($sec in $Sections) {
        if ($sec.Id -ne '7.1') { continue }
        foreach ($line in $sec.Lines) {
            if ($line -match '表 7-1') { $inTable = $true }
            if (-not $inTable) { continue }
            if ($line -match '^\|\s*(.+?)\s*\|\s*≤\s*(\d+)\s*(ms|s)\s*\|') {
                $label = $Matches[1].Trim()
                $ms = ConvertTo-Milliseconds $Matches[2] $Matches[3]
                $byLabel[$label] = $ms
                foreach ($code in [regex]::Matches($label, '0x([0-9A-Fa-f]{2})')) {
                    $byCmd[$code.Groups[1].Value.ToLower()] = $ms
                }
            }
        }
    }
    return [pscustomobject]@{ ByLabel = $byLabel; ByCmd = $byCmd }
}

function Get-CategoryCeiling {
    param($Ceilings, [string]$Cmd, [string]$Category)
    if ($Cmd -and $Ceilings.ByCmd.ContainsKey($Cmd)) {
        return [int]$Ceilings.ByCmd[$Cmd]
    }
    $label = $null
    if ($Category -match '获取') { $label = '获取类' }
    elseif ($Category -match '设置') { $label = '设置类' }
    elseif ($Category -match '动作') { $label = '动作类' }
    if ($label -and $Ceilings.ByLabel.ContainsKey($label)) {
        return [int]$Ceilings.ByLabel[$label]
    }
    return $null
}

function Get-DeclaredCeiling {
    param($Lines)
    $text = $Lines -join "`n"
    foreach ($sentence in ($text -split '。')) {
        if ($sentence -notmatch '时序') { continue }
        if ($sentence -match '上限为\s*(\d+)\s*(ms|s)') {
            return (ConvertTo-Milliseconds $Matches[1] $Matches[2])
        }
    }
    foreach ($sentence in ($text -split '。')) {
        if ($sentence -notmatch '时序') { continue }
        if ($sentence -match '≤\s*(\d+)\s*(ms|s)') {
            return (ConvertTo-Milliseconds $Matches[1] $Matches[2])
        }
    }
    return $null
}

function Get-EffectiveCeiling {
    param($Section, $Ceilings, $Declared)
    if ($null -ne $Declared) { return [int]$Declared }
    $fallback = Get-CategoryCeiling $Ceilings $Section.Cmd $Section.Category
    if ($null -eq $fallback) { return $null }
    return [int]$fallback
}

function Find-TimingInheritanceFailures {
    param([string]$SpecText)
    $found = New-Object System.Collections.Generic.List[string]
    $sections = @(Get-SpecSections $SpecText)
    $ceilings = Get-TableCeilings $sections
    $byId = @{}
    foreach ($sec in $sections) { $byId[$sec.Id] = $sec }

    foreach ($sec in $sections) {
        if (-not $sec.Category) { continue }
        $own = Get-CategoryCeiling $ceilings $sec.Cmd $sec.Category
        if ($null -eq $own) { continue }
        $text = $sec.Lines -join "`n"
        foreach ($sentence in ($text -split '。')) {
            if ($sentence -notmatch '时序') { continue }
            if ($sentence -match '\d+\s*(ms|s)') { continue }
            if ($sentence -notmatch '同\s*(\d+(?:\.\d+)+)\s*节') { continue }
            $targetId = $Matches[1]
            if (-not $byId.ContainsKey($targetId)) {
                $found.Add("spec $($sec.Id) timing points at missing section $targetId")
                continue
            }
            $target = $byId[$targetId]
            $targetMs = Get-EffectiveCeiling $target $ceilings (Get-DeclaredCeiling $target.Lines)
            if ($null -eq $targetMs) { continue }
            if ([int]$targetMs -ne [int]$own) {
                $found.Add("spec $($sec.Id) timing follows $targetId (${targetMs} ms) but this command's ceiling is $own ms")
            }
        }
    }
    return $found.ToArray()
}

$timingFailures = @(Find-TimingInheritanceFailures -SpecText $specText)
foreach ($msg in $timingFailures) {
    if ($msg) {
        Write-Host "FAIL $msg"
        $failures++
    }
}

$anchored = '错误处理同 5.2.3 节。时序适用表 7-1 设置类的 ≤100 ms，不沿用 5.2.3 的 3 s 非易失写入上限。'
$probe = $specText.Replace($anchored, '时序与错误处理同 5.2.3 节。')
if ($probe -eq $specText) {
    Write-Host 'FAIL timing check self-test: the low-power timing sentence is gone, so the detector is no longer anchored'
    $failures++
} else {
    $probeHits = @(Find-TimingInheritanceFailures -SpecText $probe)
    $saw = $false
    foreach ($hit in $probeHits) {
        if ($hit -match '5\.2\.11' -and $hit -match '5\.2\.3') { $saw = $true }
    }
    if (-not $saw) {
        Write-Host 'FAIL timing check self-test: a wholesale pointer at 5.2.3 was not reported'
        $failures++
    }
}

# ---- summary ----

if ($failures -ne 0) {
    Write-Host ''
    Write-Host "check-vectors: $failures problem(s) found"
    exit 1
}

Write-Host "check-vectors: OK ($($vectors.Count) vectors, $boardFrames board frames, all CRCs valid, coverage complete)"
exit 0
