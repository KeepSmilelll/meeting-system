param(
    [string[]]$ProtoFiles = @("signaling.proto", "sfu_rpc.proto")
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$goOut = Join-Path $scriptDir "..\signaling\protocol\pb"
$sfuCppOut = Join-Path $scriptDir "..\sfu\proto\pb"
$clientCppOut = Join-Path $scriptDir "..\..\plasma-hawking\proto\pb"

$env:PATH = "D:\go-env\protoc\bin;D:\go-env\gopath\bin;" + $env:PATH

if (-not (Get-Command protoc -ErrorAction SilentlyContinue)) {
    Write-Host "protoc 未安装，期待路径 D:\go-env\protoc\bin\protoc.exe" -ForegroundColor Yellow
    exit 1
}

if (-not (Get-Command protoc-gen-go -ErrorAction SilentlyContinue)) {
    Write-Host "protoc-gen-go 未安装，请执行 go install google.golang.org/protobuf/cmd/protoc-gen-go@latest" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Force -Path $goOut, $sfuCppOut, $clientCppOut | Out-Null

$protoPaths = $ProtoFiles | ForEach-Object { Join-Path $scriptDir $_ }

protoc --proto_path=$scriptDir --go_out=$goOut --go_opt=paths=source_relative $protoPaths
protoc --proto_path=$scriptDir --cpp_out=$sfuCppOut $protoPaths
protoc --proto_path=$scriptDir --cpp_out=$clientCppOut (Join-Path $scriptDir "signaling.proto")

Write-Host "已生成 Go Protobuf 到: $goOut" -ForegroundColor Green
Write-Host "已生成 SFU C++ Protobuf 到: $sfuCppOut" -ForegroundColor Green
Write-Host "已生成 Client C++ Protobuf 到: $clientCppOut" -ForegroundColor Green
