param(
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [ValidateRange(1, 100)]
    [double]$MaxCpuPercent = 15,
    [ValidateRange(1, 100)]
    [double]$MaxCpuPeakPercent = 50,
    [ValidateRange(320, 3840)]
    [int]$VideoWidth = 1920,
    [ValidateRange(180, 2160)]
    [int]$VideoHeight = 1080,
    [ValidateRange(1, 60)]
    [int]$VideoFps = 30,
    [ValidateRange(300000, 20000000)]
    [int]$VideoBitrateBps = 4000000,
    [switch]$SingleRealCamera,
    [switch]$SingleRealAudio,
    [switch]$SyntheticCamera,
    [switch]$RealAudio,
    [switch]$RequireAudio,
    [switch]$RequireAvSync,
    [string]$HostCameraDevice = "",
    [string]$GuestCameraDevice = "",
    [string]$HostAudioInputDevice = "",
    [string]$HostAudioOutputDevice = "",
    [string]$GuestAudioInputDevice = "",
    [string]$GuestAudioOutputDevice = "",
    [string]$HostAudioDevice = "",
    [string]$GuestAudioDevice = "",
    [ValidateSet("", "auto", "qt", "dshow", "ffmpeg-process", "ffmpeg")]
    [string]$CameraCaptureBackend = "",
    [ValidateRange(0, 200)]
    [int]$AvSyncMaxSkewMs = 40,
    [ValidateRange(0, 240)]
    [int]$SoakMinutes = 0
)

$ErrorActionPreference = "Stop"

$soakMs = $SoakMinutes * 60 * 1000
$runtimeTimeoutMs = if ($soakMs -gt 0) { $soakMs + 90000 } else { 30000 }
$processTimeoutMs = if ($soakMs -gt 0) { $soakMs + 120000 } else { 45000 }
$ctestTimeoutSec = [Math]::Ceiling(($processTimeoutMs + 60000) / 1000.0)

if ([string]::IsNullOrWhiteSpace($HostAudioInputDevice) -and -not [string]::IsNullOrWhiteSpace($HostAudioDevice)) {
    $HostAudioInputDevice = $HostAudioDevice
}
if ([string]::IsNullOrWhiteSpace($HostAudioOutputDevice) -and -not [string]::IsNullOrWhiteSpace($HostAudioDevice)) {
    $HostAudioOutputDevice = $HostAudioDevice
}
if ([string]::IsNullOrWhiteSpace($GuestAudioInputDevice) -and -not [string]::IsNullOrWhiteSpace($GuestAudioDevice)) {
    $GuestAudioInputDevice = $GuestAudioDevice
}
if ([string]::IsNullOrWhiteSpace($GuestAudioOutputDevice) -and -not [string]::IsNullOrWhiteSpace($GuestAudioDevice)) {
    $GuestAudioOutputDevice = $GuestAudioDevice
}

