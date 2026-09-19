# install_host.ps1 — 注册 IDM Next Native Messaging Host（Windows）
# 以当前用户权限运行即可，无需管理员。
# 用法：在 PowerShell 中执行  .\install_host.ps1
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$JsonPath  = Join-Path $ScriptDir "com.tencent.idm_next.json"

if (-not (Test-Path $JsonPath)) {
    Write-Host "未找到 manifest: $JsonPath" -ForegroundColor Red
    exit 1
}

# 把 manifest 的 path 改写为与本清单同目录的 C++ host 可执行文件，
# 这样开发目录与打包安装目录都能正确指向（不再写死开发路径）。
$HostExe = Join-Path $ScriptDir "idm-next-host.exe"
$Json = Get-Content -Raw -Encoding UTF8 $JsonPath | ConvertFrom-Json
$Json.path = $HostExe
$Json | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $JsonPath
Write-Host "manifest path 已更新为: $HostExe" -ForegroundColor Gray

# 同时为 Chrome 与 Edge 注册（HKCU 当前用户，无需管理员）
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
Write-Host "Native Messaging Host 注册完成。" -ForegroundColor Cyan
Write-Host "manifest 路径: $JsonPath"
Write-Host "allowed_origins 已预填本扩展固定 ID，无需手动修改。" -ForegroundColor Gray
Write-Host "请到 chrome://extensions 找到「IDM Next 嗅探器」点击刷新图标后生效。" -ForegroundColor Gray
