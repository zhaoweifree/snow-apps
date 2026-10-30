#requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "run-image-split-perf.ps1"
$tokens = $null
$parseErrors = $null
$syntax = [Management.Automation.Language.Parser]::ParseFile($runner, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw "Benchmark runner syntax errors: $($parseErrors.Message -join '; ')" }
# Load only pure comparison functions; this check neither initializes Qt nor launches benchmarks.
$functions = @("Get-ImageSplitMedian", "Get-ImageSplitRunOrder", "Get-ImageSplitNumber", "Get-ImageSplitComparison")
foreach ($definition in $syntax.FindAll({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)) {
    if ($definition.Name -in $functions) { . ([scriptblock]::Create($definition.Extent.Text)) }
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Require-Rejected([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require $rejected $Message
}
function New-Report([bool]$Candidate) {
    $scale = if ($Candidate) { 0.1 } else { 1.0 }
    return [ordered]@{
        preview_peak_resident_delta_bytes = 1000.0 * $scale
        native_history_peak_resident_delta_bytes = 2000.0 * $scale
        profiled_regions_peak_resident_delta_bytes = 3000.0 * $scale
        opaque_regions_peak_resident_delta_bytes = 4000.0 * $scale
        history_4000_retained_storage_bytes = if ($Candidate) { 900.0 } else { 10000.0 }
        preview_prepare_ms = 100.0 * $scale
        native_history_load_ms = 200.0 * $scale
        history_4000_delete_p95_ms = 1.0 * $scale
        unchanged_hover_paints = if ($Candidate) { 0 } else { 128 }
        native_history_source_shared = $Candidate
        large_image_export_ms = if ($Candidate) { 1000.0 } else { 10.0 }
        many_regions_export_ms = if ($Candidate) { 500.0 } else { 5.0 }
    }
}

Require ((Get-ImageSplitMedian @(1000, 10, 12)) -eq 12) "Odd medians must reject a single timing outlier."
Require ((Get-ImageSplitMedian @(2, 8, 4, 6)) -eq 5) "Even medians must average the central samples."
$order = @(Get-ImageSplitRunOrder -Pairs 3)
Require (($order.role -join ',') -ceq 'baseline,candidate,candidate,baseline,baseline,candidate') `
    "Each pair must alternate the execution order."
Require (($order.pair -join ',') -ceq '1,1,2,2,3,3') "Pair identifiers must preserve the raw run order."
Require-Rejected { Get-ImageSplitRunOrder -Pairs 2 } "At least three benchmark pairs are required."

$baseline = @(New-Report $false; New-Report $false; New-Report $false)
$candidate = @(New-Report $true; New-Report $true; New-Report $true)
$candidate[1].preview_peak_resident_delta_bytes = 600.0
$comparison = Get-ImageSplitComparison $baseline $candidate
Require ($comparison.passed -and $comparison.metrics.preview_peak_resident_delta_bytes.candidate_median -eq 100) `
    "Cross-process medians must control timing and memory comparisons."
Require ($comparison.metrics.large_image_export_ms.candidate_to_baseline -eq 100 -and $comparison.passed) `
    "Export timing must remain reported without a fragile throughput gate."

$candidate[0].unchanged_hover_paints = 1
$comparison = Get-ImageSplitComparison $baseline $candidate
Require (-not $comparison.passed -and -not ($comparison.assertions | Where-Object metric -eq 'unchanged_hover_paints').passed) `
    "Every candidate run must avoid unchanged-hover repaints; a median must not hide a regression."
$candidate[0].unchanged_hover_paints = 0
$candidate[2].native_history_source_shared = $false
Require (-not (Get-ImageSplitComparison $baseline $candidate).passed) `
    "Every native-history run must preserve shared source pixels."
$candidate[2].native_history_source_shared = $true

foreach ($field in @('preview_peak_resident_delta_bytes', 'native_history_peak_resident_delta_bytes',
        'profiled_regions_peak_resident_delta_bytes', 'opaque_regions_peak_resident_delta_bytes',
        'history_4000_retained_storage_bytes', 'preview_prepare_ms', 'native_history_load_ms',
        'history_4000_delete_p95_ms')) {
    $regression = @(New-Report $true; New-Report $true; New-Report $true)
    foreach ($report in $regression) { $report[$field] = $baseline[0][$field] }
    Require (-not (Get-ImageSplitComparison $baseline $regression).passed) "The $field improvement gate must reject a regression."
}
Require-Rejected { Get-ImageSplitComparison @($baseline[0], $baseline[1]) $candidate } `
    "An incomplete comparison must fail before reporting success."
$malformed = @(New-Report $true; New-Report $true; New-Report $true)
$malformed[0].Remove('preview_prepare_ms')
Require-Rejected { Get-ImageSplitComparison $baseline $malformed } "Missing required metrics must fail."
$malformed = @(New-Report $true; New-Report $true; New-Report $true)
$malformed[1].native_history_load_ms = [double]::NaN
Require-Rejected { Get-ImageSplitComparison $baseline $malformed } "Nonfinite timing values must fail."
Write-Host "Image Splitting benchmark runner checks passed."