$env:MEETING_PROCESS_SMOKE_REQUIRE_VIDEO = "1"
$env:MEETING_PROCESS_SMOKE_REQUIRE_CPU = "1"
$env:MEETING_PROCESS_SMOKE_MAX_CPU_PERCENT = "$MaxCpuPercent"
$env:MEETING_PROCESS_SMOKE_MAX_CPU_PEAK_PERCENT = "$MaxCpuPeakPercent"
$env:MEETING_PROCESS_SMOKE_CPU_VIDEO_WIDTH = "$VideoWidth"
$env:MEETING_PROCESS_SMOKE_CPU_VIDEO_HEIGHT = "$VideoHeight"
$env:MEETING_PROCESS_SMOKE_CPU_VIDEO_FPS = "$VideoFps"
$env:MEETING_PROCESS_SMOKE_CPU_VIDEO_BITRATE_BPS = "$VideoBitrateBps"
$env:MEETING_PROCESS_SMOKE_SINGLE_REAL_CAMERA = if ($SingleRealCamera) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_SINGLE_REAL_AUDIO = if ($SingleRealAudio) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA = if ($SyntheticCamera) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_SYNTHETIC_AUDIO = if ($RealAudio) { "0" } else { "1" }
$env:MEETING_PROCESS_SMOKE_REQUIRE_AUDIO = if ($RequireAudio) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_REQUIRE_AVSYNC = if ($RequireAvSync) { "1" } else { "0" }
$env:MEETING_PROCESS_SMOKE_AVSYNC_MAX_SKEW_MS = "$AvSyncMaxSkewMs"
$env:MEETING_PROCESS_SMOKE_HOST_CAMERA_DEVICE = $HostCameraDevice
$env:MEETING_PROCESS_SMOKE_GUEST_CAMERA_DEVICE = $GuestCameraDevice
$env:MEETING_PROCESS_SMOKE_HOST_AUDIO_INPUT_DEVICE = $HostAudioInputDevice
$env:MEETING_PROCESS_SMOKE_HOST_AUDIO_OUTPUT_DEVICE = $HostAudioOutputDevice
$env:MEETING_PROCESS_SMOKE_GUEST_AUDIO_INPUT_DEVICE = $GuestAudioInputDevice
$env:MEETING_PROCESS_SMOKE_GUEST_AUDIO_OUTPUT_DEVICE = $GuestAudioOutputDevice
$env:MEETING_PROCESS_SMOKE_RUNTIME_TIMEOUT_MS = "$runtimeTimeoutMs"
$env:MEETING_PROCESS_SMOKE_TIMEOUT_MS = "$processTimeoutMs"
if (-not [string]::IsNullOrWhiteSpace($CameraCaptureBackend)) {
    $env:MEETING_CAMERA_CAPTURE_BACKEND = $CameraCaptureBackend
} else {
    Remove-Item Env:\MEETING_CAMERA_CAPTURE_BACKEND -ErrorAction SilentlyContinue
}

if ($soakMs -gt 0) {
    $env:MEETING_PROCESS_SMOKE_SOAK_MS = "$soakMs"
} else {
    Remove-Item Env:\MEETING_PROCESS_SMOKE_SOAK_MS -ErrorAction SilentlyContinue
}

Write-Host "[M2-5] build_dir=$BuildDir"
Write-Host "[M2-5] cpu_threshold_avg=$MaxCpuPercent cpu_threshold_peak=$MaxCpuPeakPercent video=$VideoWidth`x$VideoHeight@$VideoFps bitrate_bps=$VideoBitrateBps"
Write-Host "[M2-5] synthetic_camera=$($SyntheticCamera.IsPresent) single_real_camera=$($SingleRealCamera.IsPresent) single_real_audio=$($SingleRealAudio.IsPresent) real_audio=$($RealAudio.IsPresent) require_audio=$($RequireAudio.IsPresent) require_avsync=$($RequireAvSync.IsPresent) avsync_max_skew_ms=$AvSyncMaxSkewMs soak_minutes=$SoakMinutes"
Write-Host "[M2-5] camera_device_override host='$HostCameraDevice' guest='$GuestCameraDevice'"
Write-Host "[M2-5] audio_device_override host_in='$HostAudioInputDevice' host_out='$HostAudioOutputDevice' guest_in='$GuestAudioInputDevice' guest_out='$GuestAudioOutputDevice'"
Write-Host "[M2-5] runtime_timeout_ms=$runtimeTimeoutMs process_timeout_ms=$processTimeoutMs ctest_timeout_sec=$ctestTimeoutSec"

if (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "Build directory not found: $BuildDir"
}

Push-Location $BuildDir
try {
    $smokeExe = Join-Path $BuildDir "Debug\test_meeting_client_process_smoke.exe"
    if (-not (Test-Path -LiteralPath $smokeExe)) {
        throw "Process smoke executable not found: $smokeExe"
    }
    & $smokeExe
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}
