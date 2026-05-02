param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerDir = "D:\meeting\meeting-server\signaling",
    [string]$SfuExe = "",
    [string]$ExternalServerHost = "",
    [ValidateRange(1, 65535)]
    [int]$ExternalServerPort = 8443,
    [ValidateSet("", "all", "relay-only")]
    [string]$IcePolicy = "",
    [bool]$SyntheticAudio = $true,
    [switch]$RequireAudio,
    [bool]$SyntheticCamera = $false,
    [bool]$RequireVideo = $true,
    [switch]$RequireChat,
    [switch]$RequireMediaStateSync,
    [switch]$RequireCameraToggleRecovery,
    [bool]$ExpectRealCamera = $true,
    [string]$HostCameraDevice = "",
    [string]$SubscriberACameraDevice = "",
    [string]$SubscriberBCameraDevice = "",
    [string]$HostAudioInputDevice = "",
    [string]$HostAudioOutputDevice = "",
    [string]$SubscriberAAudioInputDevice = "",
    [string]$SubscriberAAudioOutputDevice = "",
    [string]$SubscriberBAudioInputDevice = "",
    [string]$SubscriberBAudioOutputDevice = "",
    [string]$HostAudioDevice = "",
    [string]$SubscriberAAudioDevice = "",
    [string]$SubscriberBAudioDevice = "",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$HostAudioSource = "inherit",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$SubscriberAAudioSource = "inherit",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$SubscriberBAudioSource = "inherit",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$HostCameraSource = "inherit",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$SubscriberACameraSource = "inherit",
    [ValidateSet("inherit", "real", "synthetic")]
    [string]$SubscriberBCameraSource = "inherit",
    [bool]$HostPublishAudio = $true,
    [bool]$HostPublishVideo = $true,
    [bool]$SubscriberAPublishAudio = $false,
    [bool]$SubscriberAPublishVideo = $false,
    [bool]$SubscriberBPublishAudio = $false,
    [bool]$SubscriberBPublishVideo = $false,
    [string]$SubscriberAObservedRemoteVideoUserId = "demo",
    [string]$SubscriberBObservedRemoteVideoUserId = "demo",
    [ValidateSet("", "auto", "qt", "dshow", "ffmpeg-process", "ffmpeg")]
    [string]$CameraCaptureBackend = "",
    [switch]$Headless,
    [ValidateRange(0, 120000)]
    [int]$SoakMs = 0,
    [ValidateRange(0, 120000)]
    [int]$SubscriberMediaSoakMs = 0,
    [ValidateRange(0, 30000)]
    [int]$SubscriberBLaunchDelayMs = 1000,
    [int]$TimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($SyntheticCamera -and $ExpectRealCamera) {
    throw "SyntheticCamera cannot be used together with ExpectRealCamera"
}
if ([string]::IsNullOrWhiteSpace($HostAudioInputDevice) -and -not [string]::IsNullOrWhiteSpace($HostAudioDevice)) {
    $HostAudioInputDevice = $HostAudioDevice
}
if ([string]::IsNullOrWhiteSpace($HostAudioOutputDevice) -and -not [string]::IsNullOrWhiteSpace($HostAudioDevice)) {
    $HostAudioOutputDevice = $HostAudioDevice
}
if ([string]::IsNullOrWhiteSpace($SubscriberAAudioInputDevice) -and -not [string]::IsNullOrWhiteSpace($SubscriberAAudioDevice)) {
    $SubscriberAAudioInputDevice = $SubscriberAAudioDevice
}
if ([string]::IsNullOrWhiteSpace($SubscriberAAudioOutputDevice) -and -not [string]::IsNullOrWhiteSpace($SubscriberAAudioDevice)) {
    $SubscriberAAudioOutputDevice = $SubscriberAAudioDevice
}
if ([string]::IsNullOrWhiteSpace($SubscriberBAudioInputDevice) -and -not [string]::IsNullOrWhiteSpace($SubscriberBAudioDevice)) {
    $SubscriberBAudioInputDevice = $SubscriberBAudioDevice
}
if ([string]::IsNullOrWhiteSpace($SubscriberBAudioOutputDevice) -and -not [string]::IsNullOrWhiteSpace($SubscriberBAudioDevice)) {
    $SubscriberBAudioOutputDevice = $SubscriberBAudioDevice
}

