# ============================================================
#  IDM Next 打包脚本 (PowerShell)
#  - 用 windeployqt 收集 Qt 运行库到 installer/dist
#  - 裁剪不需要的 SQL 驱动 / 翻译以减小体积
#  - 若已安装 NSIS，则直接生成 installer/idm-next-setup-x64.exe
#  用法：在仓库根目录执行  powershell -File installer/package.ps1
# ============================================================
$ErrorActionPreference = "Stop"

$Root  = Resolve-Path (Join-Path $PSScriptRoot "..")
$Build = Join-Path $Root "build"
$Dist  = Join-Path $Root "installer\dist"
$QtBin = "C:\Qt\6.11.1\mingw_64\bin"

# 1. 清空并重建 dist
if (Test-Path $Dist) { Remove-Item $Dist -Recurse -Force }
New-Item $Dist -ItemType Directory -Force | Out-Null

# 2. 拷贝主程序与浏览器扩展宿主
Copy-Item (Join-Path $Build "idm-next.exe")      $Dist
Copy-Item (Join-Path $Build "idm-next-host.exe") $Dist

# 3. windeployqt 收集 Qt 运行库（含 Qt6Sql + sqldrivers/qsqlite.dll）
$env:PATH = "$QtBin;$env:PATH"
& "$QtBin\windeployqt.exe" --release (Join-Path $Dist "idm-next.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt 失败" }

# 4. 裁剪：仅保留 SQLite 驱动，删除其余 SQL 驱动（ibase/oci/odbc/psql/mimer）
$SqlDir = Join-Path $Dist "sqldrivers"
if (Test-Path $SqlDir) {
    Get-ChildItem $SqlDir -Filter "qsql*.dll" | Where-Object {
        $_.Name -ne "qsqlite.dll"
    } | Remove-Item -Force
}

# 5. 裁剪：删除 Qt 翻译文件（本程序 UI 字符串为内置，不依赖 Qt 翻译）
$TransDir = Join-Path $Dist "translations"
if (Test-Path $TransDir) { Remove-Item $TransDir -Recurse -Force }

# 5.5 打包辅助二进制（aria2c / ffmpeg / yt-dlp），缺失则自动下载，离线开箱即用
function Ensure-ZipTool {
    param($Url, $DestDir, $ExeName)
    $exe = Join-Path $DestDir $ExeName
    if (Test-Path $exe) { Write-Host "已存在，跳过: $exe"; return $true }
    New-Item $DestDir -ItemType Directory -Force | Out-Null
    $tmp = Join-Path $env:TEMP ("dl-" + [guid]::NewGuid().ToString("N") + ".zip")
    Write-Host "下载 $ExeName <- $Url"
    try { Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing } catch { Write-Warning "下载失败: $_"; return $false }
    $ex = Join-Path $env:TEMP ("ex-" + [guid]::NewGuid().ToString("N"))
    New-Item $ex -ItemType Directory -Force | Out-Null
    Expand-Archive -Path $tmp -DestinationPath $ex -Force
    $found = Get-ChildItem $ex -Recurse -Filter $ExeName | Select-Object -First 1
    if (-not $found) { Write-Warning "压缩包中未找到 $ExeName"; return $false }
    # ffmpeg 为动态链接：需连同同目录 DLL 一起复制；其余单 exe 直接复制即可
    foreach ($f in Get-ChildItem $found.DirectoryName -File) { Copy-Item $f.FullName -Destination $DestDir -Force }
    Remove-Item $tmp -Force; Remove-Item $ex -Recurse -Force
    Write-Host "已放置 $ExeName -> $DestDir"
    return $true
}

# aria2c（BT/磁力/FTP 后端，单 exe 静态链接）
Ensure-ZipTool -Url "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip" `
               -DestDir (Join-Path $Dist "aria2") -ExeName "aria2c.exe"
# ffmpeg（HLS TS→MP4 转封装 / 加密流转码，动态链接需 DLL 同目录）
Ensure-ZipTool -Url "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" `
               -DestDir (Join-Path $Dist "ffmpeg") -ExeName "ffmpeg.exe"
# yt-dlp（视频站点后端，单 exe）
$yt = Join-Path $Dist "yt-dlp\yt-dlp.exe"
if (-not (Test-Path $yt)) {
    New-Item (Join-Path $Dist "yt-dlp") -ItemType Directory -Force | Out-Null
    try { Invoke-WebRequest -Uri "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" `
                            -OutFile $yt -UseBasicParsing; Write-Host "已下载 yt-dlp -> $yt" }
    catch { Write-Warning "yt-dlp 下载失败: $_" }
}

# 6. 生成安装包（需安装 NSIS）
$Makensis = "C:\Program Files (x86)\NSIS\makensis.exe"
if (Test-Path $Makensis) {
    & $Makensis (Join-Path $Root "installer\idm-next.nsi")
    if ($LASTEXITCODE -eq 0) {
        Write-Host "安装包已生成：installer\idm-next-setup-x64.exe"
    } else {
        throw "makensis 失败"
    }
} else {
    Write-Host "NSIS 未安装，已准备好 installer/dist/。请安装 NSIS 后执行："
    Write-Host "  makensis installer\idm-next.nsi"
}
