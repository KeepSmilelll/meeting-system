param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build_phase4_5_check\Debug\meeting_client.exe",
    [string]$ServerDir = "D:\meeting\meeting-server\signaling",
    [switch]$SyntheticAudio,
    [switch]$Headless,
    [int]$TimeoutSeconds = 45
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return $listener.LocalEndpoint.Port
    } finally {
        $listener.Stop()
    }
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
        $process.Kill($true)
        $process.WaitForExit()
    }

    if ($process) {
        if ($Managed.StdOutPath) {
            [System.IO.File]::WriteAllText($Managed.StdOutPath, $process.StandardOutput.ReadToEnd())
        }
        if ($Managed.StdErrPath) {
            [System.IO.File]::WriteAllText($Managed.StdErrPath, $process.StandardError.ReadToEnd())
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

    return $Process.WaitForExit($TimeoutSeconds * 1000)
}

if (-not (Test-Path $ClientExe)) {
    throw "meeting_client.exe not found: $ClientExe"
}

$port = Get-FreeTcpPort
$tempRoot = Join-Path $env:TEMP ("meeting-ui-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$meetingIdPath = Join-Path $tempRoot "meeting_id.txt"
$hostResultPath = Join-Path $tempRoot "host.result.txt"
$guestResultPath = Join-Path $tempRoot "guest.result.txt"
$serverStdOut = Join-Path $tempRoot "server.stdout.txt"
$serverStdErr = Join-Path $tempRoot "server.stderr.txt"
$hostStdOut = Join-Path $tempRoot "host.stdout.txt"
$hostStdErr = Join-Path $tempRoot "host.stderr.txt"
$guestStdOut = Join-Path $tempRoot "guest.stdout.txt"
$guestStdErr = Join-Path $tempRoot "guest.stderr.txt"

$serverEnv = @{}
$serverEnv["SIGNALING_LISTEN_ADDR"] = "127.0.0.1:$port"
$serverEnv["SIGNALING_ENABLE_REDIS"] = "false"
$serverEnv["SIGNALING_MYSQL_DSN"] = ""

$clientBaseEnv = @{}
$clientBaseEnv["MEETING_RUNTIME_SMOKE"] = "1"
$clientBaseEnv["MEETING_SERVER_HOST"] = "127.0.0.1"
$clientBaseEnv["MEETING_SERVER_PORT"] = "$port"
$clientBaseEnv["MEETING_SMOKE_MEETING_ID_PATH"] = $meetingIdPath
$clientBaseEnv["MEETING_SMOKE_TIMEOUT_MS"] = "$($TimeoutSeconds * 1000)"
if ($SyntheticAudio) {
    $clientBaseEnv["MEETING_SYNTHETIC_AUDIO"] = "1"
}
if ($Headless) {
    $clientBaseEnv["QT_QPA_PLATFORM"] = "offscreen"
}

$serverProcess = $null
$hostProcess = $null
$guestProcess = $null

try {
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

    $guestEnv = $clientBaseEnv.Clone()
    $guestEnv["MEETING_SMOKE_ROLE"] = "guest"
    $guestEnv["MEETING_SMOKE_USERNAME"] = "alice"
    $guestEnv["MEETING_SMOKE_PASSWORD"] = "alice"
    $guestEnv["MEETING_DB_PATH"] = (Join-Path $tempRoot "guest.sqlite")
    $guestEnv["MEETING_SMOKE_RESULT_PATH"] = $guestResultPath
    $guestEnv["MEETING_SMOKE_PEER_RESULT_PATH"] = $hostResultPath

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
    if (Test-Path $hostResultPath) {
        Write-Error ("host result:`n" + (Get-Content $hostResultPath -Raw))
    }
    if (Test-Path $guestResultPath) {
        Write-Error ("guest result:`n" + (Get-Content $guestResultPath -Raw))
    }
    if (Test-Path $serverStdErr) {
        Write-Error ("server stderr:`n" + (Get-Content $serverStdErr -Raw))
    }
    exit 1
} finally {
    Stop-ManagedProcess $hostProcess
    Stop-ManagedProcess $guestProcess
    Stop-ManagedProcess $serverProcess
}