$singleRealAudioInputDevice = "Microphone (EDIFIER Halo Soundbar)"
$singleRealAudioOutputDevice = "扬声器 (EDIFIER Halo Soundbar)"

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
        Write-Host "[triple-ui-smoke] probe failed for '$DeviceName': $diagnostic"
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
            if ($Exclude -contains $device) {
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
            if ($Exclude -contains $device) {
                continue
            }
            if ($device.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                return $device
            }
        }
    }

    return ""
}

function Resolve-MediaSourceMode {
    param(
        [string]$Override,
        [bool]$GlobalSynthetic
    )

    switch ($Override) {
        "real" { return "real" }
        "synthetic" { return "synthetic" }
        default {
            if ($GlobalSynthetic) {
                return "synthetic"
            }
            return "real"
        }
    }
}

function Get-PublishedMeetingId {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }

    return [string]::Join("", (Get-Content -LiteralPath $Path)).Trim()
}

if (-not (Test-Path -LiteralPath $ClientExe)) {
    throw "meeting_client.exe not found: $ClientExe"
}

$resolvedCameraCaptureBackend = $CameraCaptureBackend
if (-not $SyntheticCamera -and [string]::IsNullOrWhiteSpace($resolvedCameraCaptureBackend)) {
    $resolvedCameraCaptureBackend = "ffmpeg-process"
}

$hostAudioSourceMode = Resolve-MediaSourceMode -Override $HostAudioSource -GlobalSynthetic $SyntheticAudio
$subscriberAAudioSourceMode = Resolve-MediaSourceMode -Override $SubscriberAAudioSource -GlobalSynthetic $SyntheticAudio
$subscriberBAudioSourceMode = Resolve-MediaSourceMode -Override $SubscriberBAudioSource -GlobalSynthetic $SyntheticAudio
$hostCameraSourceMode = Resolve-MediaSourceMode -Override $HostCameraSource -GlobalSynthetic $SyntheticCamera
$subscriberACameraSourceMode = Resolve-MediaSourceMode -Override $SubscriberACameraSource -GlobalSynthetic $SyntheticCamera
$subscriberBCameraSourceMode = Resolve-MediaSourceMode -Override $SubscriberBCameraSource -GlobalSynthetic $SyntheticCamera

$realAudioPublishers = @()
if ($HostPublishAudio -and $hostAudioSourceMode -eq "real") {
    $realAudioPublishers += "host"
}
if ($SubscriberAPublishAudio -and $subscriberAAudioSourceMode -eq "real") {
    $realAudioPublishers += "subscriber_a"
}
if ($SubscriberBPublishAudio -and $subscriberBAudioSourceMode -eq "real") {
    $realAudioPublishers += "subscriber_b"
}

if ($realAudioPublishers.Count -eq 1) {
    switch ($realAudioPublishers[0]) {
        "host" {
            if ([string]::IsNullOrWhiteSpace($HostAudioInputDevice)) {
                $HostAudioInputDevice = $singleRealAudioInputDevice
            }
        }
        "subscriber_a" {
            if ([string]::IsNullOrWhiteSpace($SubscriberAAudioInputDevice)) {
                $SubscriberAAudioInputDevice = $singleRealAudioInputDevice
            }
        }
        "subscriber_b" {
            if ([string]::IsNullOrWhiteSpace($SubscriberBAudioInputDevice)) {
                $SubscriberBAudioInputDevice = $singleRealAudioInputDevice
            }
        }
    }
} elseif ($realAudioPublishers.Count -gt 1) {
    $missingRealAudioDevices = @()
    if ($realAudioPublishers -contains "host" -and [string]::IsNullOrWhiteSpace($HostAudioInputDevice)) {
        $missingRealAudioDevices += "host"
    }
    if ($realAudioPublishers -contains "subscriber_a" -and [string]::IsNullOrWhiteSpace($SubscriberAAudioInputDevice)) {
        $missingRealAudioDevices += "subscriber_a"
    }
    if ($realAudioPublishers -contains "subscriber_b" -and [string]::IsNullOrWhiteSpace($SubscriberBAudioInputDevice)) {
        $missingRealAudioDevices += "subscriber_b"
    }
    if ($missingRealAudioDevices.Count -gt 0) {
        throw "multiple real audio publishers require explicit distinct input devices; missing input for $($missingRealAudioDevices -join ', '). This machine defaults to single real input '$singleRealAudioInputDevice'."
    }
}

