#requires -Version 7.2
[CmdletBinding()]
param()

# Run the real entry points against a disposable workspace. Native tools and
# benchmark execution are replaced; target selection and PE validation stay real.
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$fixtureRoot = Join-Path $repoRoot ('build/arm64-performance-runners-' + [guid]::NewGuid().ToString('N'))
$fixtureScripts = Join-Path $fixtureRoot 'scripts'
$fixtureHelpers = Join-Path $fixtureRoot 'snow_shot/scripts'
$environment = [Environment]::GetEnvironmentVariables('Process')
$runnerNames = @('run-capture-rotation-perf', 'run-recording-laser-perf',
    'run-realtime-recording-perf', 'compare-realtime-recording-perf',
    'run-gpu-recording-perf', 'run-snow-shot-scrolling-image-performance')

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Write-PeFixture([string]$Path, [int]$Machine) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    $bytes = [byte[]]::new(128)
    $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[0x3c] = 64
    $bytes[64] = 0x50; $bytes[65] = 0x45
    [BitConverter]::GetBytes([uint16]$Machine).CopyTo($bytes, 68)
    [IO.File]::WriteAllBytes($Path, $bytes)
}
function Reset-Calls {
    $global:ArmRunnerCalls = [Collections.Generic.List[object]]::new()
    $global:ArmRunnerExecutableChecks = [Collections.Generic.List[object]]::new()
    $global:ArmRunnerStopAfter = 1
    $global:LASTEXITCODE = 0
}
function Invoke-Runner([string]$Name, [hashtable]$Arguments, [string]$ExpectedError = '') {
    $caught = ''
    try { & (Join-Path $fixtureScripts "$Name.ps1") @Arguments | Out-Null }
    catch { $caught = $_.Exception.Message }
    if ($ExpectedError) {
        Require ($caught -match $ExpectedError) "$Name expected '$ExpectedError', got '$caught'."
    } else {
        Require (-not $caught) "$Name failed: $caught"
    }
}
function Require-CargoTarget($Expected, [string]$Command, [string]$Package) {
    $cargo = @($global:ArmRunnerCalls | Where-Object Kind -eq 'cargo')
    Require ($cargo.Count -eq 1) 'A runner must issue exactly one stubbed Cargo command.'
    $arguments = $cargo[0].Arguments
    Require ($arguments[0] -ceq $Command -and $arguments -contains '--release' -and
        $arguments -contains $Package) 'Cargo must use the requested Release workload.'
    $targetIndex = [Array]::IndexOf($arguments, '--target')
    $directoryIndex = [Array]::IndexOf($arguments, '--target-dir')
    Require ($targetIndex -ge 0 -and $arguments[$targetIndex + 1] -ceq $Expected.RustTarget) `
        'Cargo must explicitly select the architecture-specific Rust target.'
    Require ($directoryIndex -ge 0 -and $arguments[$directoryIndex + 1] -ceq
        (Join-Path $fixtureRoot "build/$($Expected.Preset)/cargo")) `
        'Cargo output must belong to the matching performance preset.'
    $build = @($global:ArmRunnerCalls | Where-Object Kind -eq 'environment')
    Require ($build.Count -eq 1 -and $build[0].Preset -ceq $Expected.Preset) `
        'The build environment must use the matching Release performance preset.'
}
function Require-DependencyTarget($Expected) {
    $cargo = @($global:ArmRunnerCalls | Where-Object Kind -eq 'cargo')[0]
    Require ($cargo.Triplet -ceq $Expected.Triplet -and $cargo.Dynamic -ceq '1' -and
        $cargo.Ffmpeg -ceq $Expected.DependencyRoot -and
        $cargo.Path.StartsWith((Join-Path $Expected.DependencyRoot 'bin') + ';')) `
        'FFmpeg, vcpkg and runtime PATH must all use the selected dynamic triplet.'
}

