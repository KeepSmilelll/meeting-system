param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerDir = "D:\meeting\meeting-server\signaling",
    [string]$SfuExe = "",
    [switch]$SyntheticAudio,
    [switch]$RequireAudio,
    [switch]$SingleRealAudio,
    [switch]$SyntheticCamera,
    [switch]$SingleRealCamera,
    [switch]$RequireVideo,
    [switch]$RequireAvSync,
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
    [ValidateSet("", "all", "relay-only")]
    [string]$IcePolicy = "",
    [string]$TurnServers = "",
    [string]$TurnSecret = "",
    [string]$TurnRealm = "meeting.local",
    [string]$TurnCredentialTtl = "1h",
    [ValidateRange(0, 120000)]
    [int]$SoakMs = 0,
    [ValidateRange(0, 120000)]
    [int]$GuestMediaSoakMs = 0,
    [int]$TimeoutSeconds = 45
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($SingleRealAudio -and $SyntheticAudio) {
    throw "SingleRealAudio cannot be used together with SyntheticAudio"
}
if ($SingleRealCamera -and $SyntheticCamera) {
    throw "SingleRealCamera cannot be used together with SyntheticCamera"
}
if ($IcePolicy -eq "relay-only" -and [string]::IsNullOrWhiteSpace($TurnServers)) {
    throw "relay-only validation requires -TurnServers, for example: turn:127.0.0.1:3478?transport=udp"
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

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return $listener.LocalEndpoint.Port
    } finally {
        $listener.Stop()
    }
}

function Get-FreeUdpPort {
    $socket = [System.Net.Sockets.UdpClient]::new([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 0))
    try {
        return $socket.Client.LocalEndPoint.Port
    } finally {
        $socket.Dispose()
    }
}

function Resolve-SfuExecutable {
    param([string]$Configured)

    $candidates = @(
        $Configured,
        "D:\meeting\meeting-server\sfu\build_sfu_check\Debug\meeting_sfu.exe",
        "D:\meeting\meeting-server\sfu\build\Debug\meeting_sfu.exe",
        "D:\meeting\meeting-server\sfu\build_sfu_check\Release\meeting_sfu.exe",
        "D:\meeting\meeting-server\sfu\build\Release\meeting_sfu.exe"
    )

    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }
    return ""
}

function Start-ManagedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [hashtable]$Environment,
        [string]$StdOutPath,
        [string]$StdErrPath
    )

    $startProcessCommand = Get-Command Start-Process
    if ($startProcessCommand.Parameters.ContainsKey("Environment")) {
        $startArgs = @{
            FilePath = $FilePath
            ArgumentList = $Arguments
            WorkingDirectory = $WorkingDirectory
            Environment = $Environment
            PassThru = $true
        }
        if (-not [string]::IsNullOrWhiteSpace($StdOutPath)) {
            $startArgs.RedirectStandardOutput = $StdOutPath
        }
        if (-not [string]::IsNullOrWhiteSpace($StdErrPath)) {
            $startArgs.RedirectStandardError = $StdErrPath
        }
        $process = Start-Process @startArgs
    } else {
        $psi = [System.Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $FilePath
        $quotedArgs = foreach ($arg in $Arguments) {
            '"' + ($arg -replace '"', '\"') + '"'
        }
        $psi.Arguments = ($quotedArgs -join ' ')
        $psi.WorkingDirectory = $WorkingDirectory
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true

        foreach ($entry in $Environment.GetEnumerator()) {
            $psi.Environment[$entry.Key] = [string]$entry.Value
        }

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $psi
        $process.Start() | Out-Null
    }

    return @{
        Process = $process
        StdOutPath = $StdOutPath
        StdErrPath = $StdErrPath
    }
}

