param(
    [string]$ProtocPath = "",
    [string[]]$ProtoFiles = @("signaling.proto", "sfu_rpc.proto")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$goOut = Join-Path $scriptDir "..\signaling\protocol\pb"
$sfuCppOut = Join-Path $scriptDir "..\sfu\proto\pb"
$clientCppOut = Join-Path $scriptDir "..\..\plasma-hawking\proto\pb"

function Resolve-ProtocPath {
    param(
        [string]$ExplicitPath
    )

    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) {
        $candidates += $ExplicitPath
    }
    if (-not [string]::IsNullOrWhiteSpace($env:PROTOC_BIN)) {
        $candidates += $env:PROTOC_BIN
    }
    if (-not [string]::IsNullOrWhiteSpace($env:PROTOC_PATH)) {
        $candidates += $env:PROTOC_PATH
    }
    $candidates += "D:\go-env\protoc-3.19.1\bin\protoc.exe"
    $candidates += "protoc"

    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }

        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }

        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) {
            return $cmd.Source
        }
    }

    throw "Unable to locate protoc. Pass -ProtocPath, set PROTOC_BIN/PROTOC_PATH, or install protoc on PATH."
}

$protoc = Resolve-ProtocPath -ExplicitPath $ProtocPath

if (-not (Get-Command protoc-gen-go -ErrorAction SilentlyContinue)) {
    throw "protoc-gen-go not found. Install it with: go install google.golang.org/protobuf/cmd/protoc-gen-go@latest"
}

foreach ($dir in @($goOut, $sfuCppOut, $clientCppOut)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$protoPaths = foreach ($file in $ProtoFiles) {
    $path = Join-Path $scriptDir $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing proto file: $path"
    }
    $path
}

$clientProto = Join-Path $scriptDir "signaling.proto"
if (-not (Test-Path -LiteralPath $clientProto -PathType Leaf)) {
    throw "Missing client proto file: $clientProto"
}

& $protoc --proto_path=$scriptDir --go_out=$goOut --go_opt=paths=source_relative @protoPaths
& $protoc --proto_path=$scriptDir --cpp_out=$sfuCppOut @protoPaths
& $protoc --proto_path=$scriptDir --cpp_out=lite:$clientCppOut $clientProto

Write-Host "Generated Go pb -> $goOut" -ForegroundColor Green
Write-Host "Generated SFU C++ pb -> $sfuCppOut" -ForegroundColor Green
Write-Host "Generated Qt lite C++ pb -> $clientCppOut" -ForegroundColor Green
Write-Host "Using protoc -> $protoc" -ForegroundColor Green
