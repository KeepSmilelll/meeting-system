param(
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [int]$Iterations = 2,
    [switch]$IncludeRuntimeSmoke,
    [switch]$RealAudio,
    [switch]$EnableCpuGuard
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($Iterations -le 0) {
    throw "Iterations must be > 0"
}

$audioSmokeExe = Join-Path $BuildDir "Debug\audio_codec_smoke.exe"
$runtimeSmokeExe = Join-Path $BuildDir "Debug\test_meeting_runtime_smoke.exe"

if (-not (Test-Path -LiteralPath $audioSmokeExe)) {
    throw "audio_codec_smoke executable not found: $audioSmokeExe"
}
if ($IncludeRuntimeSmoke.IsPresent -and -not (Test-Path -LiteralPath $runtimeSmokeExe)) {
    throw "test_meeting_runtime_smoke executable not found: $runtimeSmokeExe"
}

$profiles = @(
    @{
        Name = "default_chain"
        Env = @{
            "MEETING_AUDIO_AEC" = "1"
            "MEETING_AUDIO_NS" = "1"
            "MEETING_AUDIO_AGC" = "1"
            "MEETING_AUDIO_AEC_SUPPRESSION" = "0.85"
            "MEETING_AUDIO_AEC_HISTORY_MS" = "300"
            "MEETING_AUDIO_NS_MAX_DB" = "18"
            "MEETING_AUDIO_NS_FLOOR_GAIN" = "0.18"
            "MEETING_AUDIO_AGC_TARGET_RMS" = "0.12"
            "MEETING_AUDIO_AGC_MIN_GAIN" = "0.25"
            "MEETING_AUDIO_AGC_MAX_GAIN" = "8.0"
        }
    },
    @{
        Name = "aggressive_ns"
        Env = @{
            "MEETING_AUDIO_AEC" = "1"
            "MEETING_AUDIO_NS" = "1"
            "MEETING_AUDIO_AGC" = "1"
            "MEETING_AUDIO_AEC_SUPPRESSION" = "0.90"
            "MEETING_AUDIO_AEC_HISTORY_MS" = "400"
            "MEETING_AUDIO_NS_MAX_DB" = "24"
            "MEETING_AUDIO_NS_FLOOR_GAIN" = "0.10"
            "MEETING_AUDIO_AGC_TARGET_RMS" = "0.10"
            "MEETING_AUDIO_AGC_MIN_GAIN" = "0.20"
            "MEETING_AUDIO_AGC_MAX_GAIN" = "6.0"
        }
    },
    @{
        Name = "speech_preserve"
        Env = @{
            "MEETING_AUDIO_AEC" = "1"
            "MEETING_AUDIO_NS" = "1"
            "MEETING_AUDIO_AGC" = "1"
            "MEETING_AUDIO_AEC_SUPPRESSION" = "0.75"
            "MEETING_AUDIO_AEC_HISTORY_MS" = "250"
            "MEETING_AUDIO_NS_MAX_DB" = "14"
            "MEETING_AUDIO_NS_FLOOR_GAIN" = "0.30"
            "MEETING_AUDIO_AGC_TARGET_RMS" = "0.14"
            "MEETING_AUDIO_AGC_MIN_GAIN" = "0.35"
            "MEETING_AUDIO_AGC_MAX_GAIN" = "5.5"
        }
    },
    @{
        Name = "no_process_control"
        Env = @{
            "MEETING_AUDIO_AEC" = "0"
            "MEETING_AUDIO_NS" = "0"
            "MEETING_AUDIO_AGC" = "0"
        }
    }
)

$baseRuntimeEnv = @{
    "MEETING_RUNTIME_SMOKE_SYNTHETIC_AUDIO" = $(if ($RealAudio.IsPresent) { "0" } else { "1" })
    "MEETING_RUNTIME_SMOKE_SYNTHETIC_CAMERA" = "1"
}

if ($EnableCpuGuard.IsPresent) {
    $baseRuntimeEnv["MEETING_RUNTIME_SMOKE_REQUIRE_AVSYNC"] = "1"
}

$allKnownKeys = @(
    "MEETING_AUDIO_AEC",
    "MEETING_AUDIO_NS",
    "MEETING_AUDIO_AGC",
    "MEETING_AUDIO_AEC_SUPPRESSION",
    "MEETING_AUDIO_AEC_HISTORY_MS",
    "MEETING_AUDIO_NS_MAX_DB",
    "MEETING_AUDIO_NS_FLOOR_GAIN",
    "MEETING_AUDIO_AGC_TARGET_RMS",
    "MEETING_AUDIO_AGC_MIN_GAIN",
    "MEETING_AUDIO_AGC_MAX_GAIN",
    "MEETING_RUNTIME_SMOKE_SYNTHETIC_AUDIO",
    "MEETING_RUNTIME_SMOKE_SYNTHETIC_CAMERA",
    "MEETING_RUNTIME_SMOKE_REQUIRE_AVSYNC"
)

$savedEnv = @{}
foreach ($key in $allKnownKeys) {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
}

$results = New-Object System.Collections.Generic.List[object]

function Invoke-SmokeCase {
    param(
        [string]$Profile,
        [int]$Iteration,
        [string]$CaseName,
        [string]$ExePath
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    & $ExePath
    $exitCode = $LASTEXITCODE
    $stopwatch.Stop()

    $results.Add([pscustomobject]@{
        profile = $Profile
        iteration = $Iteration
        case = $CaseName
        exit_code = $exitCode
        duration_ms = [int]$stopwatch.ElapsedMilliseconds
        passed = ($exitCode -eq 0)
    }) | Out-Null

    $status = if ($exitCode -eq 0) { "PASS" } else { "FAIL($exitCode)" }
    Write-Host ("[audio-tuning] profile={0} iter={1} case={2} => {3} ({4} ms)" -f $Profile, $Iteration, $CaseName, $status, [int]$stopwatch.ElapsedMilliseconds)
}

try {
    foreach ($profile in $profiles) {
        foreach ($key in $allKnownKeys) {
            [Environment]::SetEnvironmentVariable($key, $null, "Process")
        }
        foreach ($pair in $profile.Env.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($pair.Key, [string]$pair.Value, "Process")
        }

        Write-Host ("[audio-tuning] ===== profile={0} =====" -f $profile.Name)
        foreach ($pair in $profile.Env.GetEnumerator()) {
            Write-Host ("[audio-tuning]   {0}={1}" -f $pair.Key, $pair.Value)
        }

        for ($i = 1; $i -le $Iterations; $i++) {
            Invoke-SmokeCase -Profile $profile.Name -Iteration $i -CaseName "audio_codec_smoke" -ExePath $audioSmokeExe
            if ($IncludeRuntimeSmoke.IsPresent) {
                foreach ($pair in $baseRuntimeEnv.GetEnumerator()) {
                    [Environment]::SetEnvironmentVariable($pair.Key, [string]$pair.Value, "Process")
                }
                Invoke-SmokeCase -Profile $profile.Name -Iteration $i -CaseName "test_meeting_runtime_smoke" -ExePath $runtimeSmokeExe
            }
        }
    }
}
finally {
    foreach ($key in $allKnownKeys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], "Process")
    }
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$artifactDir = Join-Path $env:TEMP "meeting-audio-tuning"
New-Item -Path $artifactDir -ItemType Directory -Force | Out-Null
$csvPath = Join-Path $artifactDir ("audio_tuning_results_{0}.csv" -f $timestamp)
$results | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

Write-Host "[audio-tuning] ===== summary ====="
$summary = $results | Group-Object profile, case | Sort-Object Name
foreach ($group in $summary) {
    $passCount = @($group.Group | Where-Object { $_.passed }).Count
    $totalCount = $group.Count
    $avgDuration = [int](($group.Group | Measure-Object duration_ms -Average).Average)
    Write-Host ("[audio-tuning] {0} pass={1}/{2} avg={3} ms" -f $group.Name, $passCount, $totalCount, $avgDuration)
}
Write-Host ("[audio-tuning] results_csv={0}" -f $csvPath)
