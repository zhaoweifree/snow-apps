# Native launcher-build regression fixture: one C++ file, no Qt or vcpkg installation.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow incremental build $([guid]::NewGuid().ToString('N'))"
$saved = @{}
foreach ($name in @('PATH', 'INCLUDE', 'LIB', 'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir',
        'VCToolsVersion', 'WindowsSdkDir', 'WindowsSDKVersion', 'SNOW_MSVC_HOST_ARCHITECTURE',
        'SNOW_MSVC_TARGET_ARCHITECTURE', 'VSCMD_ARG_HOST_ARCH', 'VSCMD_ARG_TGT_ARCH',
        'Qt6_DIR', 'SNOW_QT_STATIC_DIR', 'VCPKG_ROOT', 'CFLAGS')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
function Require([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
try {
    $scripts = Join-Path $fixture 'scripts'
    $null = New-Item -ItemType Directory -Path $scripts -Force
    $policy = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'static-qt-features.json') | ConvertFrom-Json
    foreach ($file in @('build.ps1', 'snow-build-environment.ps1', 'snow-build-cache.ps1',
            'build-toolchain.json', 'qt-toolchain.json', 'static-qt-features.json') + @($policy.windowsSourcePatches)) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $scripts
    }
    # Keep real MSVC selection and real CMake/MSBuild. Only application dependency
    # provisioning is replaced: this project has no Qt, Rust, or vcpkg targets.
    @'
function Set-SnowBuildEnvironment {
    param([string]$Preset)
    Add-SnowMsvcToolsToPath -Architecture x64 | Out-Null
    $env:SNOW_QT_STATIC_DIR = $PSScriptRoot
    $env:Qt6_DIR = $PSScriptRoot
    return [pscustomobject]@{ Compiler = $env:VCToolsInstallDir }
}
'@ | Add-Content -LiteralPath (Join-Path $scripts 'snow-build-environment.ps1')
    @'
{
  "version": 6,
  "configurePresets": [{
    "name": "windows-msvc-debug",
    "generator": "Visual Studio 18 2026",
    "architecture": "x64",
    "binaryDir": "${sourceDir}/build/${presetName}",
    "cacheVariables": { "CMAKE_CONFIGURATION_TYPES": "Debug" }
  }],
  "buildPresets": [{
    "name": "build-windows-msvc-debug",
    "configurePreset": "windows-msvc-debug",
    "configuration": "Debug"
  }]
}
'@ | Set-Content -LiteralPath (Join-Path $fixture 'CMakePresets.json')
    $cmakePath = Join-Path $fixture 'CMakeLists.txt'
    @'
cmake_minimum_required(VERSION 4.2)
project(incremental_fixture LANGUAGES CXX)
set(Qt6_DIR "$ENV{SNOW_QT_STATIC_DIR}" CACHE PATH "")
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "")
set(VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/.tools/vcpkg/installed/dynamic" CACHE PATH "")
find_program(_powershell NAMES pwsh powershell REQUIRED)
set(Z_VCPKG_POWERSHELL_PATH "${_powershell}" CACHE INTERNAL "")
add_executable(probe source.cpp)
file(APPEND "${CMAKE_SOURCE_DIR}/configure-count.txt" "configured\n")
'@ | Set-Content -LiteralPath $cmakePath
    $sourcePath = Join-Path $fixture 'source.cpp'
    $source = "#ifndef FIXTURE_RETURN`n#define FIXTURE_RETURN 0`n#endif`nint main() { return FIXTURE_RETURN; }`n"
    [IO.File]::WriteAllText($sourcePath, $source)
    $build = Join-Path $fixture 'build/windows-msvc-debug'
    $executable = Join-Path $build 'Debug/probe.exe'
    $countPath = Join-Path $fixture 'configure-count.txt'
    $buildScript = Join-Path $scripts 'build.ps1'
    $log = Join-Path $fixture 'build.log'
    function Invoke-FixtureBuild {
        param([switch]$Reconfigure, [switch]$Clean)
        try { & $buildScript -Target probe -SkipBootstrap -Reconfigure:$Reconfigure -Clean:$Clean *> $log }
        catch { Get-Content -LiteralPath $log -Tail 70 | Write-Host; throw }
    }
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 1) 'Initial build must configure once.'
    $originalBinary = (Get-FileHash -LiteralPath $executable).Hash
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 1) 'An unchanged build must skip explicit configuration.'
    Require ((Get-FileHash -LiteralPath $executable).Hash -ceq $originalBinary) 'An unchanged build must retain the executable.'
    [IO.File]::WriteAllText($sourcePath, $source.Replace('return FIXTURE_RETURN;', 'return FIXTURE_RETURN + 1;'))
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 1) 'C++ edits must build incrementally.'
    & $executable
    Require ($LASTEXITCODE -eq 1) 'The native incremental build must compile changed C++ behavior.'
    [IO.File]::AppendAllText($cmakePath,
        "`ntarget_compile_definitions(probe PRIVATE FIXTURE_RETURN=2)`nset(FIXTURE_CACHED_OPTION ON CACHE BOOL `"`" )`n")
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 2) 'Native CMake regeneration must remain enabled for CMakeLists edits.'
    & $executable
    Require ($LASTEXITCODE -eq 3) 'Native regeneration must update compiler options.'
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 2) 'Native regeneration must converge to an unchanged build.'
    $env:CFLAGS = '/DFIXTURE_NATIVE_FLAGS'
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 3) 'Changed captured flags must explicitly configure.'
    Invoke-FixtureBuild -Reconfigure
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 4) 'Explicit reconfiguration must remain available.'
    Invoke-FixtureBuild -Clean
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 5) 'Clean builds must recreate the graph and success marker.'
    Invoke-FixtureBuild
    Require (@(Get-Content -LiteralPath $countPath).Count -eq 5) 'A clean build must also converge to incremental builds.'
    Write-Output 'PASS: native configure skipping, C++ edits, CMake regeneration, flag changes, reconfigure, and clean.'
}
finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    $absolute = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup path.' }
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}
