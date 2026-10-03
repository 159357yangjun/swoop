# install_host.ps1 — 注册 IDM Next Native Messaging Host（Windows）
# 便携包：默认 CurrentUser；安装版由 NSIS 以管理员权限传 -Scope Machine。
param(
    [ValidateSet("CurrentUser", "Machine")]
    [string]$Scope = "CurrentUser"
)

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

# 仓库里的 com.tencent.idm_next.json 是**模板**，安装器不许改写它（跑一次就把开发树弄脏；
# 且 PowerShell 5.1 的 Set-Content -Encoding UTF8 会写 BOM，浏览器按 JSON 读会当场报错）。
# 正确落点：把 manifest **生成**到浏览器自己查找的用户数据目录，并在生成物里写绝对 path。
# 官方契约允许 Windows 相对路径（相对 manifest 所在目录），但生成物一旦离开扩展目录，
# 相对值就指不到 exe —— 所以这里一律解析成绝对路径再落盘。
$SourceJson = $JsonPath
$TargetDir = if ($Scope -eq "Machine") {
    Join-Path $env:ProgramData "IDMNext\NativeMessagingHosts"
} else {
    Join-Path $env:LOCALAPPDATA "IDMNext\NativeMessagingHosts"
}
if (-not (Test-Path $TargetDir)) {
    New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
}

$Json = Get-Content -Raw -Encoding UTF8 $SourceJson | ConvertFrom-Json
$Json.path = $HostExe

$JsonPath = Join-Path $TargetDir "com.tencent.idm_next.json"
# [IO.File]::WriteAllText + UTF8Encoding($false) = 无 BOM。Set-Content 做不到这一点。
[System.IO.File]::WriteAllText(
    $JsonPath,
    ($Json | ConvertTo-Json -Compress),
    (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Native Messaging host: $HostExe" -ForegroundColor Gray
Write-Host "生成的 manifest: $JsonPath（模板 $SourceJson 未被修改）" -ForegroundColor Gray

# 管理员安装不能写 HKCU：当标准用户输入管理员凭据时，HKCU 会指向管理员账户，
# 浏览器实际运行用户将完全看不到该 host。安装版因此使用 HKLM；便携包仍默认 HKCU。
$RegistryRoot = if ($Scope -eq "Machine") { "HKLM:\Software" } else { "HKCU:\Software" }
$browsers = @(
    (Join-Path $RegistryRoot "Google\Chrome\NativeMessagingHosts\com.tencent.idm_next"),
    (Join-Path $RegistryRoot "Microsoft\Edge\NativeMessagingHosts\com.tencent.idm_next")
)

foreach ($key in $browsers) {
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name "(Default)" -Value $JsonPath
    Write-Host "已注册: $key" -ForegroundColor Green
}

Write-Host ""
Write-Host "IDM Next Native Messaging Host 注册完成（$Scope）。" -ForegroundColor Cyan
Write-Host "manifest: $JsonPath"
Write-Host "Chrome/Edge 扩展 ID 由 manifest.json 中固定 key 保持稳定。" -ForegroundColor Gray
