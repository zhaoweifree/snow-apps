[CmdletBinding()]
param(
    [switch]$Fix,
    [string[]]$Workspace = @(),
    [string[]]$Package = @()
)

$ErrorActionPreference = "Stop"
$workspaceRoot = Split-Path -Parent $PSScriptRoot

# cargo check/clippy execute the ffmpeg-sys-next build script, which locates
# FFmpeg through FFMPEG_DIR, vcpkg, or pkg-config. The CMake build always sets
# FFMPEG_DIR explicitly; when linting standalone, fall back to the
# repository-managed vcpkg installation so the documented command works without
# a configured build tree.
if ([string]::IsNullOrWhiteSpace($env:FFMPEG_DIR)) {
    $ffmpegCandidates = @(
        ".tools\vcpkg\installed\dynamic\x64-windows",
        ".tools\vcpkg\installed\static\x64-windows",
        ".tools\vcpkg\installed\static\x64-windows-static"
    )
    $ffmpegRoot = $ffmpegCandidates |
        ForEach-Object { Join-Path $workspaceRoot $_ } |
        Where-Object { Test-Path (Join-Path $_ "include\libavutil") } |
        Select-Object -First 1
    if ($ffmpegRoot) {
        $env:FFMPEG_DIR = $ffmpegRoot
    }
}

$rustWorkspaces = @(
    (Join-Path $workspaceRoot "snow-crates"),
    (Join-Path $workspaceRoot "snow_draw_engine_qt"),
    (Join-Path $workspaceRoot "snow_shot\rust\snow-shot-updater"),
    (Join-Path $workspaceRoot "snow_shot\rust\snow-shot-mcp")
)

if ($Workspace.Count -gt 0) {
    $rustWorkspaces = @($Workspace | ForEach-Object { (Resolve-Path (Join-Path $workspaceRoot $_)).Path })
}
. (Join-Path $PSScriptRoot 'snow-ocr-toolchain.ps1')
foreach ($rustWorkspace in $rustWorkspaces) {
    Push-Location $rustWorkspace
    try {
        $isSnowCrates = $rustWorkspace -eq (Join-Path $workspaceRoot 'snow-crates')
        $passes = @()
        $stablePackages = @($Package | Where-Object { $_ -ne 'snow-ocr-process' })
        if ($Package.Count -eq 0) {
            $selection = @('--workspace')
            if ($isSnowCrates) { $selection += @('--exclude', 'snow-ocr-process') }
            $passes += @{ Toolchain = @(); Selection = $selection; Format = @('--all') }
        } elseif ($stablePackages.Count -gt 0) {
            $selection = @($stablePackages | ForEach-Object { '-p'; $_ })
            $passes += @{ Toolchain = @(); Selection = $selection; Format = $selection }
        }
        if ($isSnowCrates -and ($Package.Count -eq 0 -or 'snow-ocr-process' -in $Package)) {
            $passes += @{ Toolchain = @("+$(Get-SnowOcrToolchain)"); Selection = @('-p', 'snow-ocr-process'); Format = @('-p', 'snow-ocr-process') }
        }
        foreach ($pass in $passes) {
            $toolchain = $pass.Toolchain
            $selection = $pass.Selection
            $format = $pass.Format
            $formatOptions = if ($Fix) { @() } else { @('--', '--check') }
            & cargo @toolchain fmt @format @formatOptions
            if ($LASTEXITCODE -ne 0) { throw "rustfmt failed in $rustWorkspace" }
            & cargo @toolchain check @selection --all-targets --all-features
            if ($LASTEXITCODE -ne 0) { throw "cargo check failed in $rustWorkspace" }
            & cargo @toolchain clippy @selection --all-targets --all-features -- -D warnings
            if ($LASTEXITCODE -ne 0) { throw "Clippy failed in $rustWorkspace" }
        }
    } finally { Pop-Location }
}