function Stop-ManagedProcess {
    param($Managed)
    if ($null -eq $Managed) {
        return
    }

    $process = $Managed.Process
    if ($process -and -not $process.HasExited) {
        # Process.Kill(bool) is unavailable on Windows PowerShell (.NET Framework).
        # Prefer killing the full tree when supported, otherwise fall back to Kill().
        $killWithTree = $process.GetType().GetMethod(
            "Kill",
            [System.Reflection.BindingFlags] "Public, Instance",
            $null,
            [Type[]] @([bool]),
            $null
        )
        if ($killWithTree -ne $null) {
            [void]$killWithTree.Invoke($process, @($true))
        } else {
            $process.Kill()
        }
        $process.WaitForExit()
    }

    if ($process) {
        $supportsDirectRedirect = (Get-Command Start-Process).Parameters.ContainsKey("Environment")
        if ((-not $supportsDirectRedirect) -and $process.HasExited) {
            if ($Managed.StdOutPath) {
                [System.IO.File]::WriteAllText($Managed.StdOutPath, $process.StandardOutput.ReadToEnd())
            }
            if ($Managed.StdErrPath) {
                [System.IO.File]::WriteAllText($Managed.StdErrPath, $process.StandardError.ReadToEnd())
            }
        }
        $process.Dispose()
    }
}

function Wait-ForTcpListening {
    param(
        [string]$TargetHost,
        [int]$Port,
        [int]$TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $async = $client.BeginConnect($TargetHost, $Port, $null, $null)
            if ($async.AsyncWaitHandle.WaitOne(200)) {
                $client.EndConnect($async)
                return $true
            }
        } catch {
        } finally {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Wait-ForProcessExit {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds
    )

    $completed = $Process.WaitForExit($TimeoutSeconds * 1000)
    if ($completed) {
        $Process.WaitForExit()
    }
    return $completed
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
        Write-Host "[dual-ui-smoke] probe failed for '$DeviceName': $diagnostic"
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
        foreach ($device in $Devices) {
            if ([string]::IsNullOrWhiteSpace($device)) {
                continue
            }
            $excluded = $false
            foreach ($excludedDevice in $Exclude) {
                if ($device -ieq $excludedDevice) {
                    $excluded = $true
                    break
                }
            }
            if ($excluded) {
                continue
            }
            if ($device -ieq $pattern) {
                return $device
            }
        }
    }

    foreach ($pattern in $Patterns) {
        foreach ($device in $Devices) {
            if ([string]::IsNullOrWhiteSpace($device)) {
                continue
            }
            $excluded = $false
            foreach ($excludedDevice in $Exclude) {
                if ($device -ieq $excludedDevice) {
                    $excluded = $true
                    break
                }
            }
            if (-not $excluded -and $device.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                return $device
            }
        }
    }

    return ""
}

if (-not (Test-Path $ClientExe)) {
    throw "meeting_client.exe not found: $ClientExe"
}

$resolvedCameraCaptureBackend = $CameraCaptureBackend
if (-not $SyntheticCamera -and [string]::IsNullOrWhiteSpace($resolvedCameraCaptureBackend)) {
    $resolvedCameraCaptureBackend = "ffmpeg-process"
}

