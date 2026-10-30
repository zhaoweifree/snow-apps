[CmdletBinding()]
param(
    [string]$Image = "",
    [int]$ViewportHeight = 1600,
    [int]$StepPx = 25,
    [double]$ScrollFps = 30,
    [int]$MaxSteps = -1,
    [string]$OutputDirectory = "",
    [switch]$DetailedTiming,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")
Push-Location $repoRoot
try {
    . (Join-Path $PSScriptRoot "../snow_shot/scripts/performance-environment.ps1")
    $target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
    Set-SnowBuildEnvironment -Preset $target.Preset | Out-Null
    $detail = if ($DetailedTiming) { "ON" } else { "OFF" }
    & cmake --preset $target.Preset "-DSNOW_SHOT_SCROLLING_PERF_DETAIL=$detail"
    if ($LASTEXITCODE -ne 0) { throw "Scrolling benchmark configuration failed." }
    & cmake --build --preset "build-$($target.Preset)" --target `
        snow-shot-scrolling-image-performance-benchmark --parallel
    if ($LASTEXITCODE -ne 0) { throw "Scrolling benchmark build failed." }

    if ([string]::IsNullOrWhiteSpace($Image)) {
        $Image = Join-Path $repoRoot "snow_shot/test-imgs/scrollscreenshot-test.png"
    }
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $mode = if ($DetailedTiming) { "detailed" } else { "coarse" }
        $OutputDirectory = Join-Path $repoRoot "build/$($target.Preset)/scrolling-image-performance/$mode"
    }
    Set-SnowPerformanceQtRuntime -Platform offscreen -Architecture $Architecture | Out-Null
    $executable = Join-Path $repoRoot `
        "build/$($target.Preset)/snow_shot/test-bin/Release/snow-shot-scrolling-image-performance-benchmark.exe"
    Assert-SnowPerformanceExecutable -Path $executable -Architecture $Architecture
    $arguments = @(
        "--image", $Image, "--viewport-height", $ViewportHeight.ToString(),
        "--step-px", $StepPx.ToString(),
        "--scroll-fps", $ScrollFps.ToString([Globalization.CultureInfo]::InvariantCulture),
        "--max-steps", $MaxSteps.ToString(), "--output-dir", $OutputDirectory
    )
    & $executable @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Scrolling benchmark failed. Inspect $OutputDirectory/results.json."
    }
}
finally {
    Pop-Location
    $currentEnvironment = [Environment]::GetEnvironmentVariables("Process")
    foreach ($key in $currentEnvironment.Keys) {
        if (-not $previousEnvironment.Contains($key)) {
            [Environment]::SetEnvironmentVariable($key, $null, "Process")
        }
    }
    foreach ($key in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($key, $previousEnvironment[$key], "Process")
    }
}
