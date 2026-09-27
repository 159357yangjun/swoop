# install_host.ps1 — 注册 IDM Next Native Messaging Host（Windows）
# 便携包：当前用户直接运行即可；安装版会由 NSIS 自动调用。
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$JsonPath  = Join-Path $ScriptDir "com.tencent.idm_next.json"

if (-not (Test-Path $JsonPath)) {
    throw "未找到 Native Messaging manifest: $JsonPath"
}

# 开发目录中 host 会同步到本目录；正式便携/安装包中 host 位于产品根目录。
$HostCandidates = @(
    (Join-Path $ScriptDir "idm-next-host.exe"),
    (Join-Path $ScriptDir "..\..\idm-next-host.exe")
)
$HostExe = $HostCandidates |
    Where-Object { Test-Path $_ } |
    ForEach-Object { (Resolve-Path $_).Path } |
    Select-Object -First 1

if (-not $HostExe) {
    throw "未找到 idm-next-host.exe。请保持 browser-extension 与主程序目录结构不变。"
}

# Chrome/Edge 要求 manifest.path 为绝对路径，因此安装/解压位置变化后必须重写。
$Json = Get-Content -Raw -Encoding UTF8 $JsonPath | ConvertFrom-Json
$Json.path = $HostExe
$Json | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $JsonPath
Write-Host "Native Messaging host: $HostExe" -ForegroundColor Gray

$browsers = @(
    "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.tencent.idm_next",
    "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.tencent.idm_next"
)

foreach ($key in $browsers) {
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name "(Default)" -Value $JsonPath
    Write-Host "已注册: $key" -ForegroundColor Green
}

Write-Host ""
Write-Host "IDM Next Native Messaging Host 注册完成。" -ForegroundColor Cyan
Write-Host "manifest: $JsonPath"
Write-Host "Chrome/Edge 扩展 ID 由 manifest.json 中固定 key 保持稳定。" -ForegroundColor Gray
