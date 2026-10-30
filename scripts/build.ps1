[CmdletBinding()]
param(
    [ValidateSet(
        "windows-msvc-debug",
        "windows-msvc-performance",
        "windows-clang-portability",
        "snow-shot-msvc-release",
        "snow-shot-msvc-fast",
        "snow-shot-msvc-arm64-debug",
        "snow-shot-msvc-arm64-performance",
        "snow-shot-msvc-arm64-release",
        "snow-shot-msvc-arm64-fast"
    )]
    [string]$Preset = "windows-msvc-debug",
    [string]$Target = "",
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$SkipBootstrap
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")
$buildEnvironment = Set-SnowBuildEnvironment -Preset $Preset
$windowsTarget = Get-SnowWindowsTarget -Preset $Preset
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDirectory = Join-Path $repoRoot "build/$Preset"

if ($Clean -and (Test-Path -LiteralPath $buildDirectory)) {
    $resolvedBuild = (Resolve-Path -LiteralPath $buildDirectory).Path
    $resolvedRoot = (Resolve-Path -LiteralPath (Join-Path $repoRoot "build")).Path
    if (-not $resolvedBuild.StartsWith($resolvedRoot.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a build directory outside $resolvedRoot"
    }
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

if (-not $SkipBootstrap) {
    $vcpkgVariant = if ($windowsTarget.IsStatic) {
        "Static"
    }
    else {
        "Dynamic"
    }
    & (Join-Path $PSScriptRoot "bootstrap.ps1") -SkipDependencyInstall -VcpkgVariants $vcpkgVariant `
        -Architecture $windowsTarget.Architecture -Preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "Build environment bootstrap failed."
    }
}

Push-Location $repoRoot
try {
    $compatibility = Get-SnowCacheCompatibility -CachePath (Join-Path $buildDirectory 'CMakeCache.txt') -Preset $Preset
    $fingerprint = Get-SnowConfigureFingerprint -Preset $Preset -BuildEnvironment $buildEnvironment
    if ($Reconfigure -or $compatibility.Action -cne 'Reuse' -or
        -not (Test-SnowConfigureState -BuildDirectory $buildDirectory -Fingerprint $fingerprint)) {
        $configureArguments = @(Get-SnowConfigureArguments -Preset $Preset -BuildDirectory $buildDirectory `
            -Compatibility $compatibility)
        $statePath = Join-Path $buildDirectory 'snow-configure-state.json'
        if (Test-Path -LiteralPath $statePath) { Remove-Item -LiteralPath $statePath -Force }
        $reason = if ($Reconfigure) { 'explicitly requested' }
            elseif ($compatibility.Action -cne 'Reuse') { "cache action: $($compatibility.Action)" }
            else { 'configuration inputs or saved state changed' }
        Write-Host "Configuring preset $Preset ($reason)."
        & cmake @configureArguments
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed for preset $Preset."
        }
        Save-SnowConfigureState -BuildDirectory $buildDirectory -Fingerprint $fingerprint
    } else {
        Write-Host "Configuration unchanged for preset $Preset; building incrementally."
    }

    $buildArguments = @("--build", "--preset", "build-$Preset", "--parallel")
    if (-not [string]::IsNullOrWhiteSpace($Target)) {
        $buildArguments += @("--target", $Target)
    }
    elseif ($windowsTarget.IsStatic -and
        -not (Select-String -LiteralPath (Join-Path $buildDirectory 'CMakeCache.txt') `
            -Pattern '^SNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL=(ON|TRUE|1)$' -Quiet)) {
        $buildArguments += @('--target', 'snow_shot')
    }
    & cmake @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed for preset $Preset."
    }
    # Native regeneration may add cache entries while building. Acknowledge its
    # successful result so the next unchanged build does not configure again.
    Save-SnowConfigureState -BuildDirectory $buildDirectory -Fingerprint $fingerprint
}
finally {
    Pop-Location
}
