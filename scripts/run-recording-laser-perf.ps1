[CmdletBinding()]
param([string]$PreviewDirectory = "", [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../snow_shot/scripts/performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
Set-SnowBuildEnvironment -Preset $target.Preset | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$env:FFMPEG_DIR = Join-Path $target.InstalledRoot $target.Triplet
$env:VCPKGRS_TRIPLET = $target.Triplet
$env:VCPKGRS_DYNAMIC = "1"
$env:Path = "$(Join-Path $env:FFMPEG_DIR 'bin');$env:Path"
$benchmarkArguments = @()
if ($PreviewDirectory) {
    $benchmarkArguments += $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($PreviewDirectory)
}
Push-Location (Join-Path $repoRoot "snow-crates")
try {
    & cargo run -p snow-recording-runtime --release --target $target.RustTarget `
        --target-dir (Join-Path $repoRoot "build/$($target.Preset)/cargo") `
        --example laser_trail_benchmark -- @benchmarkArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Recording laser benchmark failed."
    }
}
finally {
    Pop-Location
}
