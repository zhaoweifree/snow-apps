# Shared by Windows build, launch, bootstrap, and packaging entry points.
Set-StrictMode -Version Latest

function Write-SnowBuildState {
    param([string]$Path, [object]$State)
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($State | ConvertTo-Json -Depth 12))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Get-SnowFileHash {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '<missing>' }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '<missing>' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-SnowTextHash {
    param([string]$Text)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '')
    }
    finally { $hasher.Dispose() }
}

function Get-SnowWindowsLibraryMachine {
    param([string]$Path, [string]$Dumpbin)
    # Keep the complete archive check: /GL members require the MSVC reader.
    $headers = (& $Dumpbin /nologo /headers $Path 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { return 0 }
    $machines = @([regex]::Matches($headers, '(?im)^\s*([0-9a-f]{4}) machine\s*\(') |
        ForEach-Object { [Convert]::ToInt32($_.Groups[1].Value, 16) } | Select-Object -Unique)
    if ($machines.Count -ne 1) { return 0 }
    return $machines[0]
}

function Test-SnowCachedWindowsLibraryArchitecture {
    param([string]$Path, [ValidateSet('x64', 'arm64')][string]$Architecture)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $dumpbin) { return $false }
    $hash = Get-SnowFileHash $Path
    $validator = Get-SnowFileHash $dumpbin.Source
    $key = Get-SnowTextHash "$hash|$validator"
    $stampPath = Join-Path $script:SnowRepoRoot "build/.cache/library-architecture/$key.json"
    $machine = 0
    if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        try {
            $stamp = Get-Content -Raw -LiteralPath $stampPath | ConvertFrom-Json
            if ($stamp.Schema -eq 1 -and $stamp.Hash -ceq $hash -and
                $stamp.Validator -ceq $validator -and $stamp.Machine -in @(0x8664, 0xAA64)) {
                $machine = $stamp.Machine
            }
        }
        catch { Write-Verbose "Ignoring invalid archive validation stamp: $stampPath" }
    }
    if (-not $machine) {
        $machine = Get-SnowWindowsLibraryMachine -Path $Path -Dumpbin $dumpbin.Source
        if ($machine -notin @(0x8664, 0xAA64)) { return $false }
        # Do not certify a library replaced while dumpbin was reading it.
        if ((Get-SnowFileHash $Path) -cne $hash) { return $false }
        Write-SnowBuildState -Path $stampPath -State @{
            Schema = 1; Hash = $hash; Validator = $validator; Machine = $machine
        }
    }
    return $machine -eq $(if ($Architecture -eq 'arm64') { 0xAA64 } else { 0x8664 })
}

function Get-SnowCMakeCache {
    param([string]$CachePath)
    $values = @{}
    if (Test-Path -LiteralPath $CachePath -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $CachePath) {
            if ($line -match '^([^#/:][^:]*):[^=]+=(.*)$') { $values[$Matches[1]] = $Matches[2] }
        }
    }
    return $values
}

