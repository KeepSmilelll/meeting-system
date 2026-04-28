param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerHost = "123.207.41.63",
    [ValidateRange(1, 65535)]
    [int]$ServerPort = 8443,
    [ValidateSet("both", "all", "relay-only")]
    [string]$Mode = "both",
    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 240,
    [ValidateRange(0, 30000)]
    [int]$GuestLaunchDelayMs = 3000,
    [string]$ServerSshTarget = "",
    [string]$ServerComposeDir = "/opt/meeting/meeting-server",
    [switch]$Headless
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$uiScript = Join-Path $scriptRoot "run_meeting_client_dual_ui_smoke.ps1"
if (-not (Test-Path -LiteralPath $uiScript)) {
    throw "UI smoke script not found: $uiScript"
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
        Write-Host "[cloud-dual] ops snapshot skipped: pass -ServerSshTarget root@$ServerHost to collect compose/logs/stats"
        return
    }

    $remoteCommand = "cd '$ServerComposeDir' && " +
        "echo '===== compose ps ($Label) =====' && sudo docker compose -f docker-compose.yml -f docker-compose.2c2g.yml ps && " +
        "echo '===== logs signaling/sfu/coturn ($Label) =====' && sudo docker compose -f docker-compose.yml -f docker-compose.2c2g.yml logs --tail=80 signaling sfu coturn && " +
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
        RequireAvSync = $true
        IcePolicy = $icePolicy
        TimeoutSeconds = $TimeoutSeconds
        GuestLaunchDelayMs = $GuestLaunchDelayMs
    }
    if ($Headless) {
        $params.Headless = $true
    }

    Write-Host "[cloud-dual] >>> ice_policy=$icePolicy server=${ServerHost}:$ServerPort"
    & $uiScript @params
    if ($LASTEXITCODE -ne 0) {
        throw "cloud dual validation failed for ice_policy=$icePolicy, exit_code=$LASTEXITCODE"
    }
    Write-CloudOpsSnapshot -Label "dual-$icePolicy"
    Write-Host "[cloud-dual] <<< ice_policy=$icePolicy passed"
}

Write-Host "[cloud-dual] all requested validations passed"
