#requires -Version 7.2
<#
.SYNOPSIS
Compare already-built baseline and candidate Image Splitting Release benchmarks.
.DESCRIPTION
Runs fresh processes in alternating order, saves every raw JSON report and stderr log, and compares
medians across at least three pairs. Build both binaries with the native performance preset first.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][Alias("Baseline")][string]$BaselineExecutable,
    [Alias("Candidate")][string]$CandidateExecutable = "",
    [ValidateRange(3, 100)][int]$Pairs = 3,
    [string]$OutputDirectory = "",
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 900,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"

function Get-ImageSplitMedian {
    param([Parameter(Mandatory)][double[]]$Values)
    if ($Values.Count -eq 0) { throw "A comparison median requires at least one sample." }
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if ($ordered.Count % 2) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}

function Get-ImageSplitRunOrder {
    param([ValidateRange(3, 100)][int]$Pairs)
    for ($pair = 1; $pair -le $Pairs; ++$pair) {
        $roles = if ($pair % 2) { @("baseline", "candidate") } else { @("candidate", "baseline") }
        foreach ($role in $roles) {
            [ordered]@{ pair = $pair; role = $role }
        }
    }
}

function Get-ImageSplitNumber {
    param([Parameter(Mandatory)][Collections.IDictionary]$Report,
        [Parameter(Mandatory)][string]$Name)
    if (-not $Report.Contains($Name)) { throw "Benchmark report is missing $Name." }
    $value = $Report[$Name]
    if (($value -isnot [double] -and $value -isnot [long] -and $value -isnot [int] -and
            $value -isnot [decimal]) -or -not [double]::IsFinite([double]$value)) {
        throw "Benchmark field $Name must be a finite number."
    }
    return [double]$value
}

function Get-ImageSplitComparison {
    param([Parameter(Mandatory)][object[]]$BaselineReports,
        [Parameter(Mandatory)][object[]]$CandidateReports)
    if ($BaselineReports.Count -lt 3 -or $BaselineReports.Count -ne $CandidateReports.Count) {
        throw "Image Splitting comparison requires at least three complete pairs."
    }
    $metrics = [ordered]@{}
    foreach ($name in @($BaselineReports[0].Keys | Sort-Object)) {
        $value = $BaselineReports[0][$name]
        if ($value -isnot [double] -and $value -isnot [long] -and $value -isnot [int] -and
            $value -isnot [decimal]) { continue }
        if (-not $CandidateReports[0].Contains($name)) { continue }
        $baseline = Get-ImageSplitMedian @(foreach ($report in $BaselineReports) {
            Get-ImageSplitNumber $report $name
        })
        $candidate = Get-ImageSplitMedian @(foreach ($report in $CandidateReports) {
            Get-ImageSplitNumber $report $name
        })
        $metrics[$name] = [ordered]@{
            baseline_median = $baseline
            candidate_median = $candidate
            candidate_to_baseline = if ($baseline -eq 0) { $null } else { $candidate / $baseline }
        }
    }
    $limits = @(
        @{ metric = "preview_peak_resident_delta_bytes"; ratio = 0.5 },
        @{ metric = "native_history_peak_resident_delta_bytes"; ratio = 0.5 },
        @{ metric = "profiled_regions_peak_resident_delta_bytes"; ratio = 0.5 },
        @{ metric = "opaque_regions_peak_resident_delta_bytes"; ratio = 0.5 },
        @{ metric = "history_4000_retained_storage_bytes"; ratio = 0.1 },
        @{ metric = "preview_prepare_ms"; ratio = 1.0 },
        @{ metric = "native_history_load_ms"; ratio = 1.0 },
        @{ metric = "history_4000_delete_p95_ms"; ratio = 1.0 }
    )
    $assertions = [Collections.Generic.List[object]]::new()
    foreach ($limit in $limits) {
        if (-not $metrics.Contains($limit.metric)) {
            throw "Both benchmark reports must include $($limit.metric)."
        }
        $metric = $metrics[$limit.metric]
        if ($metric.baseline_median -le 0 -or $metric.candidate_median -lt 0) {
            throw "Benchmark comparison requires a positive baseline and nonnegative candidate for $($limit.metric)."
        }
        $assertions.Add([ordered]@{
            metric = $limit.metric
            maximum_candidate_to_baseline_exclusive = $limit.ratio
            passed = $metric.candidate_median -lt $metric.baseline_median * $limit.ratio
        })
    }
    $noHoverPaints = $true
    $nativeSharing = $true
    foreach ($report in $CandidateReports) {
        $noHoverPaints = $noHoverPaints -and (Get-ImageSplitNumber $report "unchanged_hover_paints") -eq 0
        $nativeSharing = $nativeSharing -and $report.Contains("native_history_source_shared") -and
            $report.native_history_source_shared -ceq $true
    }
    $assertions.Add([ordered]@{ metric = "unchanged_hover_paints"; expected = 0; passed = $noHoverPaints })
    $assertions.Add([ordered]@{ metric = "native_history_source_shared"; expected = $true; passed = $nativeSharing })
    return [ordered]@{
        schema_version = 1
        pairs = $BaselineReports.Count
        metrics = $metrics
        assertions = $assertions.ToArray()
        passed = @($assertions | Where-Object { -not $_.passed }).Count -eq 0
    }
}

function Write-ImageSplitUtf8 {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
}

