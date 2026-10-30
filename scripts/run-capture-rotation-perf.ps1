[CmdletBinding()]
param([switch]$Hdr, [ValidateSet("x64", "arm64")][string]$Architecture = "x64")

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../snow_shot/scripts/performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
Set-SnowBuildEnvironment -Preset $target.Preset | Out-Null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location (Join-Path $repoRoot "snow-crates")
try {
    $benchmark = if ($Hdr) { "hdr_rotation_performance_benchmark" } else { "rotation::tests::rotation_performance_benchmark" }
    & cargo test -p snow-capture --release --target $target.RustTarget `
        --target-dir (Join-Path $repoRoot "build/$($target.Preset)/cargo") `
        --lib $benchmark -- --ignored --nocapture
    if ($LASTEXITCODE -ne 0) {
        throw "Capture rotation benchmark failed."
    }
}
finally {
    Pop-Location
}
