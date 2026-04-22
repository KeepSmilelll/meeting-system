param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$BuildDir = "D:\meeting\plasma-hawking\build",
    [string]$ServerDir = "D:\meeting\meeting-server\signaling",
    [switch]$SyntheticAudio,
    [switch]$SyntheticCamera,
    [switch]$RequireVideo,
    [switch]$RequireAudio,
    [switch]$RequireAvSync,
    [switch]$UiRequireAvSync,
    [ValidateRange(0, 200)]
    [int]$AvSyncMaxSkewMs = 40,
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
    [switch]$ExpectRealCamera,
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
    [switch]$Headless,
    [ValidateRange(10, 300)]
    [int]$UiTimeoutSeconds = 60,
    [ValidateRange(0, 240)]
    [int]$SoakMinutes = 0,
    [switch]$SkipUiSmoke
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$useSyntheticAudio = if ($PSBoundParameters.ContainsKey("SyntheticAudio")) { [bool]$SyntheticAudio } else { $true }
$useSyntheticCamera = if ($PSBoundParameters.ContainsKey("SyntheticCamera")) { [bool]$SyntheticCamera } else { $true }
$useRequireVideo = if ($PSBoundParameters.ContainsKey("RequireVideo")) { [bool]$RequireVideo } else { $true }
$useRequireAudio = if ($PSBoundParameters.ContainsKey("RequireAudio")) { [bool]$RequireAudio } else { $true }
$useRequireAvSync = if ($PSBoundParameters.ContainsKey("RequireAvSync")) { [bool]$RequireAvSync } else { $true }
if ($SingleRealAudio -and -not $PSBoundParameters.ContainsKey("SyntheticAudio")) {
    $useSyntheticAudio = $false
}
if ($ExpectRealCamera -and -not $PSBoundParameters.ContainsKey("SyntheticCamera")) {
    $useSyntheticCamera = $false
}
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

Write-Host "[M2] config synthetic_audio=$useSyntheticAudio synthetic_camera=$useSyntheticCamera single_real_audio=$($SingleRealAudio.IsPresent) require_video=$useRequireVideo require_audio=$useRequireAudio require_avsync=$useRequireAvSync ui_require_avsync=$($UiRequireAvSync.IsPresent)"

if ($ExpectRealCamera -and $useSyntheticCamera) {
    throw "ExpectRealCamera cannot be used together with SyntheticCamera=true"
}
if ($SingleRealCamera -and $useSyntheticCamera) {
    throw "SingleRealCamera cannot be used together with SyntheticCamera=true"
}
if ($SingleRealAudio -and $useSyntheticAudio) {
    throw "SingleRealAudio cannot be used together with SyntheticAudio=true"
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$uiScript = Join-Path $scriptRoot "run_meeting_client_dual_ui_smoke.ps1"
$cpuScript = Join-Path $scriptRoot "run_m2_cpu_budget_smoke.ps1"

if (-not (Test-Path -LiteralPath $uiScript)) {
    throw "UI smoke script not found: $uiScript"
}
if (-not (Test-Path -LiteralPath $cpuScript)) {
    throw "CPU smoke script not found: $cpuScript"
}

function Get-DshowVideoDevices {
    $ffmpegCommand = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $ffmpegCommand) {
        return @()
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ffmpegCommand.Source
    $psi.Arguments = '-hide_banner -list_devices true -f dshow -i dummy'
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $null = $process.Start()
    $null = $process.WaitForExit(10000)
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }

    $output = $process.StandardError.ReadToEnd() + "`n" + $process.StandardOutput.ReadToEnd()
    $process.Dispose()

    $devices = New-Object System.Collections.Generic.List[string]
    foreach ($line in ($output -split "`r?`n")) {
        if ($line -match '"(.+)"\s+\(video\)') {
            $deviceName = $matches[1].Trim()
            if (-not [string]::IsNullOrWhiteSpace($deviceName) -and -not $devices.Contains($deviceName)) {
                $devices.Add($deviceName)
            }
        }
    }
    return $devices.ToArray()
}