$realAudioPublishingDevices = @(
    if ($HostPublishAudio -and $hostAudioSourceMode -eq "real") { $HostAudioInputDevice }
    if ($SubscriberAPublishAudio -and $subscriberAAudioSourceMode -eq "real") { $SubscriberAAudioInputDevice }
    if ($SubscriberBPublishAudio -and $subscriberBAudioSourceMode -eq "real") { $SubscriberBAudioInputDevice }
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
$duplicateRealAudioPublishingDevices = @($realAudioPublishingDevices | Group-Object | Where-Object { $_.Count -gt 1 } | Select-Object -ExpandProperty Name)
if ($duplicateRealAudioPublishingDevices.Count -gt 0) {
    throw "duplicate real audio input assignment detected: $($duplicateRealAudioPublishingDevices -join ', '). Use one real audio publisher on this machine or pass distinct input devices."
}

if (($RequireAudio -or $realAudioPublishers.Count -gt 0) -and [string]::IsNullOrWhiteSpace($HostAudioOutputDevice)) {
    $HostAudioOutputDevice = $singleRealAudioOutputDevice
}
if ($RequireAudio -and [string]::IsNullOrWhiteSpace($SubscriberAAudioOutputDevice)) {
    $SubscriberAAudioOutputDevice = $singleRealAudioOutputDevice
}
if ($RequireAudio -and [string]::IsNullOrWhiteSpace($SubscriberBAudioOutputDevice)) {
    $SubscriberBAudioOutputDevice = $singleRealAudioOutputDevice
}

$probeExe = Join-Path (Split-Path -Parent $ClientExe) "test_camera_capture_smoke.exe"
if (($HostPublishVideo -and $hostCameraSourceMode -eq "real") -or
    ($SubscriberAPublishVideo -and $subscriberACameraSourceMode -eq "real") -or
    ($SubscriberBPublishVideo -and $subscriberBCameraSourceMode -eq "real")) {
    $workingRealCameraDevices = @(Resolve-WorkingRealCameraDevices -ProbeExe $probeExe -Backend $resolvedCameraCaptureBackend)
    if ($HostPublishVideo -and $hostCameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $HostCameraDevice = Resolve-PreferredCameraDevice -Devices $workingRealCameraDevices -Patterns @("DroidCam Video", "DroidCam")
    }
    if ($SubscriberAPublishVideo -and $subscriberACameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($SubscriberACameraDevice)) {
        $SubscriberACameraDevice = Resolve-PreferredCameraDevice -Devices $workingRealCameraDevices -Patterns @("e2eSoft iVCam", "iVCam") -Exclude @($HostCameraDevice)
    }
    if ($SubscriberBPublishVideo -and $subscriberBCameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($SubscriberBCameraDevice)) {
        $SubscriberBCameraDevice = Resolve-PreferredCameraDevice -Devices $workingRealCameraDevices -Patterns @() -Exclude @($HostCameraDevice, $SubscriberACameraDevice)
        if ([string]::IsNullOrWhiteSpace($SubscriberBCameraDevice)) {
            $remainingRealDevices = @($workingRealCameraDevices | Where-Object {
                $_ -and $_ -ine $HostCameraDevice -and $_ -ine $SubscriberACameraDevice
            })
            if ($remainingRealDevices.Count -gt 0) {
                $SubscriberBCameraDevice = $remainingRealDevices[0]
            }
        }
    }

    if ($ExpectRealCamera -and $HostPublishVideo -and $hostCameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($HostCameraDevice)) {
        $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
        throw "host real camera assignment failed, available=$availableSummary"
    }
    if ($ExpectRealCamera -and $SubscriberAPublishVideo -and $subscriberACameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($SubscriberACameraDevice)) {
        $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
        throw "subscriber_a real camera assignment failed, available=$availableSummary"
    }
    if ($ExpectRealCamera -and $SubscriberBPublishVideo -and $subscriberBCameraSourceMode -eq "real" -and [string]::IsNullOrWhiteSpace($SubscriberBCameraDevice)) {
        $availableSummary = if ($workingRealCameraDevices.Count -gt 0) { $workingRealCameraDevices -join ", " } else { "<none>" }
        throw "subscriber_b real camera assignment failed, available=$availableSummary"
    }

    $realPublishingDevices = @(
        $HostCameraDevice
        $SubscriberACameraDevice
        $SubscriberBCameraDevice
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    $duplicateRealPublishingDevices = @($realPublishingDevices | Group-Object | Where-Object { $_.Count -gt 1 } | Select-Object -ExpandProperty Name)
    if ($duplicateRealPublishingDevices.Count -gt 0) {
        throw "duplicate real camera assignment detected: $($duplicateRealPublishingDevices -join ', ')"
    }

    Write-Host "[triple-ui-smoke] real camera assignment backend=$resolvedCameraCaptureBackend host='$HostCameraDevice' subscriber_a='$SubscriberACameraDevice' subscriber_b='$SubscriberBCameraDevice'"
}

$usingExternalServer = -not [string]::IsNullOrWhiteSpace($ExternalServerHost)
$port = if ($usingExternalServer) { $ExternalServerPort } else { Get-FreeTcpPort }
$tempRoot = Join-Path $env:TEMP ("meeting-triple-ui-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$meetingIdPath = Join-Path $tempRoot "meeting_id.txt"
$hostResultPath = Join-Path $tempRoot "host.result.txt"
$subscriberAResultPath = Join-Path $tempRoot "subscriber_a.result.txt"
$subscriberBResultPath = Join-Path $tempRoot "subscriber_b.result.txt"
$serverStdOut = Join-Path $tempRoot "server.stdout.txt"
$serverStdErr = Join-Path $tempRoot "server.stderr.txt"
$sfuStdOut = Join-Path $tempRoot "sfu.stdout.txt"
$sfuStdErr = Join-Path $tempRoot "sfu.stderr.txt"
$hostStdOut = Join-Path $tempRoot "host.stdout.txt"
$hostStdErr = Join-Path $tempRoot "host.stderr.txt"
$subscriberAStdOut = Join-Path $tempRoot "subscriber_a.stdout.txt"
$subscriberAStdErr = Join-Path $tempRoot "subscriber_a.stderr.txt"
$subscriberBStdOut = Join-Path $tempRoot "subscriber_b.stdout.txt"
$subscriberBStdErr = Join-Path $tempRoot "subscriber_b.stderr.txt"

$resolvedSfuExe = ""
$sfuRpcPort = 0
$sfuMediaPort = 0
$serverEnv = @{}
if (-not $usingExternalServer) {
    $resolvedSfuExe = Resolve-SfuExecutable -Configured $SfuExe
    if ([string]::IsNullOrWhiteSpace($resolvedSfuExe)) {
        throw "meeting_sfu.exe not found; build SFU first or pass -SfuExe"
    }
    $sfuRpcPort = Get-FreeTcpPort
    $sfuMediaPort = Get-FreeUdpPort
    if ($sfuRpcPort -le 0 -or $sfuMediaPort -le 0) {
        throw "failed to reserve SFU ports"
    }

    $serverEnv = @{
        SIGNALING_LISTEN_ADDR = "127.0.0.1:$port"
        SIGNALING_ENABLE_REDIS = "false"
        SIGNALING_MYSQL_DSN = ""
        SIGNALING_DEFAULT_SFU = "127.0.0.1:$sfuMediaPort"
        SIGNALING_SFU_RPC_ADDR = "127.0.0.1:$sfuRpcPort"
    }
}

$resolvedClientServerHost = if ($usingExternalServer) { $ExternalServerHost.Trim() } else { "127.0.0.1" }
$clientBaseEnv = @{
    MEETING_RUNTIME_SMOKE = "1"
    MEETING_SERVER_HOST = $resolvedClientServerHost
    MEETING_SERVER_PORT = "$port"
    MEETING_SMOKE_MEETING_ID_PATH = $meetingIdPath
    MEETING_SMOKE_TIMEOUT_MS = "$($TimeoutSeconds * 1000)"
    MEETING_SMOKE_MEETING_CAPACITY = "3"
}
if (-not [string]::IsNullOrWhiteSpace($IcePolicy)) {
    $clientBaseEnv["MEETING_ICE_POLICY"] = $IcePolicy
    Write-Host "[triple-ui-smoke] forcing MEETING_ICE_POLICY=$IcePolicy"
}
if ($SoakMs -gt 0) {
    $clientBaseEnv["MEETING_SMOKE_SOAK_MS"] = "$SoakMs"
}
if ($SubscriberMediaSoakMs -gt 0) {
    $clientBaseEnv["MEETING_SMOKE_GUEST_MEDIA_SOAK_MS"] = "$SubscriberMediaSoakMs"
}
    if ($RequireChat) {
        $clientBaseEnv["MEETING_SMOKE_REQUIRE_CHAT"] = "1"
    }
if ($Headless) {
    $clientBaseEnv["QT_QPA_PLATFORM"] = "offscreen"
}
if (-not [string]::IsNullOrWhiteSpace($resolvedCameraCaptureBackend)) {
    $clientBaseEnv["MEETING_CAMERA_CAPTURE_BACKEND"] = $resolvedCameraCaptureBackend
    Write-Host "[triple-ui-smoke] forcing MEETING_CAMERA_CAPTURE_BACKEND=$resolvedCameraCaptureBackend"
}

$hostUsesSyntheticAudio = $hostAudioSourceMode -eq "synthetic"
$subscriberAUsesSyntheticAudio = $subscriberAAudioSourceMode -eq "synthetic"
$subscriberBUsesSyntheticAudio = $subscriberBAudioSourceMode -eq "synthetic"
$hostUsesSyntheticCamera = $hostCameraSourceMode -eq "synthetic"
$subscriberAUsesSyntheticCamera = $subscriberACameraSourceMode -eq "synthetic"
$subscriberBUsesSyntheticCamera = $subscriberBCameraSourceMode -eq "synthetic"

Write-Host "[triple-ui-smoke] role-publish host(audio=$HostPublishAudio video=$HostPublishVideo) subscriber_a(audio=$SubscriberAPublishAudio video=$SubscriberAPublishVideo) subscriber_b(audio=$SubscriberBPublishAudio video=$SubscriberBPublishVideo)"
Write-Host "[triple-ui-smoke] role-audio-source host=$hostAudioSourceMode subscriber_a=$subscriberAAudioSourceMode subscriber_b=$subscriberBAudioSourceMode"
Write-Host "[triple-ui-smoke] role-camera-source host=$hostCameraSourceMode subscriber_a=$subscriberACameraSourceMode subscriber_b=$subscriberBCameraSourceMode"
Write-Host "[triple-ui-smoke] role-audio-device host_in='$HostAudioInputDevice' host_out='$HostAudioOutputDevice' subscriber_a_in='$SubscriberAAudioInputDevice' subscriber_a_out='$SubscriberAAudioOutputDevice' subscriber_b_in='$SubscriberBAudioInputDevice' subscriber_b_out='$SubscriberBAudioOutputDevice'"

function New-ClientEnv {
    param(
        [string]$Role,
        [string]$Username,
        [string]$Password,
        [string]$DbPath,
        [string]$ResultPath,
        [string]$PeerResultPaths,
        [string]$ObservedRemoteVideoUserId,
        [bool]$EnableLocalAudio,
        [bool]$DisableLocalAudio,
        [bool]$EnableLocalVideo,
        [bool]$DisableLocalVideo,
        [bool]$RequireAudioEvidence,
        [bool]$RequireVideoEvidence,
        [bool]$UseSyntheticAudio,
        [bool]$UseSyntheticCamera,
        [bool]$ExpectCameraSource,
        [bool]$RequireMediaStateEvidence,
        [bool]$ToggleMediaState,
        [string]$MediaStatePeerUserId,
        [string]$CameraDeviceName,
        [string]$AudioInputDeviceName,
        [string]$AudioOutputDeviceName
    )

    $envTable = $clientBaseEnv.Clone()
    $envTable["MEETING_SMOKE_ROLE"] = $Role
    $envTable["MEETING_SMOKE_USERNAME"] = $Username
    $envTable["MEETING_SMOKE_PASSWORD"] = $Password
    $envTable["MEETING_DB_PATH"] = $DbPath
    $envTable["MEETING_SMOKE_RESULT_PATH"] = $ResultPath

    if (-not [string]::IsNullOrWhiteSpace($PeerResultPaths)) {
        $envTable["MEETING_SMOKE_PEER_RESULT_PATHS"] = $PeerResultPaths
    }
    if (-not [string]::IsNullOrWhiteSpace($ObservedRemoteVideoUserId)) {
        $envTable["MEETING_SMOKE_REMOTE_VIDEO_USER_ID"] = $ObservedRemoteVideoUserId
    }
    $envTable["MEETING_SMOKE_ENABLE_LOCAL_AUDIO"] = if ($EnableLocalAudio) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_DISABLE_LOCAL_AUDIO"] = if ($DisableLocalAudio) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_ENABLE_LOCAL_VIDEO"] = if ($EnableLocalVideo) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_DISABLE_LOCAL_VIDEO"] = if ($DisableLocalVideo) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_REQUIRE_AUDIO"] = if ($RequireAudioEvidence) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_REQUIRE_VIDEO"] = if ($RequireVideoEvidence) { "1" } else { "0" }
    $envTable["MEETING_SYNTHETIC_AUDIO"] = if ($UseSyntheticAudio) { "1" } else { "0" }
    $envTable["MEETING_SYNTHETIC_CAMERA"] = if ($UseSyntheticCamera) { "1" } else { "0" }
    $envTable["MEETING_SMOKE_EXPECT_CAMERA_SOURCE"] = if ($ExpectCameraSource) {
        if ($UseSyntheticCamera) { "synthetic-fallback" } else { "real-device" }
    } else {
        ""
    }
    if ($RequireMediaStateEvidence) {
        $envTable["MEETING_SMOKE_REQUIRE_MEDIA_STATE_SYNC"] = "1"
        $envTable["MEETING_SMOKE_REQUIRE_CAMERA_TOGGLE_RECOVERY"] = "1"
        if ($ToggleMediaState) {
            $envTable["MEETING_SMOKE_MEDIA_STATE_TOGGLE_LOCAL"] = "1"
            $envTable["MEETING_SMOKE_MEDIA_STATE_INITIAL_DELAY_MS"] = "3000"
            $envTable["MEETING_SMOKE_MEDIA_STATE_STEP_DELAY_MS"] = "2000"
        } elseif (-not [string]::IsNullOrWhiteSpace($MediaStatePeerUserId)) {
            $envTable["MEETING_SMOKE_MEDIA_STATE_PEER_USER_ID"] = $MediaStatePeerUserId
        }
    }

    $envTable["MEETING_CAMERA_DEVICE_NAME"] = if (-not [string]::IsNullOrWhiteSpace($CameraDeviceName)) {
        $CameraDeviceName
    } else {
        ""
    }
    $envTable["MEETING_AUDIO_INPUT_DEVICE_NAME"] = if (-not [string]::IsNullOrWhiteSpace($AudioInputDeviceName)) {
        $AudioInputDeviceName
    } else {
        ""
    }
    $envTable["MEETING_AUDIO_OUTPUT_DEVICE_NAME"] = if (-not [string]::IsNullOrWhiteSpace($AudioOutputDeviceName)) {
        $AudioOutputDeviceName
    } else {
        ""
    }

    return $envTable
}

$serverProcess = $null
$sfuProcess = $null
$hostProcess = $null
$subscriberAProcess = $null
$subscriberBProcess = $null

try {
    if ($usingExternalServer) {
        Write-Host "[triple-ui-smoke] external signaling=${ExternalServerHost}:$ExternalServerPort"
        if (-not (Wait-ForTcpListening -TargetHost $ExternalServerHost -Port $ExternalServerPort -TimeoutSeconds 20)) {
            throw "external signaling server did not accept TCP connections at ${ExternalServerHost}:$ExternalServerPort"
        }
    } else {
        $sfuEnv = @{
            SFU_ADVERTISED_HOST = "127.0.0.1"
            SFU_RPC_LISTEN_PORT = "$sfuRpcPort"
            SFU_MEDIA_LISTEN_PORT = "$sfuMediaPort"
            SFU_NODE_ID = "triple-ui-smoke-sfu"
        }
        $sfuProcess = Start-ManagedProcess -FilePath $resolvedSfuExe -Arguments @() -WorkingDirectory (Split-Path -Parent $resolvedSfuExe) -Environment $sfuEnv -StdOutPath $sfuStdOut -StdErrPath $sfuStdErr
        if (-not (Wait-ForTcpListening -TargetHost "127.0.0.1" -Port $sfuRpcPort -TimeoutSeconds 20)) {
            throw "SFU RPC did not start listening`n$(Get-Content $sfuStdErr -Raw)"
        }
        Write-Host "[triple-ui-smoke] internal SFU rpc=127.0.0.1:$sfuRpcPort media=127.0.0.1:$sfuMediaPort"

        $serverProcess = Start-ManagedProcess -FilePath "go" -Arguments @("run", ".") -WorkingDirectory $ServerDir -Environment $serverEnv -StdOutPath $serverStdOut -StdErrPath $serverStdErr
        if (-not (Wait-ForTcpListening -TargetHost "127.0.0.1" -Port $port -TimeoutSeconds 20)) {
            throw "signaling server did not start listening`n$(Get-Content $serverStdErr -Raw)"
        }
    }

$requireMediaStateEvidence = $RequireMediaStateSync -or $RequireCameraToggleRecovery
$hostPeerResultPaths = if ($requireMediaStateEvidence) { "" } else { "$subscriberAResultPath;$subscriberBResultPath" }
$hostEnv = New-ClientEnv -Role "host" -Username "demo" -Password "demo" -DbPath (Join-Path $tempRoot "host.sqlite") -ResultPath $hostResultPath -PeerResultPaths $hostPeerResultPaths -ObservedRemoteVideoUserId "" -EnableLocalAudio $HostPublishAudio -DisableLocalAudio (-not $HostPublishAudio) -EnableLocalVideo $HostPublishVideo -DisableLocalVideo (-not $HostPublishVideo) -RequireAudioEvidence ($RequireAudio -and $HostPublishAudio) -RequireVideoEvidence $false -UseSyntheticAudio $hostUsesSyntheticAudio -UseSyntheticCamera $hostUsesSyntheticCamera -ExpectCameraSource ($HostPublishVideo -and $ExpectRealCamera) -RequireMediaStateEvidence $requireMediaStateEvidence -ToggleMediaState $requireMediaStateEvidence -MediaStatePeerUserId "" -CameraDeviceName $HostCameraDevice -AudioInputDeviceName $HostAudioInputDevice -AudioOutputDeviceName $HostAudioOutputDevice
    if ($RequireChat) {
        $hostEnv["MEETING_SMOKE_CHAT_SEND_TEXT"] = "cloud-chat-host-history"
        $hostEnv["MEETING_SMOKE_CHAT_EXPECT_TEXTS"] = "cloud-chat-subscriber-a-to-host"
        $hostEnv["MEETING_SMOKE_CHAT_SEND_DELAY_MS"] = "1000"
    }

$subscriberAEnv = New-ClientEnv -Role "subscriber_a" -Username "alice" -Password "alice" -DbPath (Join-Path $tempRoot "subscriber_a.sqlite") -ResultPath $subscriberAResultPath -PeerResultPaths "" -ObservedRemoteVideoUserId $SubscriberAObservedRemoteVideoUserId -EnableLocalAudio $SubscriberAPublishAudio -DisableLocalAudio (-not $SubscriberAPublishAudio) -EnableLocalVideo $SubscriberAPublishVideo -DisableLocalVideo (-not $SubscriberAPublishVideo) -RequireAudioEvidence ($RequireAudio -and $SubscriberAPublishAudio) -RequireVideoEvidence $RequireVideo -UseSyntheticAudio $subscriberAUsesSyntheticAudio -UseSyntheticCamera $subscriberAUsesSyntheticCamera -ExpectCameraSource ($SubscriberAPublishVideo -and $ExpectRealCamera) -RequireMediaStateEvidence $requireMediaStateEvidence -ToggleMediaState $false -MediaStatePeerUserId "u1001" -CameraDeviceName $SubscriberACameraDevice -AudioInputDeviceName $SubscriberAAudioInputDevice -AudioOutputDeviceName $SubscriberAAudioOutputDevice
$subscriberBEnv = New-ClientEnv -Role "subscriber_b" -Username "bob" -Password "bob" -DbPath (Join-Path $tempRoot "subscriber_b.sqlite") -ResultPath $subscriberBResultPath -PeerResultPaths "" -ObservedRemoteVideoUserId $SubscriberBObservedRemoteVideoUserId -EnableLocalAudio $SubscriberBPublishAudio -DisableLocalAudio (-not $SubscriberBPublishAudio) -EnableLocalVideo $SubscriberBPublishVideo -DisableLocalVideo (-not $SubscriberBPublishVideo) -RequireAudioEvidence ($RequireAudio -and $SubscriberBPublishAudio) -RequireVideoEvidence $RequireVideo -UseSyntheticAudio $subscriberBUsesSyntheticAudio -UseSyntheticCamera $subscriberBUsesSyntheticCamera -ExpectCameraSource ($SubscriberBPublishVideo -and $ExpectRealCamera) -RequireMediaStateEvidence $requireMediaStateEvidence -ToggleMediaState $false -MediaStatePeerUserId "u1001" -CameraDeviceName $SubscriberBCameraDevice -AudioInputDeviceName $SubscriberBAudioInputDevice -AudioOutputDeviceName $SubscriberBAudioOutputDevice
    if ($requireMediaStateEvidence) {
        $subscriberAEnv["MEETING_SMOKE_SOAK_MS"] = "0"
        $subscriberAEnv["MEETING_SMOKE_GUEST_MEDIA_SOAK_MS"] = "0"
        $subscriberBEnv["MEETING_SMOKE_SOAK_MS"] = "0"
        $subscriberBEnv["MEETING_SMOKE_GUEST_MEDIA_SOAK_MS"] = "0"
    }
    if ($RequireChat) {
        $subscriberAEnv["MEETING_SMOKE_CHAT_SEND_TEXT"] = "cloud-chat-subscriber-a-to-host"
        $subscriberAEnv["MEETING_SMOKE_CHAT_EXPECT_TEXTS"] = "cloud-chat-host-history"
        $subscriberAEnv["MEETING_SMOKE_CHAT_SEND_DELAY_MS"] = "3000"
        $subscriberBEnv["MEETING_SMOKE_CHAT_EXPECT_TEXTS"] = "cloud-chat-host-history"
    }

    $clientDir = Split-Path -Parent $ClientExe
    $hostProcess = Start-ManagedProcess -FilePath $ClientExe -Arguments @() -WorkingDirectory $clientDir -Environment $hostEnv -StdOutPath $hostStdOut -StdErrPath $hostStdErr
    $meetingIdDeadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $meetingIdDeadline) {
        $meetingId = Get-PublishedMeetingId -Path $meetingIdPath
        if (-not [string]::IsNullOrWhiteSpace($meetingId)) {
            break
        }
        if ($hostProcess.Process.HasExited) {
            throw "host exited before publishing meeting id`n$(Get-Content $hostResultPath -Raw)"
        }
        Start-Sleep -Milliseconds 200
    }
    if ([string]::IsNullOrWhiteSpace((Get-PublishedMeetingId -Path $meetingIdPath))) {
        throw "meeting id was not published in time`n$(if (Test-Path $hostResultPath) { Get-Content $hostResultPath -Raw } else { '<missing host result>' })"
    }

    $subscriberAProcess = Start-ManagedProcess -FilePath $ClientExe -Arguments @() -WorkingDirectory $clientDir -Environment $subscriberAEnv -StdOutPath $subscriberAStdOut -StdErrPath $subscriberAStdErr
    Start-Sleep -Milliseconds $SubscriberBLaunchDelayMs
    $subscriberBProcess = Start-ManagedProcess -FilePath $ClientExe -Arguments @() -WorkingDirectory $clientDir -Environment $subscriberBEnv -StdOutPath $subscriberBStdOut -StdErrPath $subscriberBStdErr

    $hostExited = Wait-ForProcessExit -Process $hostProcess.Process -TimeoutSeconds $TimeoutSeconds
    $subscriberAExited = Wait-ForProcessExit -Process $subscriberAProcess.Process -TimeoutSeconds $TimeoutSeconds
    $subscriberBExited = Wait-ForProcessExit -Process $subscriberBProcess.Process -TimeoutSeconds $TimeoutSeconds
    if (-not $hostExited -or -not $subscriberAExited -or -not $subscriberBExited) {
        throw "client timeout"
    }

    $hostResult = if (Test-Path $hostResultPath) { Get-Content $hostResultPath -Raw } else { "<missing>" }
    $subscriberAResult = if (Test-Path $subscriberAResultPath) { Get-Content $subscriberAResultPath -Raw } else { "<missing>" }
    $subscriberBResult = if (Test-Path $subscriberBResultPath) { Get-Content $subscriberBResultPath -Raw } else { "<missing>" }

    $hostOk = $hostProcess.Process.ExitCode -eq 0
    $subscriberAOk = $subscriberAProcess.Process.ExitCode -eq 0
    $subscriberBOk = $subscriberBProcess.Process.ExitCode -eq 0
    if (-not $hostOk -or -not $subscriberAOk -or -not $subscriberBOk) {
        throw @"
host exit=$($hostProcess.Process.ExitCode)
$hostResult

subscriber_a exit=$($subscriberAProcess.Process.ExitCode)
$subscriberAResult

subscriber_b exit=$($subscriberBProcess.Process.ExitCode)
$subscriberBResult
"@
    }

    Write-Output "triple ui smoke passed"
    Write-Output "temp root: $tempRoot"
    exit 0
} catch {
    Write-Error $_
    Write-Error ("temp root: " + $tempRoot)
    if (Test-Path $hostResultPath) {
        Write-Error ("host result:`n" + (Get-Content $hostResultPath -Raw))
    }
    if (Test-Path $subscriberAResultPath) {
        Write-Error ("subscriber_a result:`n" + (Get-Content $subscriberAResultPath -Raw))
    }
    if (Test-Path $subscriberBResultPath) {
        Write-Error ("subscriber_b result:`n" + (Get-Content $subscriberBResultPath -Raw))
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
    Stop-ManagedProcess $subscriberAProcess
    Stop-ManagedProcess $subscriberBProcess
    Stop-ManagedProcess $serverProcess
    Stop-ManagedProcess $sfuProcess
}
