<#
.SYNOPSIS
  从 ECSP 通用规范生成一份自包含的板卡规范骨架。

.DESCRIPTION
  复制 docs/specs/ecsp-spec.md，替换标题、前言、1.1/1.2 口径、5.3 与附录 C 标题，删除附录 E，
  按本板地址改写示例帧中的 P/B，最后打印待填写清单。
  生成结果自包含：通用层正文完整保留，只有扩展点需要人填。

  注意：本文件必须以 UTF-8 with BOM 保存。Windows PowerShell 5.1 会按本地代码页解析无 BOM 脚本，中文会乱码并报语法错误。

.EXAMPLE
  pwsh tools/new-board-spec.ps1 -Name '脉冲信号发生板' -ProductAddr 0x03 -BoardAddr 0x02

.EXAMPLE
  powershell -File tools/new-board-spec.ps1 -Name '储能控制板' -ProductAddr 0x03 -BoardAddr 0x04
#>
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$ProductAddr = '0x03',
    [string]$BoardAddr = '0x02',
    [string]$IfType = 'UART（TTL 3.3V）',
    [string]$Baud = '115200 bps',
    [string]$Out
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'docs/specs/ecsp-spec.md'
if (-not (Test-Path $src)) { throw "找不到通用规范：$src" }

if (-not $Out) {
    $safe = $Name -replace '[^\w\u4e00-\u9fa5-]', ''
    $Out = Join-Path $root "docs/specs/boards/$safe-spec.md"
}
$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }

$text = (Get-Content $src -Raw -Encoding UTF8) -replace "`r`n", "`n"

function Replace-Section {
    param([string]$Doc, [string]$Start, [string]$End, [string]$Body)
    $i = $Doc.IndexOf($Start)
    if ($i -lt 0) { throw "未找到起始标题：$Start" }
    if (-not $End) { return $Doc.Substring(0, $i) + $Body }
    $j = $Doc.IndexOf($End, $i + $Start.Length)
    if ($j -lt 0) { throw "未找到结束标题：$End" }
    return $Doc.Substring(0, $i) + $Body + $Doc.Substring($j)
}

# 1) 标题
$oldTitle = '# 设备组自定义串行通信协议（ECSP）通用规范'
if ($text.IndexOf($oldTitle) -lt 0) { throw '标题行与预期不符，请检查通用规范首行' }
$text = $text.Replace($oldTitle, "# $Name ECSP 接口协议规范")

# 2) 前言
$preface = @"
## 前言

本规范是${Name}的 ECSP 接口协议规范，自包含地重述设备组自定义串行通信协议（ECSP）的通用层（帧格式、寻址模型、通信机制、通用命令、应答码与通用时序），并在通用规范预留的扩展点内定义本板的板卡功能命令、故障位扩展、时序覆盖值与符合性声明。

本规范遵循《设备组自定义串行通信协议（ECSP）通用规范》V1.0（2026-10-29 快照）。通用层条款不得在本规范内修改；确需修改的，先变更通用规范，再同步本规范。

本规范由通用规范复制并实例化而来，实例化处理依据见通用规范附录 E。

"@
$preface = $preface.TrimEnd() + "`n`n"
$text = Replace-Section -Doc $text -Start '## 前言' -End '# 1 引言' -Body $preface

# 2.1) 1.1 目的与 1.2 适用范围改为本板口径
$text = $text.Replace(
    '本文件定义设备组自定义串行通信协议（Equipment Custom Serial Protocol, ECSP）的通用部分，作为设备组内各产品软件设计、编码、单元测试与系统联调的共同依据，并为产品 SRS 中的接口需求提供实现层细节。预期读者包括：嵌入式软件工程师、上位机软件工程师、测试工程师、协议维护与评审人员。',
    "本文件定义${Name}与主机之间的 ECSP 接口协议（通用层 + 本板功能命令 + 本板业务约束），作为双方软件设计、编码、单元测试与系统联调的共同依据，并为产品 SRS 中的接口需求提供实现层细节。预期读者包括：嵌入式软件工程师、上位机软件工程师、测试工程师。")
$text = $text.Replace(
    '本规范适用于设备组内各产品中，节点之间以及节点与外部系统之间采用 ECSP 的串行通信链路。采用其他协议或接口形式的通信（如 USB、网络等）不在本规范范围内，由相应接口规范定义。',
    "本规范适用于${Name}与主机之间的 ECSP 串行通信链路。通用层条款按《设备组自定义串行通信协议（ECSP）通用规范》V1.0 执行，并在本规范内自包含重述；本板级内容（板卡功能命令、故障位扩展、时序覆盖值、符合性声明）由本规范定义。采用其他协议或接口形式的通信（如 USB、网络等）不在本规范范围内。")
