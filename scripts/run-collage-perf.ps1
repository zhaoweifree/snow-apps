[CmdletBinding()]
param(
    [ValidateSet("windows-msvc-performance", "snow-shot-msvc-arm64-performance")]
    [string]$Preset = "windows-msvc-performance",
    [ValidateSet(20, 100)]
    [int[]]$Images = @(20, 100),
    [string]$OutputDirectory = "",
    [switch]$SkipBuild,
    [switch]$IncludeWorkflows
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$previousEnvironment = [Environment]::GetEnvironmentVariables("Process")

function Get-CollageBenchmarkFile {
    param([string]$Name, [string]$ProjectDirectory = "snow_shot", [string]$Extension = ".exe")
    $projectPath = Join-Path $repoRoot "build/$Preset/$ProjectDirectory/$Name.vcxproj"
    if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) {
        throw "The configured $Name project was not found. Run without -SkipBuild."
    }
    [xml]$benchmarkProject = Get-Content -Raw -LiteralPath $projectPath
    $releaseOutput = $benchmarkProject.SelectSingleNode(
        "//*[local-name()='OutDir' and contains(@Condition, 'Release')]")
    if (-not $releaseOutput) { throw "$Name does not define a Release output directory." }
    $output = $releaseOutput.InnerText.Trim()
    if (-not [IO.Path]::IsPathRooted($output)) {
        $output = Join-Path (Split-Path $projectPath) $output
    }
    $file = [IO.Path]::GetFullPath((Join-Path $output "$Name$Extension"))
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "The Release $Name binary was not found at $file. Run without -SkipBuild."
    }
    return $file
}

function Get-CollageGitPaths {
    param([string[]]$Arguments)
    # NUL-delimited names preserve spaces and Unicode without parsing Git's quoted display form.
    $output = (@(& git @Arguments)) -join ""
    if ($LASTEXITCODE -ne 0) { throw "Could not collect source paths from git $($Arguments -join ' ')." }
    return $output.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)
}