function Invoke-ImageSplitBenchmark {
    param([string]$Executable, [string]$JsonPath, [string]$StderrPath, [int]$TimeoutSeconds)
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = Split-Path -Parent $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $completed = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $completed) {
            $process.Kill($true)
            $process.WaitForExit()
        }
        Write-ImageSplitUtf8 $JsonPath ($stdout.GetAwaiter().GetResult())
        Write-ImageSplitUtf8 $StderrPath ($stderr.GetAwaiter().GetResult())
        if (-not $completed) { throw "Benchmark timed out; output is saved at $JsonPath and $StderrPath." }
        if ($process.ExitCode -ne 0) {
            throw "Benchmark exited with code $($process.ExitCode); inspect $StderrPath."
        }
        return Get-Content -LiteralPath $JsonPath -Raw | ConvertFrom-Json -AsHashtable
    }
    finally { $process.Dispose() }
}

$workspace = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
. (Join-Path $PSScriptRoot "performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$performanceBuildDirectory = Join-Path $workspace "build/$($target.Preset)"
$BaselineExecutable = (Resolve-Path -LiteralPath $BaselineExecutable).Path
if ([string]::IsNullOrWhiteSpace($CandidateExecutable)) {
    $candidatePaths = @(
        (Join-Path $performanceBuildDirectory "snow_shot/Release/snow-shot-image-split-performance-benchmark.exe"),
        (Join-Path $performanceBuildDirectory "snow_shot/test-bin/Release/snow-shot-image-split-performance-benchmark.exe")
    )
    $CandidateExecutable = $candidatePaths | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
    if (-not $CandidateExecutable) {
        throw "The Release Image Splitting benchmark was not found under $performanceBuildDirectory."
    }
}
$CandidateExecutable = (Resolve-Path -LiteralPath $CandidateExecutable).Path
if ($BaselineExecutable -ieq $CandidateExecutable) { throw "Baseline and candidate executables must differ." }
Assert-SnowPerformanceExecutable -Path $BaselineExecutable -Architecture $Architecture
Assert-SnowPerformanceExecutable -Path $CandidateExecutable -Architecture $Architecture
Initialize-SnowPerformanceEnvironment -Architecture $Architecture
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $performanceBuildDirectory `
        ("image-split-comparison-" + [DateTime]::UtcNow.ToString("yyyyMMddTHHmmssfffZ"))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$null = [IO.Directory]::CreateDirectory($OutputDirectory)
$baselineReports = [Collections.Generic.List[object]]::new()
$candidateReports = [Collections.Generic.List[object]]::new()
$runOrder = [Collections.Generic.List[object]]::new()
$qtRuntime = Set-SnowPerformanceQtRuntime -Platform offscreen -Architecture $Architecture
try {
    foreach ($run in Get-ImageSplitRunOrder -Pairs $Pairs) {
        $role = $run.role
        $executable = if ($role -ceq "baseline") { $BaselineExecutable } else { $CandidateExecutable }
        $stem = "pair-{0:D2}-{1}" -f $run.pair, $role
        $jsonPath = Join-Path $OutputDirectory "$stem.json"
        $stderrPath = Join-Path $OutputDirectory "$stem.stderr.log"
        Write-Host "Image Splitting pair $($run.pair)/${Pairs}: $role"
        $report = Invoke-ImageSplitBenchmark $executable $jsonPath $stderrPath $TimeoutSeconds
        $implementation = if ($role -ceq "baseline") { "captured_baseline" } else { "optimized" }
        if ($report.build_type -cne "Release" -or $report.implementation -cne $implementation) {
            throw "$role executable must report Release/$implementation; inspect $jsonPath."
        }
        if ($role -ceq "baseline") { $baselineReports.Add($report) } else { $candidateReports.Add($report) }
        $runOrder.Add([ordered]@{ pair = $run.pair; role = $role; json = $jsonPath; stderr = $stderrPath })
    }
}
finally { Restore-SnowPerformanceQtRuntime -Snapshot $qtRuntime }

$comparison = Get-ImageSplitComparison $baselineReports.ToArray() $candidateReports.ToArray()
$comparison.baseline_executable = $BaselineExecutable
$comparison.candidate_executable = $CandidateExecutable
$comparison.baseline_sha256 = (Get-FileHash -LiteralPath $BaselineExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
$comparison.candidate_sha256 = (Get-FileHash -LiteralPath $CandidateExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
$comparison.run_order = $runOrder.ToArray()
$summaryPath = Join-Path $OutputDirectory "comparison.json"
Write-ImageSplitUtf8 $summaryPath (($comparison | ConvertTo-Json -Depth 12) + "`n")
$display = @(
    "preview_prepare_ms", "preview_peak_resident_delta_bytes", "native_history_load_ms",
    "preview_coprime_prepare_ms", "preview_coprime_peak_resident_delta_bytes",
    "native_history_peak_resident_delta_bytes", "history_4000_retained_storage_bytes",
    "history_4000_delete_p95_ms", "unchanged_hover_paints", "large_image_export_ms", "many_regions_export_ms",
    "profiled_regions_export_ms", "profiled_regions_peak_resident_delta_bytes",
    "opaque_regions_export_ms", "opaque_regions_peak_resident_delta_bytes"
)
foreach ($name in $display) {
    if (-not $comparison.metrics.Contains($name)) { continue }
    $metric = $comparison.metrics[$name]
    $ratio = if ($null -eq $metric.candidate_to_baseline) { "n/a" } else { "{0:P1}" -f $metric.candidate_to_baseline }
    Write-Host ("{0}: baseline {1:N3}, candidate {2:N3}, ratio {3}" -f
        $name, $metric.baseline_median, $metric.candidate_median, $ratio)
}
Write-Host "Raw runs and median comparison: $summaryPath"
$failed = @($comparison.assertions | Where-Object { -not $_.passed })
if ($failed.Count) { throw "Image Splitting performance assertions failed: $($failed.metric -join ', ')." }
