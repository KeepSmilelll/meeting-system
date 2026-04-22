param(
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [switch]$UseSyntheticCamera,
    [switch]$UseSingleRealAudio
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$smokeExe = Join-Path $BuildDir "Debug\test_meeting_client_process_smoke.exe"
if (-not (Test-Path -LiteralPath $smokeExe)) {
    throw "process smoke executable not found: $smokeExe"
}

# Default device matrix for this workstation. Override by editing this list or
# duplicating the script for CI host-specific presets.
$cases = @(
    @{
        Name = "edifier_pair"
        HostInput = "Microphone (EDIFIER Halo Soundbar)"
        HostOutput = "扬声器 (EDIFIER Halo Soundbar)"
    },
    @{
        Name = "droidcam_pair"
        HostInput = "麦克风 (DroidCam Audio)"
        HostOutput = "内部 AUX 插座 (DroidCam Audio)"
    }
)

$baseEnv = @{
    "MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO" = "0"
    "MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA" = $(if ($UseSyntheticCamera.IsPresent) { "1" } else { "0" })
    "MEETING_PROCESS_SMOKE_SINGLE_REAL_AUDIO" = $(if ($UseSingleRealAudio.IsPresent) { "1" } else { "0" })
}

$allKnownKeys = @(
    "MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO",
    "MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA",
    "MEETING_PROCESS_SMOKE_SINGLE_REAL_AUDIO",
    "MEETING_PROCESS_SMOKE_HOST_AUDIO_INPUT_DEVICE",
    "MEETING_PROCESS_SMOKE_HOST_AUDIO_OUTPUT_DEVICE",
    "MEETING_PROCESS_SMOKE_GUEST_AUDIO_INPUT_DEVICE",
    "MEETING_PROCESS_SMOKE_GUEST_AUDIO_OUTPUT_DEVICE"
)

$savedEnv = @{}
foreach ($key in $allKnownKeys) {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
}

$results = New-Object System.Collections.Generic.List[object]

try {
    foreach ($case in $cases) {
        foreach ($key in $allKnownKeys) {
            [Environment]::SetEnvironmentVariable($key, $null, "Process")
        }
        foreach ($pair in $baseEnv.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($pair.Key, [string]$pair.Value, "Process")
        }
        [Environment]::SetEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_INPUT_DEVICE", [string]$case.HostInput, "Process")
        [Environment]::SetEnvironmentVariable("MEETING_PROCESS_SMOKE_HOST_AUDIO_OUTPUT_DEVICE", [string]$case.HostOutput, "Process")
        [Environment]::SetEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_INPUT_DEVICE", "", "Process")
        [Environment]::SetEnvironmentVariable("MEETING_PROCESS_SMOKE_GUEST_AUDIO_OUTPUT_DEVICE", "", "Process")

        Write-Host ("[audio-device-matrix] ===== case={0} =====" -f $case.Name)
        Write-Host ("[audio-device-matrix]   host_input={0}" -f $case.HostInput)
        Write-Host ("[audio-device-matrix]   host_output={0}" -f $case.HostOutput)

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $smokeExe
        $exitCode = $LASTEXITCODE
        $sw.Stop()

        $status = if ($exitCode -eq 0) { "PASS" } elseif ($exitCode -eq 77) { "SKIP" } else { "FAIL($exitCode)" }
        Write-Host ("[audio-device-matrix] case={0} => {1} ({2} ms)" -f $case.Name, $status, [int]$sw.ElapsedMilliseconds)

        $results.Add([pscustomobject]@{
            case_name = $case.Name
            host_input = $case.HostInput
            host_output = $case.HostOutput
            synthetic_camera = $baseEnv["MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA"]
            single_real_audio = $baseEnv["MEETING_PROCESS_SMOKE_SINGLE_REAL_AUDIO"]
            exit_code = $exitCode
            passed = ($exitCode -eq 0)
            skipped = ($exitCode -eq 77)
            duration_ms = [int]$sw.ElapsedMilliseconds
        }) | Out-Null
    }
}
finally {
    foreach ($key in $allKnownKeys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], "Process")
    }
}

$artifactDir = Join-Path $env:TEMP "meeting-audio-device-matrix"
New-Item -Path $artifactDir -ItemType Directory -Force | Out-Null
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$csvPath = Join-Path $artifactDir ("audio_device_matrix_{0}.csv" -f $timestamp)
$results | Export-Csv -Path $csvPath -NoTypeInformation -Encoding UTF8

$passCount = @($results | Where-Object { $_.passed }).Count
$skipCount = @($results | Where-Object { $_.skipped }).Count
$failCount = @($results | Where-Object { -not $_.passed -and -not $_.skipped }).Count

Write-Host "[audio-device-matrix] ===== summary ====="
Write-Host ("[audio-device-matrix] pass={0} skip={1} fail={2} total={3}" -f $passCount, $skipCount, $failCount, $results.Count)
Write-Host ("[audio-device-matrix] results_csv={0}" -f $csvPath)

if ($failCount -gt 0) {
    exit 1
}

exit 0