function Write-CollageSourceManifest {
    param([string]$Revision, [string]$Path)
    $modified = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $staged = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $untracked = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($entry in @(Get-CollageGitPaths @("ls-files", "--modified", "--deleted", "-z"))) {
        $null = $modified.Add($entry)
        $null = $paths.Add($entry)
    }
    foreach ($entry in @(Get-CollageGitPaths @("diff", "--cached", "--name-only", "--no-renames", "-z"))) {
        $null = $staged.Add($entry)
        $null = $paths.Add($entry)
    }
    foreach ($entry in @(Get-CollageGitPaths @("ls-files", "--others", "--exclude-standard", "-z"))) {
        $null = $untracked.Add($entry)
        $null = $paths.Add($entry)
    }
    $orderedPaths = [Collections.Generic.List[string]]::new($paths)
    $orderedPaths.Sort([StringComparer]::Ordinal)
    $files = @(
        foreach ($entry in $orderedPaths) {
            $sourcePath = Join-Path $repoRoot $entry
            $exists = Test-Path -LiteralPath $sourcePath -PathType Leaf
            if (-not $exists -and (Test-Path -LiteralPath $sourcePath)) {
                throw "The source manifest cannot hash the non-file path $entry."
            }
            [ordered]@{
                path = $entry
                deleted = -not $exists
                sha256 = if ($exists) {
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash.ToLowerInvariant()
                } else { $null }
                modified = $modified.Contains($entry)
                staged = $staged.Contains($entry)
                untracked = $untracked.Contains($entry)
            }
        }
    )
    $manifest = [ordered]@{ revision = $Revision; files = $files }
    [IO.File]::WriteAllText($Path, ($manifest | ConvertTo-Json -Depth 5) + "`n",
                          [Text.UTF8Encoding]::new($false))
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Assert-CollageImportReportsEquivalent {
    param([string]$Scenario)
    $fullPath = Join-Path $OutputDirectory "import-$Scenario-full.json"
    $optimizedPath = Join-Path $OutputDirectory "import-$Scenario-optimized.json"
    $full = Get-Content -Raw -LiteralPath $fullPath | ConvertFrom-Json -ErrorAction Stop
    $optimized = Get-Content -Raw -LiteralPath $optimizedPath | ConvertFrom-Json -ErrorAction Stop
    if ($full.scenario -ne $Scenario -or $optimized.scenario -ne $Scenario -or
        $full.implementation -ne "full" -or $optimized.implementation -ne "optimized" -or
        $full.width -le 0 -or $full.height -le 0 -or
        [string]::IsNullOrWhiteSpace([string]$full.checksum) -or
        $full.width -ne $optimized.width -or $full.height -ne $optimized.height -or
        $full.checksum -ne $optimized.checksum) {
        throw "The $Scenario import paths produced different pixels or invalid reports. Inspect $fullPath and $optimizedPath."
    }
}

function Invoke-CollageWorkflowBenchmark {
    param([string]$Name, [string]$Executable, [string[]]$Arguments,
          [ValidateSet("json", "csv", "log")][string]$ReportKind = "json")
    $log = Join-Path $OutputDirectory "$Name.log"
    $errorLog = Join-Path $OutputDirectory "$Name-stderr.log"
    $report = if ($ReportKind -eq "log") { $null } else {
        Join-Path $OutputDirectory "$Name.$ReportKind"
    }
    $started = [DateTime]::UtcNow
    # Each call starts a fresh process and finishes before the next scenario. Keep stderr out of
    # machine-readable stdout, while retaining both streams for failures and diagnostic warnings.
    $console = @(& $Executable @Arguments 2> $errorLog)
    $exitCode = $LASTEXITCODE
    $stdout = ($console | ForEach-Object { $_.ToString() }) -join "`n"
    [IO.File]::WriteAllText($log, $stdout + "`n", [Text.UTF8Encoding]::new($false))
    $metadata.results += [ordered]@{
        workflow = $Name
        executable = $Executable
        arguments = @($Arguments)
        startedUtc = $started.ToString("o")
        elapsedSeconds = ([DateTime]::UtcNow - $started).TotalSeconds
        report = $report
        log = $log
        stderrLog = $errorLog
        exitCode = $exitCode
    }
    $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding utf8
    if ($exitCode -ne 0) { throw "The $Name benchmark failed. Inspect $log and $errorLog." }
    if ($ReportKind -eq "json") {
        $null = $stdout | ConvertFrom-Json -ErrorAction Stop
    }
    if ($report) {
        [IO.File]::WriteAllText($report, $stdout + "`n", [Text.UTF8Encoding]::new($false))
    }
    Write-Output "Completed $Name; console: $log"
}

Push-Location $repoRoot
try {
    . (Join-Path $PSScriptRoot "snow-build-environment.ps1")
    $windowsTarget = Get-SnowWindowsTarget -Preset $Preset
    if ($windowsTarget.Architecture -cne $windowsTarget.HostArchitecture) {
        throw "Run collage performance measurements on a native $($windowsTarget.Architecture) host."
    }
    Set-SnowBuildEnvironment -Preset $Preset | Out-Null
    if (-not $SkipBuild) {
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "Collage benchmark configuration failed." }
        $buildTarget = if ($IncludeWorkflows) { "snow-shot-collage-workflow-benchmarks" } else {
            "snow-shot-collage-performance-benchmark"
        }
        & cmake --build --preset "build-$Preset" --target $buildTarget --parallel
        if ($LASTEXITCODE -ne 0) { throw "Collage benchmark build failed." }
    }

    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $stamp = Get-Date -Format "yyyyMMdd-HHmmssfff"
        $OutputDirectory = Join-Path $repoRoot "build/$Preset/collage-performance/$stamp"
    }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
    $executable = Get-CollageBenchmarkFile "snow-shot-collage-performance-benchmark"
    $workflowExecutables = [ordered]@{}
    if ($IncludeWorkflows) {
        foreach ($name in @("snow-shot-collage-rows-performance-benchmark",
                            "snow-shot-collage-workflow-performance-benchmark",
                            "snow-shot-collage-import-performance-benchmark",
                            "snow-shot-save-to-file-performance-benchmark",
                            "snow-shot-collage-clipboard-performance-benchmark")) {
            $workflowExecutables[$name] = Get-CollageBenchmarkFile $name
        }
        $workflowExecutables["snow_image_memory_performance_benchmark"] =
            Get-CollageBenchmarkFile "snow_image_memory_performance_benchmark" "snow_image/tools"
    }

    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $env:QTDIR "plugins/platforms"
    $runtime = Join-Path $windowsTarget.InstalledRoot "bin"
    $env:PATH = "$(Join-Path $env:QTDIR 'bin');$runtime;$env:PATH"

    $computer = Get-CimInstance Win32_ComputerSystem
    $processor = Get-CimInstance Win32_Processor | Select-Object -First 1
    $operatingSystem = Get-CimInstance Win32_OperatingSystem
    $qtManifest = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "qt-toolchain.json") | ConvertFrom-Json
    $revision = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Could not read the source revision." }
    $sourceManifestPath = Join-Path $OutputDirectory "source-manifest.json"
    $sourceManifestSha256 = Write-CollageSourceManifest $revision $sourceManifestPath
    $metadata = [ordered]@{
        startedUtc = [DateTime]::UtcNow.ToString("o")
        preset = $Preset
        configuration = "Release"
        architecture = $windowsTarget.Architecture
        hostArchitecture = $windowsTarget.HostArchitecture
        operatingSystem = $operatingSystem.Caption
        operatingSystemVersion = $operatingSystem.Version
        processor = $processor.Name
        physicalMemoryBytes = [long]$computer.TotalPhysicalMemory
        qtVersion = $qtManifest.qtVersion
        qtDirectory = $env:QTDIR
        revision = $revision
        dirtyFiles = @(& git status --short).Count
        sourceManifest = $sourceManifestPath
        sourceManifestSha256 = $sourceManifestSha256
        executableSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $executable).Hash.ToLowerInvariant()
        images = @($Images | Select-Object -Unique)
        imageSize = @(3840, 2160)
        editorSize = @(1200, 700)
        memorySamplingIntervalMs = 5
        clipboardPublished = $false
        fixture = "Opaque patterned RGB; independently decoded image rasters"
        memoryDefinitions = "Resident working set and Windows private committed bytes; clipboard admission estimates are separate from sampled memory."
        scenarioOrder = @("vertical unchanged", "vertical mixed transforms", "horizontal unchanged", "horizontal mixed transforms")
        includeWorkflows = [bool]$IncludeWorkflows
        importSampleCount = if ($IncludeWorkflows) { 7 } else { $null }
        resizeSampleCount = if ($IncludeWorkflows) { 7 } else { $null }
        saveSampleCount = if ($IncludeWorkflows) { 7 } else { $null }
        workflowOrder = if ($IncludeWorkflows) {
            @("rows", "editor-tiny", "editor-detail", "import-saved-pin-full",
              "import-saved-pin-optimized", "import-animated-file-full",
              "import-animated-file-optimized", "resize-extreme", "save", "clipboard")
        } else { @() }
        workflowExecutables = @($workflowExecutables.GetEnumerator() | ForEach-Object {
            [ordered]@{ target = $_.Key; path = $_.Value;
                         sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.Value).Hash.ToLowerInvariant() }
        })
        results = @()
    }
    $metadataPath = Join-Path $OutputDirectory "environment.json"
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8

    foreach ($count in @($Images | Select-Object -Unique)) {
        $report = Join-Path $OutputDirectory "results-$count.json"
        $log = Join-Path $OutputDirectory "console-$count.log"
        & $executable --images $count.ToString() --output $report 2>&1 | Tee-Object -FilePath $log
        $exitCode = $LASTEXITCODE
        $metadata.results += [ordered]@{ images = $count; report = $report; log = $log; exitCode = $exitCode }
        $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8
        if ($exitCode -ne 0) {
            throw "The $count-image collage benchmark failed. Inspect $log."
        }
    }
    if ($IncludeWorkflows) {
        $metadata['codecBackendSha256'] = (Get-FileHash -Algorithm SHA256 -LiteralPath (
            Get-CollageBenchmarkFile "snow_shot_image_codec_backend" "snow_shot" ".dll")).Hash.ToLowerInvariant()
        $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $metadataPath -Encoding utf8
        Invoke-CollageWorkflowBenchmark "rows" $workflowExecutables["snow-shot-collage-rows-performance-benchmark"] @()
        Invoke-CollageWorkflowBenchmark "editor-tiny" $workflowExecutables["snow-shot-collage-workflow-performance-benchmark"] @("--tiny-only")
        Invoke-CollageWorkflowBenchmark "editor-detail" $workflowExecutables["snow-shot-collage-workflow-performance-benchmark"] @("--detail-only")
        foreach ($scenario in @("saved-pin", "animated-file")) {
            foreach ($mode in @("full", "optimized")) {
                Invoke-CollageWorkflowBenchmark "import-$scenario-$mode" $workflowExecutables["snow-shot-collage-import-performance-benchmark"] @(
                    "--scenario", $scenario, "--mode", $mode, "--width", "3840", "--height", "2160", "--count", "7")
            }
            Assert-CollageImportReportsEquivalent $scenario
        }
        Invoke-CollageWorkflowBenchmark "resize-extreme" $workflowExecutables["snow_image_memory_performance_benchmark"] @(
            "--scenario", "resize-extreme", "--width", "32768", "--height", "2", "--warmup", "1", "--count", "7") "csv"
        Invoke-CollageWorkflowBenchmark "save" $workflowExecutables["snow-shot-save-to-file-performance-benchmark"] @("--samples", "7") "log"
        Invoke-CollageWorkflowBenchmark "clipboard" $workflowExecutables["snow-shot-collage-clipboard-performance-benchmark"] @()
    }
    $metadata['finishedUtc'] = [DateTime]::UtcNow.ToString("o")
    $metadata | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $metadataPath -Encoding utf8
    Write-Output "Collage benchmark reports: $OutputDirectory"
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
