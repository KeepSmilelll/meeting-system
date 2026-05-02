param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerHost = "123.207.41.63",
    [ValidateRange(1, 65535)]
    [int]$ServerPort = 8443,
    [ValidateSet("both", "all", "relay-only")]
    [string]$Mode = "both",
    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 150,
    [string]$VideoPreset = "360p",
    [string]$ServerSshTarget = "",
    [string]$ServerComposeDir = "/opt/meeting/meeting-server",
    [switch]$Headless
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$uiScript = Join-Path $scriptRoot "run_meeting_client_triple_ui_smoke.ps1"
if (-not (Test-Path -LiteralPath $uiScript)) {
    throw "triple UI smoke script not found: $uiScript"
}
if (-not (Test-Path -LiteralPath $ClientExe)) {
    throw "meeting_client.exe not found: $ClientExe"
}

$runs = switch ($Mode) {
    "both" { @("all", "relay-only") }
    "all" { @("all") }
    "relay-only" { @("relay-only") }
}

function Write-CloudOpsSnapshot {
    param(
        [string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($ServerSshTarget)) {
        Write-Host "[cloud-triple] ops snapshot skipped: pass -ServerSshTarget root@$ServerHost to collect compose/logs/stats"
        return
    }

    $remoteCommand = "cd '$ServerComposeDir' && " +
        "echo '===== compose ps ($Label) =====' && sudo docker compose -f docker-compose.yml -f docker-compose.2c2g.yml ps && " +
        "echo '===== logs signaling/sfu/coturn ($Label) =====' && sudo docker compose -f docker-compose.yml -f docker-compose.2c2g.yml logs --tail=120 signaling sfu coturn && " +
        "echo '===== docker stats ($Label) =====' && sudo docker stats --no-stream"
    & ssh $ServerSshTarget $remoteCommand
    if ($LASTEXITCODE -ne 0) {
        throw "cloud ops snapshot failed for $Label, exit_code=$LASTEXITCODE"
    }
}

foreach ($icePolicy in $runs) {
    $params = @{
        ClientExe = $ClientExe
        ExternalServerHost = $ServerHost
        ExternalServerPort = $ServerPort
        SyntheticAudio = $true
        SyntheticCamera = $true
        RequireAudio = $true
        RequireVideo = $true
        RequireMediaStateSync = $true
        RequireCameraToggleRecovery = $true
        HostAudioSource = "synthetic"
        SubscriberAAudioSource = "synthetic"
        SubscriberBAudioSource = "synthetic"
        HostCameraSource = "synthetic"
        SubscriberACameraSource = "synthetic"
        SubscriberBCameraSource = "synthetic"
        HostPublishAudio = $true
        HostPublishVideo = $true
        SubscriberAPublishAudio = $true
        SubscriberAPublishVideo = $true
        SubscriberBPublishAudio = $true
        SubscriberBPublishVideo = $true
        ExpectRealCamera = $false
        IcePolicy = $icePolicy
        TimeoutSeconds = $TimeoutSeconds
        SoakMs = 90000
        SubscriberMediaSoakMs = 25000
        RequireChat = $true
        SubscriberBLaunchDelayMs = 5000
    }
    if ($Headless) {
        $params.Headless = $true
    }

    $previousVideoPreset = [Environment]::GetEnvironmentVariable("MEETING_VIDEO_PRESET", "Process")
    $hadVideoPreset = $null -ne $previousVideoPreset
    if (-not [string]::IsNullOrWhiteSpace($VideoPreset)) {
        $env:MEETING_VIDEO_PRESET = $VideoPreset
    }

    Write-Host "[cloud-triple] >>> ice_policy=$icePolicy server=${ServerHost}:$ServerPort video_preset=$VideoPreset"
    try {
        & $uiScript @params
        if ($LASTEXITCODE -ne 0) {
            throw "cloud triple validation failed for ice_policy=$icePolicy, exit_code=$LASTEXITCODE"
        }
    } finally {
        if ($hadVideoPreset) {
            $env:MEETING_VIDEO_PRESET = $previousVideoPreset
        } else {
            Remove-Item Env:\MEETING_VIDEO_PRESET -ErrorAction SilentlyContinue
        }
    }
    Write-CloudOpsSnapshot -Label "triple-$icePolicy"
    Write-Host "[cloud-triple] <<< ice_policy=$icePolicy passed"
}

Write-Host "[cloud-triple] all requested validations passed"
