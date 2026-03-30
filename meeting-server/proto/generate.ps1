param(
    [string]$ProtoFile = "signaling.proto"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$protoPath = Join-Path $scriptDir $ProtoFile
$goOut = Join-Path $scriptDir "..\signaling\protocol\pb"

if (-not (Get-Command protoc -ErrorAction SilentlyContinue)) {
    Write-Host "protoc 未安装，请先安装 protobuf-compiler。" -ForegroundColor Yellow
    exit 1
}

if (-not (Get-Command protoc-gen-go -ErrorAction SilentlyContinue)) {
    Write-Host "protoc-gen-go 未安装，请执行: go install google.golang.org/protobuf/cmd/protoc-gen-go@latest" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Force -Path $goOut | Out-Null

protoc --proto_path=$scriptDir --go_out=$goOut --go_opt=paths=source_relative $protoPath

Write-Host "已生成 Go Protobuf 代码到: $goOut" -ForegroundColor Green