function Test-RealCameraDevice {
    param(
        [string]$ProbeExe,
        [string]$DeviceName,
        [string]$Backend
    )

    if ([string]::IsNullOrWhiteSpace($ProbeExe) -or -not (Test-Path -LiteralPath $ProbeExe)) {
        return $false
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $ProbeExe
    $psi.WorkingDirectory = Split-Path -Parent $ProbeExe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Environment["MEETING_TEST_CAMERA_DEVICE"] = $DeviceName
    if (-not [string]::IsNullOrWhiteSpace($Backend)) {
        $psi.Environment["MEETING_CAMERA_CAPTURE_BACKEND"] = $Backend
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $null = $process.Start()
    $finished = $process.WaitForExit(20000)
    if (-not $finished) {
        $process.Kill()
        $process.WaitForExit()
    }
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $exitCode = $process.ExitCode
    $process.Dispose()

    if ($exitCode -eq 0) {
        return $true
    }

    $diagnostic = ($stdout + "`n" + $stderr).Trim()
    if (-not [string]::IsNullOrWhiteSpace($diagnostic)) {
        Write-Host "[M2] probe failed for '$DeviceName': $diagnostic"
    }
    return $false
}

function Resolve-WorkingRealCameraDevices {
    param(
        [string]$ProbeExe,
        [string]$Backend
    )

    $deviceNames = @(Get-DshowVideoDevices)
    if ($deviceNames.Count -eq 0) {
        return @()
    }
    if ([string]::IsNullOrWhiteSpace($ProbeExe) -or -not (Test-Path -LiteralPath $ProbeExe)) {
        return $deviceNames
    }

    $workingDevices = New-Object System.Collections.Generic.List[string]
    foreach ($deviceName in $deviceNames) {
        if (Test-RealCameraDevice -ProbeExe $ProbeExe -DeviceName $deviceName -Backend $Backend) {
            $workingDevices.Add($deviceName)
        }
    }
    return $workingDevices.ToArray()
}

function Resolve-PreferredCameraDevice {
    param(
        [string[]]$Devices,
        [string[]]$Patterns,
        [string[]]$Exclude = @()
    )

    foreach ($pattern in $Patterns) {
        foreach ($deviceName in $Devices) {
            if ([string]::IsNullOrWhiteSpace($deviceName)) {
                continue
            }
            $excluded = $false
            foreach ($excludedDevice in $Exclude) {
                if ($deviceName -ieq $excludedDevice) {
                    $excluded = $true
                    break
                }
            }
            if (-not $excluded -and $deviceName -ieq $pattern) {
                return $deviceName
            }
        }
    }

    foreach ($pattern in $Patterns) {
        foreach ($deviceName in $Devices) {
            if ([string]::IsNullOrWhiteSpace($deviceName)) {
                continue
            }
            $excluded = $false
            foreach ($excludedDevice in $Exclude) {
                if ($deviceName -ieq $excludedDevice) {
                    $excluded = $true
                    break
                }
            }
            if (-not $excluded -and $deviceName.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                return $deviceName
            }
        }
    }

    return ""
}

if (-not $useSyntheticCamera -and [string]::IsNullOrWhiteSpace($CameraCaptureBackend)) {
    $CameraCaptureBackend = "ffmpeg-process"
}

if (-not $useSyntheticCamera) {
    $probeExe = Join-Path $BuildDir "Debug\test_camera_capture_smoke.exe"
    $workingRealCameraDevices = @(Resolve-WorkingRealCameraDevices -ProbeExe $probeExe -Backend $CameraCaptureBackend)
    if ($useRequireVideo -and $ExpectRealCamera) {
        if ([string]::IsNullOrWhiteSpace($HostCameraDevice)) {
            $HostCameraDevice = Resolve-PreferredCameraDevice -Devices $workingRealCameraDevices -Patterns @("DroidCam Video", "DroidCam")
        }
        if (-not $SingleRealCamera -and [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
            $GuestCameraDevice = Resolve-PreferredCameraDevice -Devices $workingRealCameraDevices -Patterns @("e2eSoft iVCam", "iVCam") -Exclude @($HostCameraDevice)
        }
        if ([string]::IsNullOrWhiteSpace($HostCameraDevice) -or (-not $SingleRealCamera -and [string]::IsNullOrWhiteSpace($GuestCameraDevice))) {
            $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
            throw "DroidCam/iVCam default camera assignment failed, available=$availableSummary"
        }
    }

    $remainingDevices = New-Object System.Collections.Generic.List[string]
    foreach ($deviceName in $workingRealCameraDevices) {
        if (($deviceName -ine $HostCameraDevice) -and ($deviceName -ine $GuestCameraDevice)) {
            $remainingDevices.Add($deviceName)
        }
    }

    if ([string]::IsNullOrWhiteSpace($HostCameraDevice) -and $remainingDevices.Count -gt 0) {
        $HostCameraDevice = $remainingDevices[0]
        $remainingDevices.RemoveAt(0)
    }
    if (-not $SingleRealCamera -and [string]::IsNullOrWhiteSpace($GuestCameraDevice) -and $remainingDevices.Count -gt 0) {
        $GuestCameraDevice = $remainingDevices[0]
        $remainingDevices.RemoveAt(0)
    }

    if ($ExpectRealCamera -and -not $SingleRealCamera) {
        if ([string]::IsNullOrWhiteSpace($HostCameraDevice) -or [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
            $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
            throw "not enough working real camera devices for dual-instance validation, available=$availableSummary"
        }
    } elseif ($ExpectRealCamera -and [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
        throw "no working real camera device available for host, available=$availableSummary"
    }

    if (-not [string]::IsNullOrWhiteSpace($HostCameraDevice) -or -not [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
        Write-Host "[M2] camera assignment backend=$CameraCaptureBackend host='$HostCameraDevice' guest='$GuestCameraDevice'"
    }
}

if (-not $SkipUiSmoke) {
    $uiParams = @{
        ClientExe = $ClientExe
        ServerDir = $ServerDir
        TimeoutSeconds = $UiTimeoutSeconds
    }
    if ($useSyntheticAudio) {
        $uiParams.SyntheticAudio = $true
    }
    if ($SingleRealAudio) {
        $uiParams.SingleRealAudio = $true
    }
    if ($useSyntheticCamera) {
        $uiParams.SyntheticCamera = $true
    }
    if ($SingleRealCamera) {
        $uiParams.SingleRealCamera = $true
    }
    if ($useRequireVideo) {
        $uiParams.RequireVideo = $true
    }
    if ($useRequireAudio) {
        $uiParams.RequireAudio = $true
    }
    if ($UiRequireAvSync) {
        $uiParams.RequireAvSync = $true
    }
    if ($ExpectRealCamera) {
        $uiParams.ExpectRealCamera = $true
    }
    if ($Headless) {
        $uiParams.Headless = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($CameraCaptureBackend)) {
        $uiParams.CameraCaptureBackend = $CameraCaptureBackend
    }
    if (-not [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $uiParams.HostCameraDevice = $HostCameraDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
        $uiParams.GuestCameraDevice = $GuestCameraDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($HostAudioInputDevice)) {
        $uiParams.HostAudioInputDevice = $HostAudioInputDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($HostAudioOutputDevice)) {
        $uiParams.HostAudioOutputDevice = $HostAudioOutputDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestAudioInputDevice)) {
        $uiParams.GuestAudioInputDevice = $GuestAudioInputDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestAudioOutputDevice)) {
        $uiParams.GuestAudioOutputDevice = $GuestAudioOutputDevice
    }

    Write-Host "[M2] >>> dual-ui-smoke"
    & $uiScript @uiParams
    if ($LASTEXITCODE -ne 0) {
        throw "dual-ui-smoke failed, exit_code=$LASTEXITCODE"
    }
    Write-Host "[M2] <<< dual-ui-smoke passed"
}

$cpuParams = @{
    BuildDir = $BuildDir
    MaxCpuPercent = $MaxCpuPercent
    MaxCpuPeakPercent = $MaxCpuPeakPercent
    VideoWidth = $VideoWidth
    VideoHeight = $VideoHeight
    VideoFps = $VideoFps
    VideoBitrateBps = $VideoBitrateBps
    AvSyncMaxSkewMs = $AvSyncMaxSkewMs
    SoakMinutes = $SoakMinutes
}
if ($SingleRealCamera) {
    $cpuParams.SingleRealCamera = $true
}
if ($SingleRealAudio) {
    $cpuParams.SingleRealAudio = $true
}
if ($useSyntheticCamera) {
    $cpuParams.SyntheticCamera = $true
}
if (-not $useSyntheticAudio) {
    $cpuParams.RealAudio = $true
}
if ($useRequireAudio) {
    $cpuParams.RequireAudio = $true
}
if ($useRequireAvSync) {
    $cpuParams.RequireAvSync = $true
}
if (-not [string]::IsNullOrWhiteSpace($CameraCaptureBackend)) {
    $cpuParams.CameraCaptureBackend = $CameraCaptureBackend
}
if (-not [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
    $cpuParams.HostCameraDevice = $HostCameraDevice
}
if (-not [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
    $cpuParams.GuestCameraDevice = $GuestCameraDevice
}
if (-not [string]::IsNullOrWhiteSpace($HostAudioInputDevice)) {
    $cpuParams.HostAudioInputDevice = $HostAudioInputDevice
}
if (-not [string]::IsNullOrWhiteSpace($HostAudioOutputDevice)) {
    $cpuParams.HostAudioOutputDevice = $HostAudioOutputDevice
}
if (-not [string]::IsNullOrWhiteSpace($GuestAudioInputDevice)) {
    $cpuParams.GuestAudioInputDevice = $GuestAudioInputDevice
}
if (-not [string]::IsNullOrWhiteSpace($GuestAudioOutputDevice)) {
    $cpuParams.GuestAudioOutputDevice = $GuestAudioOutputDevice
}

Write-Host "[M2] >>> process-smoke-cpu-avsync"
& $cpuScript @cpuParams
if ($LASTEXITCODE -ne 0) {
    throw "process-smoke-cpu-avsync failed, exit_code=$LASTEXITCODE"
}
Write-Host "[M2] <<< process-smoke-cpu-avsync passed"

Write-Host "[M2] dual-instance validation passed"
Write-Host "[M2] mode synthetic_audio=$useSyntheticAudio synthetic_camera=$useSyntheticCamera single_real_camera=$($SingleRealCamera.IsPresent) single_real_audio=$($SingleRealAudio.IsPresent) require_video=$useRequireVideo require_audio=$useRequireAudio require_avsync=$useRequireAvSync"