$text = $text.Replace(
    '本规范只规定通用层。下列内容不在本规范范围内，由对应实例化文档定义：',
    '本规范自包含通用层，并补充本板内容；下列内容不在本规范范围内，由产品规范定义：')

# 3) 1.3 增补“本板”，1.4 增补通用规范引用
$ecspTerm = '* **ECSP**：Equipment Custom Serial Protocol，即设备组自定义串行通用协议。'
$text = $text.Replace($ecspTerm, $ecspTerm + "`n* **本板**：$Name。")
$refLine = '| 医疗器械软件注册审查指导原则 | 2022年修订版 |'
$text = $text.Replace($refLine, "| 设备组自定义串行通信协议（ECSP）通用规范 | V1.0（2026-10-29 快照） |`n$refLine")

# 4) 2.2 物理层取值：由“推荐示例”改为本板待填写字段
$oldPhys = @'
推荐取值示例如下（非强制，实际取值由板卡规范在本板规范中声明，见附录 C 表 10-3）：

* 接口类型：UART（TTL 3.3V）
* 波特率：115200 bps
* 数据位：8
* 停止位：1
* 校验位：无
* 流控制：无

实际取值以及其对第 7 章时序默认值的影响由板卡规范声明；同一链路两端必须取相同参数，该一致性由产品规范在整机拓扑中核对（见附录 C 表 10-2）。调整波特率时通信双方须同步更新。
'@
if ($text.IndexOf($oldPhys) -lt 0) { throw '2.2 节正文与预期不符，请检查通用规范是否已变更' }
$newPhys = @"
本板物理层参数（同一链路两端必须一致）：

* 接口类型：【$IfType】
* 波特率：【$Baud】
* 数据位：【8】
* 停止位：【1】
* 校验位：【无】
* 流控制：【无】
* 最大帧长：【 】字节（整帧，帧头至帧尾）

本板取值对第 7 章时序默认值的影响在此声明；与同链路对端的一致性由产品规范在整机拓扑中核对（见附录 C 表 10-2）。
"@
$text = $text.Replace($oldPhys, $newPhys)

# 5) 示例帧地址：仅改写 “产品地址 + 通用命令码 + 板卡地址” 的三连字节
function Hex2([string]$v) {
    $h = ($v -replace '^0[xX]', '').Trim()
    if ($h -notmatch '^[0-9A-Fa-f]{1,2}$') { return $null }
    return $h.PadLeft(2, '0').ToUpper()
}
$pHex = Hex2 $ProductAddr
$bHex = Hex2 $BoardAddr
$addrNote = ''
if ($pHex -and $bHex) {
    $pattern = '\[03\] \[([0-9A-F]{2})\] \[02\]'
    $text = [regex]::Replace($text, $pattern, "[$pHex] [`$1] [$bHex]")
    $text = $text.Replace('取 P=0x03、B=0x02', "取 P=0x$pHex、B=0x$bHex")
}
else {
    $addrNote = '示例帧地址未改写（ProductAddr/BoardAddr 不是 0x00~0xFF），请手工替换 [03] 与 [02]。'
}

# 6) 5.3 替换为板卡功能命令骨架
$board53 = @'
## 5.3 板卡功能命令详细定义

0x30~0xEF 为板卡功能命令区。本节定义本板的板卡功能命令；通用层的要求（命令码唯一、复用通用机制、不得修改通用层、不支持应答 0x04）见通用规范 5.3 节，本规范不再重复。

**（1）命令总表**

**表 5-5 板卡功能命令总表**

| 命令码 | 命令名称 | 方向 | 命令类别 | 幂等性 | 安全关键 |
| --- | --- | --- | --- | --- | --- |
| 0x30 | 【命令名称】 | Host→Slave | 【获取/设置/动作/事件】 | 【是/否】 | 【是/否】 |
| 0x31 | 【命令名称】 | Host→Slave | 【获取/设置/动作/事件】 | 【是/否】 | 【是/否】 |

**（2）公共约定**

