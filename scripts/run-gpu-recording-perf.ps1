[CmdletBinding()]
param(
    [ValidateSet("dxgi", "wgc")][string]$Backend = "dxgi",
    [ValidateRange(1, 120)][int]$DurationSeconds = 10,
    [ValidateRange(0, 30)][int]$WarmupSeconds = 2,
    [string]$OutputDirectory = "",
    [switch]$SkipBuild,
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
. (Join-Path $PSScriptRoot "../snow_shot/scripts/performance-environment.ps1")
$target = Get-SnowPerformanceTarget -Architecture $Architecture -RequireNative
$runner = Join-Path $PSScriptRoot "run-realtime-recording-perf.ps1"
if (-not $SkipBuild) {
    & $runner -BuildOnly -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) { throw "The Release recording benchmark build failed." }
}
$executable = Join-Path $repoRoot "build/$($target.Preset)/cargo/$($target.RustTarget)/release/examples/realtime_recording_benchmark.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw "Build the recording benchmark first." }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot ("build/gpu-recording-perf/" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
foreach ($size in @("1920x1080", "3840x2160")) {
    foreach ($workload in @("static", "sparse", "continuous")) {
        foreach ($effects in @("no-effects", "all-effects")) {
            foreach ($audio in @($false, $true)) {
                foreach ($hardware in @($false, $true)) {
                    $name = "$size-$workload-$effects-audio$([int]$audio)-gpu$([int]$hardware)"
                    & $runner -Architecture $Architecture -Executable $executable -Backend $Backend -Fps 60 -Clarity native `
                        -RegionSize $size -Workload $workload -Scenario $effects -Audio:$audio `
                        -PreferHardware:$hardware -DurationSeconds $DurationSeconds `
                        -WarmupSeconds $WarmupSeconds -OutputDirectory (Join-Path $OutputDirectory $name)
                    if ($LASTEXITCODE -ne 0) { throw "Recording benchmark failed: $name" }
                }
            }
        }
    }
}
