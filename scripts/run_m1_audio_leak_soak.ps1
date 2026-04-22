param(
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [ValidateRange(1, 240)]
    [int]$DurationMinutes = 30,
    [switch]$SyntheticAudio,
    [ValidateRange(0, 600)]
    [int]$MemoryBaselineDelaySeconds = 15,
    [ValidateRange(0, 4096)]
    [int]$MaxWorkingSetGrowthMb = 256,
    [ValidateRange(0, 1800)]
    [int]$CtestExtraTimeoutSeconds = 180,
    [switch]$EnableAsanLeakChecks
)

$ErrorActionPreference = "Stop"

$soakMs = $DurationMinutes * 60 * 1000
$runtimeTimeoutMs = $soakMs + 90000
$processTimeoutMs = $soakMs + 120000
$memoryBaselineDelayMs = [Math]::Max(0, $MemoryBaselineDelaySeconds * 1000)
$ctestTimeoutSec = [Math]::Ceiling(($processTimeoutMs + 60000) / 1000.0) + $CtestExtraTimeoutSeconds

$env:MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO = if ($SyntheticAudio) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_REQUIRE_AUDIO = "1"
$env:MEETING_PROCESS_SMOKE_SOAK_MS = "$soakMs"
$env:MEETING_PROCESS_SMOKE_RUNTIME_TIMEOUT_MS = "$runtimeTimeoutMs"
$env:MEETING_PROCESS_SMOKE_TIMEOUT_MS = "$processTimeoutMs"
$env:MEETING_PROCESS_SMOKE_MAX_WORKING_SET_GROWTH_MB = "$MaxWorkingSetGrowthMb"
$env:MEETING_PROCESS_SMOKE_MEMORY_BASELINE_DELAY_MS = "$memoryBaselineDelayMs"

if ($EnableAsanLeakChecks) {
    if ([string]::IsNullOrWhiteSpace($env:ASAN_OPTIONS)) {
        $env:ASAN_OPTIONS = "detect_leaks=1:halt_on_error=1"
    } elseif ($env:ASAN_OPTIONS -notmatch "detect_leaks=") {
        $env:ASAN_OPTIONS = "$($env:ASAN_OPTIONS):detect_leaks=1:halt_on_error=1"
    }
}

Write-Host "[M1-5] build_dir=$BuildDir"
Write-Host "[M1-5] synthetic_audio=$($SyntheticAudio.IsPresent) soak_minutes=$DurationMinutes soak_ms=$soakMs"
Write-Host "[M1-5] runtime_timeout_ms=$runtimeTimeoutMs process_timeout_ms=$processTimeoutMs max_ws_growth_mb=$MaxWorkingSetGrowthMb baseline_delay_ms=$memoryBaselineDelayMs ctest_timeout_sec=$ctestTimeoutSec ctest_extra_timeout_sec=$CtestExtraTimeoutSeconds"
if ($EnableAsanLeakChecks) {
    Write-Host "[M1-5] ASAN_OPTIONS=$env:ASAN_OPTIONS"
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "Build directory not found: $BuildDir"
}

$smokeExe = Join-Path $BuildDir "Debug\test_meeting_client_process_smoke.exe"
if (-not (Test-Path -LiteralPath $smokeExe)) {
    throw "Process smoke executable not found: $smokeExe"
}

Push-Location $BuildDir
try {
    & $smokeExe
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}
