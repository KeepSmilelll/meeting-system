param(
    [string]$ClientExe = "D:\meeting\plasma-hawking\build\Debug\meeting_client.exe",
    [string]$ServerHost = "123.207.41.63",
    [ValidateRange(1, 65535)]
    [int]$ServerPort = 8443,
    [ValidateSet("both", "all", "relay-only")]
    [string]$Mode = "both",
    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 120,
    [ValidateRange(0, 30000)]
    [int]$GuestLaunchDelayMs = 3000,
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
    Write-Host "[cloud-dual] <<< ice_policy=$icePolicy passed"
}

Write-Host "[cloud-dual] all requested validations passed"
