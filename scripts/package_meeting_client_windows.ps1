param(
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$OutputDir = "D:\meeting\dist\meeting_client_win64_portable",
    [string]$FfmpegRoot = "D:\Setup\ffmpeg-7.1-full_build-shared",
    [string]$QtDeployPath = "",
    [switch]$SkipBuild,
    [switch]$Zip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-WinDeployQt {
    param([string]$Hint)

    if (-not [string]::IsNullOrWhiteSpace($Hint)) {
        if (-not (Test-Path -LiteralPath $Hint)) {
            throw "windeployqt not found: $Hint"
        }
        return $Hint
    }

    $candidates = @(where.exe windeployqt 2>$null)
    if ($candidates.Count -eq 0) {
        throw "windeployqt not found in PATH"
    }

    foreach ($candidate in $candidates) {
        if ($candidate -match "msvc2022_64") {
            return $candidate
        }
    }
    return $candidates[0]
}

function Copy-IfExists {
    param(
        [string]$Source,
        [string]$DestinationDir
    )
    if (Test-Path -LiteralPath $Source) {
        Copy-Item -LiteralPath $Source -Destination $DestinationDir -Force
        return $true
    }
    return $false
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$clientProjectDir = Join-Path $repoRoot "plasma-hawking"
$exePath = Join-Path $BuildDir "$Config\meeting_client.exe"
$winDeployQt = Resolve-WinDeployQt -Hint $QtDeployPath
$configLower = $Config.ToLowerInvariant()

Write-Host "[package] build_dir=$BuildDir config=$Config"
Write-Host "[package] output_dir=$OutputDir"
Write-Host "[package] windeployqt=$winDeployQt"

if (-not $SkipBuild) {
    & cmake --build $BuildDir --config $Config --target meeting_client
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
}

if (-not (Test-Path -LiteralPath $exePath)) {
    throw "meeting_client.exe not found: $exePath"
}

if (Test-Path -LiteralPath $OutputDir) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDir | Out-Null

Copy-Item -LiteralPath $exePath -Destination $OutputDir -Force

$deployArgs = @(
    "--$configLower"
    "--qmldir"
    (Join-Path $clientProjectDir "qml")
    "--compiler-runtime"
    "--dir"
    $OutputDir
    (Join-Path $OutputDir "meeting_client.exe")
)

& $winDeployQt @deployArgs
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed."
}

$ffmpegBinDir = Join-Path $FfmpegRoot "bin"
if (-not (Test-Path -LiteralPath $ffmpegBinDir)) {
    throw "FFmpeg runtime bin dir not found: $ffmpegBinDir"
}

$ffmpegDllPatterns = @(
    "avcodec-*.dll",
    "avutil-*.dll",
    "swresample-*.dll",
    "swscale-*.dll"
)

foreach ($pattern in $ffmpegDllPatterns) {
    $matches = Get-ChildItem -Path $ffmpegBinDir -Filter $pattern -File -ErrorAction SilentlyContinue
    foreach ($match in $matches) {
        Copy-Item -LiteralPath $match.FullName -Destination $OutputDir -Force
    }
}

$manifestPath = Join-Path $OutputDir "PACKAGE_MANIFEST.txt"
$gitCommit = (git -C $repoRoot rev-parse --short HEAD).Trim()
$manifest = @(
    "meeting_client portable package"
    "generated_at=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "git_commit=$gitCommit"
    "build_config=$Config"
    "exe=meeting_client.exe"
    "qt_deploy=$winDeployQt"
    "ffmpeg_root=$FfmpegRoot"
)
$manifest | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if ($Zip) {
    $zipPath = "$OutputDir.zip"
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path "$OutputDir\*" -DestinationPath $zipPath -Force
    Write-Host "[package] zip=$zipPath"
}

Write-Host "[package] done: $OutputDir"