try {
    New-Item -ItemType Directory -Path $fixtureScripts, $fixtureHelpers,
        (Join-Path $fixtureRoot 'snow-crates') -Force | Out-Null
    foreach ($name in $runnerNames) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "scripts/$name.ps1") `
            -Destination (Join-Path $fixtureScripts "$name.ps1")
        $errors = $null
        [Management.Automation.Language.Parser]::ParseFile(
            (Join-Path $fixtureScripts "$name.ps1"), [ref]$null, [ref]$errors) | Out-Null
        Require ($errors.Count -eq 0) "$name must parse."
    }
    $global:ArmRunnerActualHelper = Join-Path $repoRoot 'snow_shot/scripts/performance-environment.ps1'
    $global:ArmRunnerRoot = $fixtureRoot
    Set-Content -LiteralPath (Join-Path $fixtureHelpers 'performance-environment.ps1') -Value @'
. $global:ArmRunnerActualHelper
$script:SnowRepoRoot = $global:ArmRunnerRoot
function Get-SnowWindowsHostArchitecture { return $global:ArmRunnerHost }
function Set-SnowBuildEnvironment {
    param([string]$Preset)
    $target = Get-SnowWindowsTarget -Preset $Preset
    $global:ArmRunnerCalls.Add([pscustomobject]@{ Kind = 'environment'; Preset = $Preset })
    $env:QTDIR = Join-Path $global:ArmRunnerRoot "Qt/$($target.Architecture)"
    $env:Qt6_DIR = Join-Path $env:QTDIR 'lib/cmake/Qt6'
    return $target
}
$global:ArmRunnerRealAssert = ${function:Assert-SnowPerformanceExecutable}
function Assert-SnowPerformanceExecutable {
    param([string]$Path, [string]$Architecture)
    & $global:ArmRunnerRealAssert -Path $Path -Architecture $Architecture
    $global:ArmRunnerExecutableChecks.Add([pscustomobject]@{ Path = $Path; Architecture = $Architecture })
    if ($global:ArmRunnerExecutableChecks.Count -ge $global:ArmRunnerStopAfter) {
        throw 'Fixture stopped before benchmark execution.'
    }
}
$global:ArmRunnerRealQt = ${function:Set-SnowPerformanceQtRuntime}
function Set-SnowPerformanceQtRuntime {
    param([string]$Platform, [string]$Architecture)
    $snapshot = & $global:ArmRunnerRealQt -Platform $Platform -Architecture $Architecture
    $global:ArmRunnerCalls.Add([pscustomobject]@{
        Kind = 'qpa'; Platform = $Platform; Architecture = $Architecture
        Path = $env:PATH; Plugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
    })
    return $snapshot
}
'@
    function cargo {
        $global:ArmRunnerCalls.Add([pscustomobject]@{
            Kind = 'cargo'; Arguments = @($args); Triplet = $env:VCPKGRS_TRIPLET
            Dynamic = $env:VCPKGRS_DYNAMIC; Ffmpeg = $env:FFMPEG_DIR; Path = $env:Path
        })
        $global:LASTEXITCODE = 0
    }
    function cmake {
        $global:ArmRunnerCalls.Add([pscustomobject]@{ Kind = 'cmake'; Arguments = @($args) })
        $global:LASTEXITCODE = 0
    }
    function git { $global:LASTEXITCODE = 0; return ('0' * 40) }
    function python { throw 'A fixture must not launch result analysis.' }
    function Start-Sleep { throw 'A fixture must not reach benchmark countdown.' }

    foreach ($architecture in @('x64', 'arm64')) {
        $preset = if ($architecture -ceq 'arm64') { 'snow-shot-msvc-arm64-performance' } else { 'windows-msvc-performance' }
        $rustTarget = if ($architecture -ceq 'arm64') { 'aarch64-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
        $machine = if ($architecture -ceq 'arm64') { 0xaa64 } else { 0x8664 }
        $dependency = if ($architecture -ceq 'arm64') { '.tools/vcpkg/installed/arm64/dynamic/arm64-windows' } else { '.tools/vcpkg/installed/dynamic/x64-windows' }
        $expected = [pscustomobject]@{
            Preset = $preset; RustTarget = $rustTarget; Triplet = "$architecture-windows"
            DependencyRoot = Join-Path $fixtureRoot $dependency
        }
        $global:ArmRunnerHost = $architecture
        $qt = Join-Path $fixtureRoot "Qt/$architecture"
        New-Item -ItemType Directory -Path (Join-Path $qt 'lib/cmake/Qt6'),
            (Join-Path $qt 'lib/cmake/Qt6Core') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $qt 'lib/cmake/Qt6Core/Qt6CoreTargets.cmake') `
            -Value 'QT_ENABLED_PUBLIC_FEATURES "shared;timezone"'
        Write-PeFixture (Join-Path $qt 'bin/Qt6Core.dll') $machine
        $recording = Join-Path $fixtureRoot "build/$preset/cargo/$rustTarget/release/examples/realtime_recording_benchmark.exe"
        $scrolling = Join-Path $fixtureRoot "build/$preset/snow_shot/test-bin/Release/snow-shot-scrolling-image-performance-benchmark.exe"
        Write-PeFixture $recording $machine
        Write-PeFixture $scrolling $machine

        foreach ($hdr in @($false, $true)) {
            Reset-Calls
            Invoke-Runner 'run-capture-rotation-perf' @{ Architecture = $architecture; Hdr = $hdr }
            Require-CargoTarget $expected 'test' 'snow-capture'
            $arguments = @($global:ArmRunnerCalls | Where-Object Kind -eq 'cargo')[0].Arguments
            $filter = if ($hdr) { 'hdr_rotation_performance_benchmark' } else { 'rotation::tests::rotation_performance_benchmark' }
            Require ($arguments -contains $filter -and $arguments -contains '--ignored') `
                'Rotation must retain its narrowly selected SDR or HDR benchmark.'
        }
        Reset-Calls
        Invoke-Runner 'run-recording-laser-perf' @{ Architecture = $architecture; PreviewDirectory = (Join-Path $fixtureRoot 'previews') }
        Require-CargoTarget $expected 'run' 'snow-recording-runtime'
        Require-DependencyTarget $expected
        Reset-Calls
        Invoke-Runner 'run-realtime-recording-perf' @{ Architecture = $architecture; BuildOnly = $true }
        Require-CargoTarget $expected 'build' 'snow-recording-runtime'
        Require-DependencyTarget $expected
        Require ($global:ArmRunnerExecutableChecks.Count -eq 0) 'BuildOnly must never execute a benchmark.'
        Reset-Calls
        Invoke-Runner 'run-realtime-recording-perf' @{ Architecture = $architecture } 'Fixture stopped before benchmark execution'
        Require-CargoTarget $expected 'build' 'snow-recording-runtime'
        Require ($global:ArmRunnerExecutableChecks[0].Path -ceq $recording -and
            $global:ArmRunnerExecutableChecks[0].Architecture -ceq $architecture) `
            'Recording must validate the selected target-specific Release executable.'

        Reset-Calls
        $global:ArmRunnerStopAfter = 2
        Invoke-Runner 'compare-realtime-recording-perf' @{
            Architecture = $architecture; Baseline = $recording; Candidate = $recording
            OutputDirectory = (Join-Path $fixtureRoot 'comparison')
        } 'Fixture stopped before benchmark execution'
        Require ($global:ArmRunnerExecutableChecks.Count -eq 2 -and
            @($global:ArmRunnerExecutableChecks | Where-Object Architecture -cne $architecture).Count -eq 0) `
            'Paired comparisons must validate both executable architectures before execution.'
        Require (@($global:ArmRunnerCalls | Where-Object Kind -eq 'environment')[0].Preset -ceq $preset) `
            'Paired comparisons must initialize the target performance preset.'

        Reset-Calls
        Invoke-Runner 'run-snow-shot-scrolling-image-performance' @{ Architecture = $architecture; DetailedTiming = $true } `
            'Fixture stopped before benchmark execution'
        $cmake = @($global:ArmRunnerCalls | Where-Object Kind -eq 'cmake')
        Require ($cmake.Count -eq 2 -and $cmake[0].Arguments -contains $preset -and
            $cmake[0].Arguments -contains '-DSNOW_SHOT_SCROLLING_PERF_DETAIL=ON' -and
            $cmake[1].Arguments -contains "build-$preset" -and
            $cmake[1].Arguments -contains 'snow-shot-scrolling-image-performance-benchmark') `
            'Scrolling must configure and build its selected Release performance target.'
        $qpa = @($global:ArmRunnerCalls | Where-Object Kind -eq 'qpa')
        Require ($qpa.Count -eq 1 -and $qpa[0].Architecture -ceq $architecture -and
            $qpa[0].Platform -ceq 'offscreen' -and
            $qpa[0].Path.Contains((Join-Path $expected.DependencyRoot 'bin')) -and
            $qpa[0].Plugins -ceq (Join-Path $qt 'plugins/platforms')) `
            'Scrolling QPA must use the target Qt kit and dependency triplet.'
        Require ($global:ArmRunnerExecutableChecks[0].Path -ceq $scrolling) `
            'Scrolling must validate the target-specific Release executable.'

        # Cross-host executions fail before a build, while BuildOnly can cross-compile.
        $global:ArmRunnerHost = if ($architecture -ceq 'arm64') { 'x64' } else { 'arm64' }
        foreach ($runner in $runnerNames) {
            Reset-Calls
            $arguments = @{ Architecture = $architecture }
            if ($runner -ceq 'compare-realtime-recording-perf') {
                $arguments.Baseline = $recording; $arguments.Candidate = $recording
                $arguments.OutputDirectory = Join-Path $fixtureRoot 'cross-comparison'
            }
            Invoke-Runner $runner $arguments 'Performance benchmarks require a native'
            Require ($global:ArmRunnerCalls.Count -eq 0) "$runner must reject cross-host execution before starting tools."
        }
        Reset-Calls
        Invoke-Runner 'run-realtime-recording-perf' @{ Architecture = $architecture; BuildOnly = $true }
        Require-CargoTarget $expected 'build' 'snow-recording-runtime'
        Require ($global:ArmRunnerExecutableChecks.Count -eq 0) 'A cross-build cannot execute its output.'

        # Real PE checks reject mismatched binaries even on a matching native host.
        $global:ArmRunnerHost = $architecture
        $wrongMachine = if ($architecture -ceq 'arm64') { 0x8664 } else { 0xaa64 }
        Write-PeFixture $recording $wrongMachine
        Reset-Calls
        Invoke-Runner 'run-realtime-recording-perf' @{ Architecture = $architecture; Executable = $recording } `
            'Performance executable does not match the native'
        Reset-Calls
        Invoke-Runner 'compare-realtime-recording-perf' @{
            Architecture = $architecture; Baseline = $recording; Candidate = $recording
            OutputDirectory = (Join-Path $fixtureRoot 'wrong-comparison')
        } 'Performance executable does not match the native'
        $wrongCandidate = Join-Path $fixtureRoot "candidate-$architecture.exe"
        Write-PeFixture $wrongCandidate $wrongMachine
        Write-PeFixture $recording $machine
        Reset-Calls
        $global:ArmRunnerStopAfter = 2
        Invoke-Runner 'compare-realtime-recording-perf' @{
            Architecture = $architecture; Baseline = $recording; Candidate = $wrongCandidate
            OutputDirectory = (Join-Path $fixtureRoot 'wrong-candidate-comparison')
        } 'Performance executable does not match the native'
        Require ($global:ArmRunnerExecutableChecks.Count -eq 1) `
            'A wrong-machine candidate must fail after validating the native baseline.'
        Write-PeFixture $scrolling $wrongMachine
        Reset-Calls
        Invoke-Runner 'run-snow-shot-scrolling-image-performance' @{ Architecture = $architecture } `
            'Performance executable does not match the native'
        Write-PeFixture $scrolling $machine

        # GPU matrix wrapper uses a stub child runner so every forwarded call can
        # be inspected without configuring a compiler or launching a workload.
        $child = Join-Path $fixtureScripts 'run-realtime-recording-perf.ps1'
        $originalChild = Get-Content -Raw -LiteralPath $child
        try {
            Set-Content -LiteralPath $child -Value @'
param([switch]$BuildOnly, [string]$Architecture, [string]$Executable, [string]$Backend,
    [int]$Fps, [string]$Clarity, [string]$RegionSize, [string]$Workload, [string]$Scenario,
    [switch]$Audio, [switch]$PreferHardware, [int]$DurationSeconds, [int]$WarmupSeconds,
    [string]$OutputDirectory)
$global:ArmRunnerCalls.Add([pscustomobject]@{ Kind = 'gpu-child'; Parameters = @{} + $PSBoundParameters })
$global:LASTEXITCODE = 0
'@
            foreach ($skipBuild in @($false, $true)) {
                Reset-Calls
                Invoke-Runner 'run-gpu-recording-perf' @{
                    Architecture = $architecture; SkipBuild = $skipBuild
                    OutputDirectory = (Join-Path $fixtureRoot 'gpu-reports')
                }
                $children = @($global:ArmRunnerCalls | Where-Object Kind -eq 'gpu-child')
                Require ($children.Count -eq $(if ($skipBuild) { 48 } else { 49 })) `
                    'The GPU wrapper must forward its entire workload matrix.'
                foreach ($childCall in $children) {
                    Require ($childCall.Parameters.Architecture -ceq $architecture) `
                        'The GPU wrapper must forward Architecture to BuildOnly and every run.'
                    if (-not $childCall.Parameters.BuildOnly) {
                        Require ($childCall.Parameters.Executable -ceq $recording) `
                            'Every GPU workload must use the matching target Release executable.'
                    }
                }
            }
        } finally { Set-Content -LiteralPath $child -Value $originalChild }
    }
}
finally {
    $current = [Environment]::GetEnvironmentVariables('Process')
    foreach ($key in $current.Keys) {
        if (-not $environment.Contains($key)) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
    }
    foreach ($key in $environment.Keys) { [Environment]::SetEnvironmentVariable($key, $environment[$key], 'Process') }
    if (Test-Path -LiteralPath $fixtureRoot) {
        $buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build')) + [IO.Path]::DirectorySeparatorChar
        $resolved = (Resolve-Path -LiteralPath $fixtureRoot).Path
        if (-not $resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to clean a fixture outside the build directory.'
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
Write-Output 'ARM64 performance runner contract tests passed (x64 and ARM64 fixtures; no benchmarks executed).'
