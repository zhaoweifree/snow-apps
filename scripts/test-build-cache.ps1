# Deterministic cache contracts; no dependency installation or application launch.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')

function Require([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
$originalRoot = $script:SnowRepoRoot
$fixture = Join-Path ([IO.Path]::GetTempPath()) "snow build cache $([guid]::NewGuid().ToString('N'))"
$saved = @{}
foreach ($name in @('PATH', 'SNOW_QT_STATIC_DIR', 'Qt6_DIR', 'VCToolsVersion', 'VSINSTALLDIR',
        'CFLAGS', 'RUSTFLAGS', 'SNOW_CACHE_INCLUDE')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $script:SnowRepoRoot = $fixture
    $bin = Join-Path $fixture 'bin'
    $null = New-Item -ItemType Directory -Path $bin -Force
    $validator = Join-Path $bin 'dumpbin.exe'
    [IO.File]::WriteAllText($validator, 'validator one')
    $env:PATH = "$bin;$env:PATH"
    $script:Machine = 0x8664
    $script:Inspections = 0
    function Get-SnowWindowsLibraryMachine {
        param([string]$Path, [string]$Dumpbin)
        $script:Inspections++
        return $script:Machine
    }
    $library = Join-Path $fixture 'core library.lib'
    [IO.File]::WriteAllText($library, 'library one')
    Require (Test-SnowWindowsLibraryArchitecture $library x64) 'Validate an uncached x64 archive.'
    Require (Test-SnowWindowsLibraryArchitecture $library x64) 'Reuse an identical archive validation.'
    Require (-not (Test-SnowWindowsLibraryArchitecture $library arm64)) 'Cached machine identity must still reject the wrong architecture.'
    Require ($script:Inspections -eq 1) 'Identical content must be inspected only once.'
    $time = (Get-Item -LiteralPath $library).LastWriteTimeUtc
    [IO.File]::WriteAllText($library, 'library two')
    (Get-Item -LiteralPath $library).LastWriteTimeUtc = $time
    $script:Machine = 0xAA64
    Require (-not (Test-SnowWindowsLibraryArchitecture $library x64)) 'Same size and timestamp must not hide changed library bytes.'
    Require (Test-SnowWindowsLibraryArchitecture $library arm64) 'Retain the actual machine of replacement content.'
    Require ($script:Inspections -eq 2) 'A content replacement requires exactly one new inspection.'
    [IO.File]::WriteAllText($validator, 'validator two')
    Require (Test-SnowWindowsLibraryArchitecture $library arm64) 'A changed archive reader requires revalidation.'
    Require ($script:Inspections -eq 3) 'Reader identity belongs to the validation key.'
    foreach ($stamp in Get-ChildItem -LiteralPath (Join-Path $fixture 'build/.cache/library-architecture')) {
        [IO.File]::WriteAllText($stamp.FullName, '{broken')
    }
    Require (Test-SnowWindowsLibraryArchitecture $library arm64) 'An invalid validation stamp must be rebuilt.'
    Require ($script:Inspections -eq 4) 'Corrupt stamps must never certify archives.'
    [IO.File]::WriteAllText($library, 'mixed machines')
    $script:Machine = 0
    Require (-not (Test-SnowWindowsLibraryArchitecture $library x64)) 'Reject mixed or unreadable archives.'
    Require (-not (Test-SnowWindowsLibraryArchitecture $library x64)) 'A failed inspection must not be cached as successful.'
    Require ($script:Inspections -eq 6) 'Failed validation must be retried on the next attempt.'

    function Get-SnowWindowsHostArchitecture { return 'x64' }
    $env:VCToolsVersion = '14.51.99999'
    $env:VSINSTALLDIR = Join-Path $fixture 'Visual Studio'
    $env:SNOW_QT_STATIC_DIR = Join-Path $fixture 'Qt kit/lib/cmake/Qt6'
    $env:Qt6_DIR = $env:SNOW_QT_STATIC_DIR
    $build = Join-Path $fixture 'build/windows-msvc-debug'
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $build 'CMakeFiles/4.2.3')
    $cachePath = Join-Path $build 'CMakeCache.txt'
    $target = Get-SnowWindowsTarget -Preset windows-msvc-debug
    $cacheText = @(
        "Qt6_DIR:PATH=$env:SNOW_QT_STATIC_DIR",
        "Z_VCPKG_POWERSHELL_PATH:INTERNAL=$((Get-Process -Id $PID).Path)",
        'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026',
        'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64',
        'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4',
        'CMAKE_CACHE_MINOR_VERSION:INTERNAL=2',
        'CMAKE_CACHE_PATCH_VERSION:INTERNAL=3',
        "CMAKE_GENERATOR_INSTANCE:INTERNAL=$env:VSINSTALLDIR",
        "CMAKE_GENERATOR_TOOLSET:INTERNAL=$($target.Toolset)",
        'VCPKG_TARGET_TRIPLET:STRING=x64-windows',
        "VCPKG_INSTALLED_DIR:PATH=$($target.InstalledRoot)",
        "CMAKE_HOME_DIRECTORY:INTERNAL=$fixture"
    ) -join "`n"
    [IO.File]::WriteAllText($cachePath, $cacheText)
    Require (Test-SnowCacheAlignment $cachePath windows-msvc-debug) 'Matching cache state must be reusable.'
    # Cache keys and protocol values are case sensitive; path values alone may vary.
    $pathVariant = [regex]::Replace($cacheText, '(?m)^(Qt6_DIR|CMAKE_GENERATOR_INSTANCE|VCPKG_INSTALLED_DIR|CMAKE_HOME_DIRECTORY):([^=]+)=(.*)$', {
        param($match)
        $match.Groups[1].Value + ':' + $match.Groups[2].Value + '=' + $match.Groups[3].Value.Replace('\', '/').ToUpperInvariant()
    })
    [IO.File]::WriteAllText($cachePath, $pathVariant)
    Require (Test-SnowCacheAlignment $cachePath windows-msvc-debug) 'Equivalent Windows paths must not reset CMake.'
    $familyCache = $cacheText.Replace('version=14.51.99999', 'version=14.51')
    [IO.File]::WriteAllText($cachePath, $familyCache)
    Require (-not (Test-SnowCacheAlignment $cachePath windows-msvc-debug)) 'Unproven compiler equivalence requires a deliberate migration.'
    $compilerFile = Join-Path $build 'CMakeFiles/4.2.3/CMakeCXXCompiler.cmake'
    [IO.File]::WriteAllText($compilerFile,
        ('set(CMAKE_CXX_COMPILER "' + $env:VSINSTALLDIR.Replace('\', '/') + '/VC/Tools/MSVC/14.51.99999/bin/Hostx64/x64/cl.exe")'))
    Require (Test-SnowCacheAlignment $cachePath windows-msvc-debug) 'Compiler metadata must allow an equivalent family selector.'
    $arguments = @(Get-SnowConfigureArguments windows-msvc-debug $build)
    Require ($arguments -contains 'host=x64,version=14.51' -and $arguments -notcontains '--fresh') `
        'Keep the cached toolset spelling when reconfiguring an equivalent compiler.'
    [IO.File]::WriteAllText($compilerFile,
        'set(CMAKE_CXX_COMPILER "C:/fixture/VC/Tools/MSVC/14.51.11111/bin/Hostx64/x64/cl.exe")')
    Require (-not (Test-SnowCacheAlignment $cachePath windows-msvc-debug)) 'A different compiler patch must still invalidate a family selector.'
    $oldCompilerDirectory = Join-Path $build 'CMakeFiles/4.1.0'
    $null = New-Item -ItemType Directory -Force -Path $oldCompilerDirectory
    [IO.File]::WriteAllText((Join-Path $oldCompilerDirectory 'CMakeCXXCompiler.cmake'),
        ('set(CMAKE_CXX_COMPILER "' + $env:VSINSTALLDIR.Replace('\', '/') + '/VC/Tools/MSVC/14.51.99999/bin/Hostx64/x64/cl.exe")'))
    Require (-not (Test-SnowCacheAlignment $cachePath windows-msvc-debug)) 'Obsolete CMake metadata must not prove compiler equivalence.'
    foreach ($change in @(@('PLATFORM:INTERNAL=x64', 'PLATFORM:INTERNAL=ARM64'),
            @('version=14.51.99999', 'version=14.100.99999'), @('x64-windows', 'x64-windows-static'),
            @($env:SNOW_QT_STATIC_DIR, "$fixture/other Qt"))) {
        [IO.File]::WriteAllText($cachePath, $cacheText.Replace($change[0], $change[1]))
        $compatibility = Get-SnowCacheCompatibility $cachePath windows-msvc-debug
        Require ($compatibility.Action -ceq 'Fresh' -and $compatibility.Differences.Count -gt 0) `
            'Real compiler, architecture, dependency, and Qt changes must explain their reset.'
    }
    $unavailable = $cacheText -replace '(?m)^Z_VCPKG_POWERSHELL_PATH:INTERNAL=.*$',
        "Z_VCPKG_POWERSHELL_PATH:INTERNAL=$fixture/removed/pwsh.exe"
    [IO.File]::WriteAllText($cachePath, $unavailable)
    Require ((Get-SnowCacheCompatibility $cachePath windows-msvc-debug).Action -ceq 'Repair') `
        'A missing PowerShell executable is discovery state, not a toolchain change.'
    $arguments = @(Get-SnowConfigureArguments windows-msvc-debug $build)
    Require ($arguments -notcontains '--fresh' -and $arguments -contains '-UZ_VCPKG_POWERSHELL_PATH' -and
        $arguments -contains '-UZ_VCPKG_PWSH_PATH' -and $arguments -contains '-UZ_VCPKG_BUILTIN_POWERSHELL_PATH') `
        'Repair all cached PowerShell discovery entries without resetting CMake.'

    [IO.File]::WriteAllText($cachePath, $cacheText)
    [IO.File]::WriteAllText((Join-Path $build 'CMakeFiles/generate.stamp'), 'graph')
    $fingerprint = Get-SnowConfigureFingerprint windows-msvc-debug $null
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'Unstamped build trees need initial configuration.'
    Save-SnowConfigureState $build $fingerprint
    Require (Test-SnowConfigureState $build $fingerprint) 'Successful configuration must be reusable.'
    [IO.File]::WriteAllText((Join-Path $fixture 'source.cpp'), 'changed source')
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -ceq $fingerprint) `
        'Source edits belong to the native build dependency graph.'
    [IO.File]::AppendAllText($cachePath, "`nMANUAL_OVERRIDE:BOOL=ON")
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'External cache changes must not reuse a stale success marker.'
    [IO.File]::WriteAllText($cachePath, $cacheText + "`nCMAKE_SUPPRESS_REGENERATION:BOOL=ON")
    Save-SnowConfigureState $build $fingerprint
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'Do not skip configuration when native regeneration is disabled.'
    [IO.File]::WriteAllText($cachePath, $cacheText)
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'presets')
    $env:SNOW_CACHE_INCLUDE = Join-Path $fixture 'presets'
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakePresets.json'),
        '{"version":7,"include":["$penv{SNOW_CACHE_INCLUDE}/shared.json"]}')
    $included = Join-Path $fixture 'presets/shared.json'
    [IO.File]::WriteAllText($included, '{"version":7}')
    $before = Get-SnowConfigureFingerprint windows-msvc-debug $null
    [IO.File]::WriteAllText($included, '{"version":7,"vendor":{"fixture":{"flag":true}}}')
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -cne $before) 'Included presets must be fingerprinted.'
    $before = Get-SnowConfigureFingerprint windows-msvc-debug $null
    $env:RUSTFLAGS = '-C debuginfo=1'
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -cne $before) 'Changed Rust flags require regeneration of Cargo commands.'
    $before = Get-SnowConfigureFingerprint windows-msvc-debug $null
    [IO.File]::WriteAllText((Join-Path $fixture 'vcpkg.json'), '{"dependencies":[]}')
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -cne $before) 'Dependency manifest changes require configuration.'
    $before = Get-SnowConfigureFingerprint windows-msvc-debug $null
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeUserPresets.json'), '{"version":7}')
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -cne $before) 'User presets belong to the configuration fingerprint.'
    $overlay = Join-Path $fixture 'cmake/vcpkg-overlay-ports/fixture'
    $null = New-Item -ItemType Directory -Force -Path $overlay
    $before = Get-SnowConfigureFingerprint windows-msvc-debug $null
    [IO.File]::WriteAllText((Join-Path $overlay 'portfile.cmake'), '# dependency recipe')
    Require ((Get-SnowConfigureFingerprint windows-msvc-debug $null) -cne $before) 'Overlay recipe changes require dependency resolution.'
    [IO.File]::WriteAllText((Join-Path $build 'snow-configure-state.json'), '{broken')
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'Corrupt success markers must require configuration.'
    Save-SnowConfigureState $build $fingerprint
    Remove-Item -LiteralPath (Join-Path $build 'CMakeFiles/generate.stamp')
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'A missing native build graph requires configuration.'
    $ninjaCache = $cacheText.Replace('Visual Studio 18 2026', 'Ninja') + "`nCMAKE_CXX_COMPILER:FILEPATH=C:/tools/clang-cl.exe"
    [IO.File]::WriteAllText($cachePath, $ninjaCache)
    Require ((Get-SnowCacheCompatibility $cachePath windows-clang-portability).Action -ceq 'Reuse') `
        'The portability preset must retain its Ninja and clang-cl identity.'
    $arguments = @(Get-SnowConfigureArguments windows-clang-portability $build)
    Require ($arguments -notcontains '-T') 'Ninja must not receive a Visual Studio toolset argument.'
    [IO.File]::WriteAllText((Join-Path $build 'build.ninja'), 'graph')
    Save-SnowConfigureState $build $fingerprint
    Require (Test-SnowConfigureState $build $fingerprint) 'Ninja build graphs must also be reusable.'
    Remove-Item -LiteralPath (Join-Path $build 'build.ninja')
    Require (-not (Test-SnowConfigureState $build $fingerprint)) 'A removed Ninja graph must be regenerated.'
    Write-Output "PASS: archive validation, cache compatibility, and configuration fingerprints (PowerShell $($PSVersionTable.PSVersion))."
}
finally {
    $script:SnowRepoRoot = $originalRoot
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    $absolute = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $absolute.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe fixture cleanup path.' }
    if (Test-Path -LiteralPath $absolute) { Remove-Item -LiteralPath $absolute -Recurse -Force }
}