function ConvertTo-SnowComparablePath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    try { return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/').Replace('\', '/').ToLowerInvariant() }
    catch { return $Path.Replace('\', '/').ToLowerInvariant() }
}

function Test-SnowMsvcToolsetAlignment {
    param([hashtable]$Cache, [string]$CachePath, [string]$Toolset)
    $cached = [string]$Cache['CMAKE_GENERATOR_TOOLSET']
    if ($cached -ceq $Toolset) { return $true }
    $version = [regex]::Match($Toolset, '(?:^|,)version=([0-9.]+)(?:,|$)')
    $oldVersion = [regex]::Match($cached, '(?:^|,)version=([0-9.]+)(?:,|$)')
    if (-not $version.Success -or -not $oldVersion.Success -or
        -not $version.Groups[1].Value.StartsWith($oldVersion.Groups[1].Value + '.') -or
        $cached.Replace($oldVersion.Groups[1].Value, $version.Groups[1].Value) -cne $Toolset) {
        return $false
    }
    # A family selector may be retained only with evidence of the compiler it
    # actually configured. Otherwise migrate once to the explicit full version.
    $cmakeVersion = @($Cache['CMAKE_CACHE_MAJOR_VERSION'], $Cache['CMAKE_CACHE_MINOR_VERSION'],
        $Cache['CMAKE_CACHE_PATCH_VERSION']) -join '.'
    if ($cmakeVersion -notmatch '^\d+\.\d+\.\d+$') { return $false }
    $compilerFile = Join-Path (Split-Path -Parent $CachePath) "CMakeFiles/$cmakeVersion/CMakeCXXCompiler.cmake"
    if (-not (Test-Path -LiteralPath $compilerFile -PathType Leaf)) { return $false }
    $text = Get-Content -Raw -LiteralPath $compilerFile
    $compiler = [regex]::Match($text, 'set\(CMAKE_CXX_COMPILER "([^"]+)"\)')
    if (-not $compiler.Success -or -not $env:VSINSTALLDIR) { return $false }
    $hostMatch = [regex]::Match($Toolset, '(?:^|,)host=([^,]+)')
    $hostArchitecture = if ($hostMatch.Success) { $hostMatch.Groups[1].Value }
        else { Get-SnowWindowsHostArchitecture }
    $architecture = ([string]$Cache['CMAKE_GENERATOR_PLATFORM']).ToLowerInvariant()
    $expected = Join-Path $env:VSINSTALLDIR (
        "VC/Tools/MSVC/$($version.Groups[1].Value)/bin/Host$hostArchitecture/$architecture/cl.exe")
    return (ConvertTo-SnowComparablePath $compiler.Groups[1].Value) -ceq (ConvertTo-SnowComparablePath $expected)
}

function Get-SnowCacheCompatibility {
    param([string]$CachePath, [string]$Preset)
    $cache = Get-SnowCMakeCache $CachePath
    $differences = [Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return [pscustomobject]@{ Action = 'New'; Differences = @(); Cache = $cache }
    }
    $target = Get-SnowWindowsTarget -Preset $Preset
    $check = {
        param($Name, $Expected, $Action = 'Fresh', [switch]$Path, [switch]$AllowMissing)
        $actual = [string]$cache[$Name]
        if ($AllowMissing -and -not $cache.ContainsKey($Name)) { return }
        $equal = if ($Path) {
            (ConvertTo-SnowComparablePath $actual) -ceq (ConvertTo-SnowComparablePath $Expected)
        } else { $actual -ceq $Expected }
        if (-not $equal) {
            $differences.Add([pscustomobject]@{
                Name = $Name; Actual = $actual; Expected = $Expected; Action = $Action
            })
        }
    }
    & $check 'Qt6_DIR' $env:SNOW_QT_STATIC_DIR -Path
    if ($target.IsStatic) { & $check 'SNOW_QT_STATIC_DIR' $env:SNOW_QT_STATIC_DIR -Path }
    if ($Preset -eq 'windows-clang-portability') {
        & $check 'CMAKE_GENERATOR' 'Ninja'
        if ($cache['CMAKE_CXX_COMPILER'] -notmatch '(?i)(?:^|[/\\])clang-cl(?:\.exe)?$') {
            & $check 'CMAKE_CXX_COMPILER' 'clang-cl'
        }
    } else {
        & $check 'CMAKE_GENERATOR' 'Visual Studio 18 2026'
        & $check 'CMAKE_GENERATOR_PLATFORM' $target.GeneratorPlatform
        if (-not (Test-SnowMsvcToolsetAlignment -Cache $cache -CachePath $CachePath -Toolset $target.Toolset)) {
            & $check 'CMAKE_GENERATOR_TOOLSET' $target.Toolset
        }
        if ($env:VSINSTALLDIR -and $cache.ContainsKey('CMAKE_GENERATOR_INSTANCE')) {
            & $check 'CMAKE_GENERATOR_INSTANCE' $env:VSINSTALLDIR -Path
        }
    }
    if ($target.Architecture -cne $target.HostArchitecture) {
        & $check 'QT_HOST_PATH' $env:QT_HOST_PATH -Path
    }
    & $check 'SNOW_RUST_TARGET' $target.RustTarget -AllowMissing:($target.Architecture -eq 'x64')
    & $check 'VCPKG_HOST_TRIPLET' $target.HostTriplet `
        -AllowMissing:($target.Architecture -eq 'x64' -and $target.HostArchitecture -eq 'x64')
    & $check 'VCPKG_TARGET_TRIPLET' $target.Triplet
    & $check 'VCPKG_INSTALLED_DIR' $target.InstalledRoot -Path
    & $check 'CMAKE_HOME_DIRECTORY' $script:SnowRepoRoot -Path
    $powerShell = [string]$cache['Z_VCPKG_POWERSHELL_PATH']
    $powerShellValid = if ([IO.Path]::IsPathRooted($powerShell)) {
        Test-Path -LiteralPath $powerShell -PathType Leaf
    } else { $powerShell -and $null -ne (Get-Command $powerShell -ErrorAction SilentlyContinue) }
    if (-not $powerShellValid) {
        & $check 'Z_VCPKG_POWERSHELL_PATH' 'an available PowerShell executable' 'Repair'
    }
    $action = if (@($differences | Where-Object Action -CEQ 'Fresh').Count) { 'Fresh' }
        elseif ($differences.Count) { 'Repair' } else { 'Reuse' }
    return [pscustomobject]@{ Action = $action; Differences = @($differences.ToArray()); Cache = $cache }
}

function Test-SnowCacheAlignment {
    param([string]$CachePath, [string]$Preset)
    return (Get-SnowCacheCompatibility -CachePath $CachePath -Preset $Preset).Action -ceq 'Reuse'
}

function Get-SnowConfigureArguments {
    param([string]$Preset, [string]$BuildDirectory, [object]$Compatibility = $null)
    if ($null -eq $Compatibility) {
        $Compatibility = Get-SnowCacheCompatibility -CachePath (Join-Path $BuildDirectory 'CMakeCache.txt') -Preset $Preset
    }
    $target = Get-SnowWindowsTarget -Preset $Preset
    $arguments = @('--preset', $Preset, '-S', $script:SnowRepoRoot, '-B', $BuildDirectory,
        "-DVCPKG_HOST_TRIPLET=$($target.HostTriplet)", "-DSNOW_RUST_TARGET=$($target.RustTarget)")
    $toolset = $target.Toolset
    if ($Compatibility.Action -in @('Reuse', 'Repair')) {
        $toolset = [string]$Compatibility.Cache['CMAKE_GENERATOR_TOOLSET']
    }
    if ($Preset -ne 'windows-clang-portability' -and $toolset) { $arguments += @('-T', $toolset) }
    if ($env:LIBCLANG_PATH) {
        $arguments += "-DSNOW_LIBCLANG_BIN_DIR:PATH=$env:LIBCLANG_PATH"
        $arguments += "-DSNOW_SHOT_LIBCLANG_DIR:PATH=$env:LIBCLANG_PATH"
    }
    if ($target.Architecture -cne $target.HostArchitecture) {
        $arguments += "-DQT_HOST_PATH:PATH=$env:QT_HOST_PATH"
    } else { $arguments += '-UQT_HOST_PATH' }
    foreach ($difference in $Compatibility.Differences) {
        Write-Host ("CMake cache {0}: cached='{1}', expected='{2}' ({3})." -f
            $difference.Name, $difference.Actual, $difference.Expected, $difference.Action)
    }
    switch ($Compatibility.Action) {
        'Fresh' { return @('--fresh') + $arguments }
        'Repair' {
            $arguments += @('-UZ_VCPKG_POWERSHELL_PATH', '-UZ_VCPKG_PWSH_PATH', '-UZ_VCPKG_BUILTIN_POWERSHELL_PATH')
        }
    }
    return $arguments
}

function Get-SnowConfigureFingerprint {
    param([string]$Preset, [object]$BuildEnvironment)
    $files = [Collections.Generic.List[string]]::new()
    foreach ($path in @('scripts/build.ps1', 'scripts/snow-build-environment.ps1',
            'scripts/snow-build-cache.ps1', 'scripts/build-toolchain.json', 'scripts/qt-toolchain.json',
            'scripts/static-qt-features.json', 'vcpkg.json', 'vcpkg-configuration.json',
            'cmake/vcpkg-windows-triplet-environment.cmake', 'cmake/vcpkg-msvc-145-14.51-toolchain.cmake',
            'cmake/SnowBuildToolchain.cmake', '.tools/vcpkg/scripts/buildsystems/vcpkg.cmake',
            '.tools/vcpkg/vcpkg.exe')) {
        $files.Add((Join-Path $script:SnowRepoRoot $path))
    }
    foreach ($directory in @('cmake/vcpkg-overlay-ports', 'cmake/vcpkg-overlay-triplets')) {
        $path = Join-Path $script:SnowRepoRoot $directory
        if (Test-Path -LiteralPath $path) {
            foreach ($file in Get-ChildItem -LiteralPath $path -File -Recurse) { $files.Add($file.FullName) }
        }
    }
    $environment = [ordered]@{}
    foreach ($name in @('CFLAGS', 'CXXFLAGS', 'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'FFMPEG_DIR',
            'Qt6_DIR', 'SNOW_QT_STATIC_DIR', 'QT_HOST_PATH', 'LIBCLANG_PATH', 'VSINSTALLDIR',
            'VCToolsInstallDir', 'VCToolsVersion', 'WindowsSdkDir', 'WindowsSDKVersion',
            'CMAKE_PREFIX_PATH', 'CMAKE_INCLUDE_PATH', 'CMAKE_LIBRARY_PATH', 'CC', 'CXX',
            'CL', '_CL_', 'LINK', '_LINK_', 'INCLUDE', 'LIB', 'LIBPATH')) {
        $environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue((Join-Path $script:SnowRepoRoot 'CMakePresets.json'))
    $pending.Enqueue((Join-Path $script:SnowRepoRoot 'CMakeUserPresets.json'))
    $visited = @{}
    while ($pending.Count) {
        $path = [IO.Path]::GetFullPath($pending.Dequeue())
        if ($visited.ContainsKey($path)) { continue }
        $visited[$path] = $true
        $files.Add($path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $text = Get-Content -Raw -LiteralPath $path
        foreach ($reference in [regex]::Matches($text, '\$p?env\{([^}]+)\}')) {
            $name = $reference.Groups[1].Value
            $environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        }
        $data = $text | ConvertFrom-Json
        if ($data.PSObject.Properties['include']) {
            foreach ($include in $data.include) {
                $expanded = $include.Replace('${sourceDir}', $script:SnowRepoRoot).
                    Replace('${sourceParentDir}', (Split-Path -Parent $script:SnowRepoRoot)).
                    Replace('${sourceDirName}', (Split-Path -Leaf $script:SnowRepoRoot)).
                    Replace('${fileDir}', (Split-Path -Parent $path)).Replace('${hostSystemName}', 'Windows').
                    Replace('${dollar}', '$').Replace('${pathListSep}', ';')
                $expanded = [regex]::Replace($expanded, '\$penv\{([^}]+)\}', {
                    param($match)
                    [Environment]::GetEnvironmentVariable($match.Groups[1].Value, 'Process')
                })
                if (-not [IO.Path]::IsPathRooted($expanded)) {
                    $expanded = Join-Path (Split-Path -Parent $path) $expanded
                }
                $pending.Enqueue($expanded)
            }
        }
    }
    $hashes = [ordered]@{}
    foreach ($path in @($files | Sort-Object -Unique)) { $hashes[$path] = Get-SnowFileHash $path }
    $tools = [ordered]@{}
    $toolNames = @('cmake', 'cargo', 'rustc', 'cl.exe', 'link.exe', 'lib.exe', 'rc.exe')
    if ($Preset -eq 'windows-clang-portability') { $toolNames += @('ninja', 'clang-cl.exe') }
    foreach ($name in $toolNames) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        $tools[$name] = if (-not $command) { '<missing>' }
            elseif ($command.CommandType -eq 'Application') { "$($command.Source):$(Get-SnowFileHash $command.Source)" }
            else { $command.Definition }
    }
    $state = [ordered]@{ Schema = 1; Preset = $Preset; Files = $hashes; Environment = $environment
        Tools = $tools; BuildEnvironment = $BuildEnvironment }
    return Get-SnowTextHash ($state | ConvertTo-Json -Depth 12 -Compress)
}

function Test-SnowConfigureState {
    param([string]$BuildDirectory, [string]$Fingerprint)
    $statePath = Join-Path $BuildDirectory 'snow-configure-state.json'
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $false }
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    $cache = Get-SnowCMakeCache $cachePath
    if ($cache['CMAKE_SUPPRESS_REGENERATION'] -match '^(?i:ON|TRUE|YES|1)$') { return $false }
    $graph = if ($cache['CMAKE_GENERATOR'] -eq 'Ninja') { 'build.ninja' } else { 'CMakeFiles/generate.stamp' }
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory $graph) -PathType Leaf)) { return $false }
    try {
        $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
        return $state.Schema -eq 1 -and $state.Fingerprint -ceq $Fingerprint -and
            $state.CacheHash -ceq (Get-SnowFileHash $cachePath)
    }
    catch { return $false }
}

function Save-SnowConfigureState {
    param([string]$BuildDirectory, [string]$Fingerprint)
    Write-SnowBuildState -Path (Join-Path $BuildDirectory 'snow-configure-state.json') -State @{
        Schema = 1; Fingerprint = $Fingerprint
        CacheHash = Get-SnowFileHash (Join-Path $BuildDirectory 'CMakeCache.txt')
    }
}
