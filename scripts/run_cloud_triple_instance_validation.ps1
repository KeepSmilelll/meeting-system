param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerHost = "123.207.41.63",
    [ValidateRange(1, 65535)]
    [int]$ServerPort = 8443,
    [ValidateSet("both", "all", "relay-only")]
    [string]$Mode = "both",
    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 150,
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
        "echo '===== compose ps ($Label) =====' && docker compose -f docker-compose.yml -f docker-compose.2c2g.yml ps && " +
        "echo '===== logs signaling/sfu/coturn ($Label) =====' && docker compose -f docker-compose.yml -f docker-compose.2c2g.yml logs --tail=120 signaling sfu coturn && " +
        "echo '===== docker stats ($Label) =====' && docker stats --no-stream"
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
        SoakMs = 5000
        SubscriberMediaSoakMs = 10000
    }
    if ($Headless) {
        $params.Headless = $true
    }

    Write-Host "[cloud-triple] >>> ice_policy=$icePolicy server=${ServerHost}:$ServerPort"
    & $uiScript @params
    if ($LASTEXITCODE -ne 0) {
        throw "cloud triple validation failed for ice_policy=$icePolicy, exit_code=$LASTEXITCODE"
    }
    Write-CloudOpsSnapshot -Label "triple-$icePolicy"
    Write-Host "[cloud-triple] <<< ice_policy=$icePolicy passed"
}

Write-Host "[cloud-triple] all requested validations passed"