* 公共应答码 0x01（未指定错误）、0x02（载荷长度错误）、0x10（未握手拒绝）适用于本节全部 Host→Slave 命令，各命令小节只列出 0x00 与该命令特有的拒绝应答码；
* 状态类拒绝按通用规范表 5-4 映射；设置类命令被拒绝时，涉及的全部配置项必须保持原值；
* 非幂等动作类命令的请求帧载荷首字节为事务序号，规则见通用规范 4.2 节；
* 长耗时命令必须声明最长执行时间（T_exec_max），并配套结果事件命令与“获取执行结果”命令，规则见通用规范 4.3 节；
* 板卡功能命令不得占用 0x01~0x2F，不得修改通用层定义的帧结构、应答码语义与通用时序。

**（3）本板运行状态**

| 状态 | 含义 | 进入条件 | 离开条件 |
| --- | --- | --- | --- |
| 【状态1】 | 【含义】 | 【条件】 | 【条件】 |
| 【状态2】 | 【含义】 | 【条件】 | 【条件】 |

本板状态必须能映射到通用规范的会话语义：未握手、就绪、忙（长耗时执行中）、故障（故障字非 0x00）。

**（4）命令—状态许可矩阵**

**表 5-6 命令—状态许可矩阵**

| 命令 | 【状态1】 | 【状态2】 | 【状态3】 |
| --- | --- | --- | --- |
| 【命令】 | 【受理/0x11/0x12/0x13】 | 【同上】 | 【同上】 |

**（5）逐条命令定义**

每条命令按下列模板展开，小节编号从 5.3.1 开始。

### 5.3.1 【命令名称】（0x30）

**（1）命令基本信息**

* 命令类别：【获取类 / 设置类 / 动作类（幂等/非幂等）/ 事件类（周期/单次）】
* 方向：【Host→Slave / Slave→Host】
* 功能说明：【一句话说明业务语义】
* 安全关键：【是/否】
* 请求帧数据载荷：【无 / 字段与字节数】
* 应答帧数据载荷：【仅应答码 / 应答码 + 字段与字节数】

**（2）请求帧数据载荷字段**

* 【字段名】（【字节数】字节，【类型】）：【单位、取值范围、编码规则】

**（3）应答帧数据载荷字段**

* 应答码
  * 0x00：【含义】
  * 【其它应答码】：【含义】
* 【其它字段】（【字节数】字节）：【含义】

**（4）示例**

* 主机请求帧示例：[FA] 【帧长】 [P] [30] [B] 【载荷】 [CRC16] [0D]
* 从机应答帧示例：[FA] 【帧长】 [P] [30] [B] [00] [CRC16] [0D]

'@
$text = Replace-Section -Doc $text -Start '## 5.3 板卡功能命令的实例化要求' -End '# 6 错误处理与异常' -Body $board53

# 7) 附录 C 改为自检表
$text = $text.Replace('## 10.3 附录 C：实例化清单', '## 10.3 附录 C：实例化自检表')

# 8) 删除附录 E（生成步骤）
$text = Replace-Section -Doc $text -Start '## 10.5 附录 E：生成板卡规范的步骤' -End $null -Body ''

# 9) 写出
$text = ($text -replace "`r`n", "`n").TrimEnd() + "`n"
[IO.File]::WriteAllText((Join-Path (Resolve-Path $outDir) (Split-Path -Leaf $Out)), $text, [Text.UTF8Encoding]::new($false))

# 10) 待办清单
$lines = $text -split "`n"
$todos = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '【') { $todos += ('{0}: {1}' -f ($i + 1), $lines[$i].Trim()) }
}
Write-Host "已生成：$Out"
Write-Host "总行数：$($lines.Count)；待填写字段：$($todos.Count) 处"
if ($addrNote) { Write-Host "注意：$addrNote" }
Write-Host ''
Write-Host '仍需人工处理的节（依据通用规范附录 E 表 10-5）：'
@(
    '标题/前言：确认板卡名与快照日期',
    '1.1 目的、1.2 适用范围：改写为本板链路',
    '2.2 物理层：确认六项取值与最大帧长',
    '5.3 板卡功能命令：填命令总表、运行状态、许可矩阵、逐条定义',
    '7.4 默认值：只填需覆盖的项',
    '第 8 章：增补本板安全关键命令',
    '9.2 表 9-1：填本板协议版本与固件版本对应',
    '10.1 修订记录、10.4 符合性矩阵：填本板',
    '删除：通用规范附录 E 引用（已完成）、1.6 中与板卡无关的表述（如适用）'
) | ForEach-Object { Write-Host "  - $_" }
Write-Host ''
Write-Host "剩余【】位置："
$todos | ForEach-Object { Write-Host "  $_" }