$probeExe = Join-Path (Split-Path -Parent $ClientExe) "test_camera_capture_smoke.exe"
if (-not $SyntheticCamera) {
    $workingRealCameraDevices = @(Resolve-WorkingRealCameraDevices -ProbeExe $probeExe -Backend $resolvedCameraCaptureBackend)
    if ($RequireVideo -and $ExpectRealCamera) {
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
            throw "not enough working real camera devices for dual-instance run, available=$availableSummary"
        }
    } elseif ($ExpectRealCamera -and [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
        throw "no working real camera device available for host, available=$availableSummary"
    }

    if (-not [string]::IsNullOrWhiteSpace($HostCameraDevice) -or -not [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
        Write-Host "[dual-ui-smoke] camera assignment backend=$resolvedCameraCaptureBackend host='$HostCameraDevice' guest='$GuestCameraDevice'"
    }
}

$port = Get-FreeTcpPort
$tempRoot = Join-Path $env:TEMP ("meeting-ui-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$meetingIdPath = Join-Path $tempRoot "meeting_id.txt"
$hostResultPath = Join-Path $tempRoot "host.result.txt"
$guestResultPath = Join-Path $tempRoot "guest.result.txt"
$serverStdOut = Join-Path $tempRoot "server.stdout.txt"
$serverStdErr = Join-Path $tempRoot "server.stderr.txt"
$sfuStdOut = Join-Path $tempRoot "sfu.stdout.txt"
$sfuStdErr = Join-Path $tempRoot "sfu.stderr.txt"
$hostStdOut = Join-Path $tempRoot "host.stdout.txt"
$hostStdErr = Join-Path $tempRoot "host.stderr.txt"
$guestStdOut = Join-Path $tempRoot "guest.stdout.txt"
$guestStdErr = Join-Path $tempRoot "guest.stderr.txt"

$resolvedSfuExe = Resolve-SfuExecutable -Configured $SfuExe
if ([string]::IsNullOrWhiteSpace($resolvedSfuExe)) {
    throw "meeting_sfu.exe not found; build SFU first or pass -SfuExe"
}
$sfuRpcPort = Get-FreeTcpPort
$sfuMediaPort = Get-FreeUdpPort
if ($sfuRpcPort -le 0 -or $sfuMediaPort -le 0) {
    throw "failed to reserve SFU ports"
}

$serverEnv = @{}
$serverEnv["SIGNALING_LISTEN_ADDR"] = "127.0.0.1:$port"
$serverEnv["SIGNALING_ENABLE_REDIS"] = "false"
$serverEnv["SIGNALING_MYSQL_DSN"] = ""
$serverEnv["SIGNALING_DEFAULT_SFU"] = "127.0.0.1:$sfuMediaPort"
$serverEnv["SIGNALING_SFU_RPC_ADDR"] = "127.0.0.1:$sfuRpcPort"
if (-not [string]::IsNullOrWhiteSpace($TurnServers)) {
    $serverEnv["SIGNALING_TURN_SERVERS"] = $TurnServers
}
if (-not [string]::IsNullOrWhiteSpace($TurnSecret)) {
    $serverEnv["SIGNALING_TURN_SECRET"] = $TurnSecret
}
if (-not [string]::IsNullOrWhiteSpace($TurnRealm)) {
    $serverEnv["SIGNALING_TURN_REALM"] = $TurnRealm
}
if (-not [string]::IsNullOrWhiteSpace($TurnCredentialTtl)) {
    $serverEnv["SIGNALING_TURN_CRED_TTL"] = $TurnCredentialTtl
}

$clientBaseEnv = @{}
$clientBaseEnv["MEETING_RUNTIME_SMOKE"] = "1"
$clientBaseEnv["MEETING_SERVER_HOST"] = "127.0.0.1"
$clientBaseEnv["MEETING_SERVER_PORT"] = "$port"
$clientBaseEnv["MEETING_SMOKE_MEETING_ID_PATH"] = $meetingIdPath
$clientBaseEnv["MEETING_SMOKE_TIMEOUT_MS"] = "$($TimeoutSeconds * 1000)"
if ($SoakMs -gt 0) {
    $clientBaseEnv["MEETING_SMOKE_SOAK_MS"] = "$SoakMs"
}
if ($GuestMediaSoakMs -gt 0) {
    $clientBaseEnv["MEETING_SMOKE_GUEST_MEDIA_SOAK_MS"] = "$GuestMediaSoakMs"
}
if ($RequireVideo) {
    $clientBaseEnv["MEETING_SMOKE_REQUIRE_VIDEO"] = "1"
}
if ($RequireAudio) {
    $clientBaseEnv["MEETING_SMOKE_REQUIRE_AUDIO"] = "1"
}
if ($RequireAvSync) {
    $clientBaseEnv["MEETING_SMOKE_REQUIRE_AVSYNC"] = "1"
    $clientBaseEnv["MEETING_VIDEO_AUDIO_DRIVEN_MAX_DELAY_MS"] = "20"
    $clientBaseEnv["MEETING_VIDEO_RENDER_QUEUE_DEPTH"] = "4"
    $clientBaseEnv["MEETING_VIDEO_MAX_FRAMES_PER_DRAIN"] = "4"
    $clientBaseEnv["MEETING_VIDEO_MAX_CADENCE_MS"] = "40"
    if ([string]::IsNullOrWhiteSpace($env:MEETING_VIDEO_FPS)) {
        $clientBaseEnv["MEETING_VIDEO_FPS"] = "24"
    }
}
if ($ExpectRealCamera) {
    $clientBaseEnv["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = "real-device"
}
if ($Headless) {
    $clientBaseEnv["QT_QPA_PLATFORM"] = "offscreen"
}
if (-not [string]::IsNullOrWhiteSpace($IcePolicy)) {
    $clientBaseEnv["MEETING_ICE_POLICY"] = $IcePolicy
    Write-Host "[dual-ui-smoke] forcing MEETING_ICE_POLICY=$IcePolicy"
}
if (-not [string]::IsNullOrWhiteSpace($resolvedCameraCaptureBackend)) {
    $clientBaseEnv["MEETING_CAMERA_CAPTURE_BACKEND"] = $resolvedCameraCaptureBackend
    Write-Host "[dual-ui-smoke] forcing MEETING_CAMERA_CAPTURE_BACKEND=$resolvedCameraCaptureBackend"
}

$serverProcess = $null
$sfuProcess = $null
$hostProcess = $null
$guestProcess = $null

try {
    $sfuEnv = @{}
    $sfuEnv["SFU_ADVERTISED_HOST"] = "127.0.0.1"
    $sfuEnv["SFU_RPC_LISTEN_PORT"] = "$sfuRpcPort"
    $sfuEnv["SFU_MEDIA_LISTEN_PORT"] = "$sfuMediaPort"
    $sfuEnv["SFU_NODE_ID"] = "dual-ui-smoke-sfu"
    $sfuProcess = Start-ManagedProcess -FilePath $resolvedSfuExe -Arguments @() -WorkingDirectory (Split-Path -Parent $resolvedSfuExe) -Environment $sfuEnv -StdOutPath $sfuStdOut -StdErrPath $sfuStdErr
    if (-not (Wait-ForTcpListening -TargetHost "127.0.0.1" -Port $sfuRpcPort -TimeoutSeconds 20)) {
        throw "SFU RPC did not start listening`n$(Get-Content $sfuStdErr -Raw)"
    }
    Write-Host "[dual-ui-smoke] internal SFU rpc=127.0.0.1:$sfuRpcPort media=127.0.0.1:$sfuMediaPort"

    $serverProcess = Start-ManagedProcess -FilePath "go" -Arguments @("run", ".") -WorkingDirectory $ServerDir -Environment $serverEnv -StdOutPath $serverStdOut -StdErrPath $serverStdErr
    if (-not (Wait-ForTcpListening -TargetHost "127.0.0.1" -Port $port -TimeoutSeconds 20)) {
        throw "signaling server did not start listening`n$(Get-Content $serverStdErr -Raw)"
    }

    $hostEnv = $clientBaseEnv.Clone()
    $hostEnv["MEETING_SMOKE_ROLE"] = "host"
    $hostEnv["MEETING_SMOKE_USERNAME"] = "demo"
    $hostEnv["MEETING_SMOKE_PASSWORD"] = "demo"
    $hostEnv["MEETING_DB_PATH"] = (Join-Path $tempRoot "host.sqlite")
    $hostEnv["MEETING_SMOKE_RESULT_PATH"] = $hostResultPath
    $hostEnv["MEETING_SMOKE_PEER_RESULT_PATH"] = $guestResultPath
    if ($SyntheticCamera) {
        $hostEnv["MEETING_SYNTHETIC_CAMERA"] = "1"
        $hostEnv["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = "synthetic-fallback"
    } else {
        $hostEnv.Remove("MEETING_SYNTHETIC_CAMERA")
        if ($ExpectRealCamera) {
            $hostEnv["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = "real-device"
        }
    }
    if ($SyntheticAudio) {
        $hostEnv["MEETING_SYNTHETIC_AUDIO"] = "1"
    } else {
        $hostEnv.Remove("MEETING_SYNTHETIC_AUDIO")
    }
    if (-not [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $hostEnv["MEETING_CAMERA_DEVICE_NAME"] = $HostCameraDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($HostAudioInputDevice)) {
        $hostEnv["MEETING_AUDIO_INPUT_DEVICE_NAME"] = $HostAudioInputDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($HostAudioOutputDevice)) {
        $hostEnv["MEETING_AUDIO_OUTPUT_DEVICE_NAME"] = $HostAudioOutputDevice
    }

    $guestEnv = $clientBaseEnv.Clone()
    $guestEnv["MEETING_SMOKE_ROLE"] = "guest"
    $guestEnv["MEETING_SMOKE_USERNAME"] = "alice"
    $guestEnv["MEETING_SMOKE_PASSWORD"] = "alice"
    $guestEnv["MEETING_DB_PATH"] = (Join-Path $tempRoot "guest.sqlite")
    $guestEnv["MEETING_SMOKE_RESULT_PATH"] = $guestResultPath
    $guestEnv["MEETING_SMOKE_PEER_RESULT_PATH"] = $hostResultPath
    if ($SyntheticCamera -or $SingleRealCamera) {
        $guestEnv["MEETING_SYNTHETIC_CAMERA"] = "1"
        $guestEnv["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = "synthetic-fallback"
    } else {
        $guestEnv.Remove("MEETING_SYNTHETIC_CAMERA")
        if ($ExpectRealCamera) {
            $guestEnv["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = "real-device"
        }
    }
    if ($SyntheticAudio -or $SingleRealAudio) {
        $guestEnv["MEETING_SYNTHETIC_AUDIO"] = "1"
    } else {
        $guestEnv.Remove("MEETING_SYNTHETIC_AUDIO")
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestCameraDevice)) {
        $guestEnv["MEETING_CAMERA_DEVICE_NAME"] = $GuestCameraDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestAudioInputDevice)) {
        $guestEnv["MEETING_AUDIO_INPUT_DEVICE_NAME"] = $GuestAudioInputDevice
    }
    if (-not [string]::IsNullOrWhiteSpace($GuestAudioOutputDevice)) {
        $guestEnv["MEETING_AUDIO_OUTPUT_DEVICE_NAME"] = $GuestAudioOutputDevice
    }

    $clientDir = Split-Path -Parent $ClientExe
    $hostProcess = Start-ManagedProcess -FilePath $ClientExe -Arguments @() -WorkingDirectory $clientDir -Environment $hostEnv -StdOutPath $hostStdOut -StdErrPath $hostStdErr
    $guestProcess = Start-ManagedProcess -FilePath $ClientExe -Arguments @() -WorkingDirectory $clientDir -Environment $guestEnv -StdOutPath $guestStdOut -StdErrPath $guestStdErr

    $hostExited = Wait-ForProcessExit -Process $hostProcess.Process -TimeoutSeconds $TimeoutSeconds
    $guestExited = Wait-ForProcessExit -Process $guestProcess.Process -TimeoutSeconds $TimeoutSeconds
    if (-not $hostExited -or -not $guestExited) {
        throw "client timeout"
    }

    $hostResult = if (Test-Path $hostResultPath) { Get-Content $hostResultPath -Raw } else { "<missing>" }
    $guestResult = if (Test-Path $guestResultPath) { Get-Content $guestResultPath -Raw } else { "<missing>" }

    $hostOk = $hostProcess.Process.ExitCode -eq 0
    $guestOk = $guestProcess.Process.ExitCode -eq 0
    if (-not $hostOk -or -not $guestOk) {
        throw @"
host exit=$($hostProcess.Process.ExitCode)
$hostResult

guest exit=$($guestProcess.Process.ExitCode)
$guestResult
"@
    }

    Write-Output "dual ui smoke passed"
    Write-Output "temp root: $tempRoot"
    exit 0
} catch {
    Write-Error $_
    Write-Error ("temp root: " + $tempRoot)
    if (Test-Path $hostResultPath) {
        Write-Error ("host result:`n" + (Get-Content $hostResultPath -Raw))
    }
    if (Test-Path $guestResultPath) {
        Write-Error ("guest result:`n" + (Get-Content $guestResultPath -Raw))
    }
    if (Test-Path $serverStdErr) {
        Write-Error ("server stderr:`n" + (Get-Content $serverStdErr -Raw))
    }
    if (Test-Path $sfuStdErr) {
        Write-Error ("sfu stderr:`n" + (Get-Content $sfuStdErr -Raw))
    }
    exit 1
} finally {
    Stop-ManagedProcess $hostProcess
    Stop-ManagedProcess $guestProcess
    Stop-ManagedProcess $serverProcess
    Stop-ManagedProcess $sfuProcess
}
