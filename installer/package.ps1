# ============================================================
# IDM Next Windows 发行打包脚本
# - 自动定位 Qt / windeployqt，不绑定某台机器的 C:\Qt\...
# - 收集 Qt、MinGW、libcurl 等完整运行时依赖闭包
# - 保留浏览器扩展正式 Native Messaging 注册文件
# - 下载并校验运行所需辅助工具
# - 生成便携 ZIP + SHA256
# - 安装 NSIS 时同时生成 setup.exe
# ============================================================
param(
    [string]$QtBin = $env:QT_BIN,
    [string]$Configuration = "Release",
    [switch]$SkipTools
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Root   = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Build  = Join-Path $Root "build"
$Dist   = Join-Path $Root "installer\dist"
$OutDir = Join-Path $Root "installer\out"

function Find-Executable {
    param([string]$Name, [string[]]$Candidates = @())
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) { return (Resolve-Path $candidate).Path }
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Resolve-QtBin {
    if ($QtBin) {
        $candidate = Join-Path $QtBin "windeployqt.exe"
        if (Test-Path $candidate) { return (Resolve-Path $QtBin).Path }
        throw "指定的 QtBin 无效：$QtBin（未找到 windeployqt.exe）"
    }

    $windeployqt = Find-Executable "windeployqt.exe"
    if ($windeployqt) { return (Split-Path $windeployqt -Parent) }

    $roots = @("C:\Qt", "D:\Qt") | Where-Object { Test-Path $_ }
    foreach ($qtRoot in $roots) {
        $found = Get-ChildItem $qtRoot -Recurse -Filter windeployqt.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'mingw_64\\bin\\windeployqt\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($found) { return $found.DirectoryName }
    }

    throw "未找到 Qt 6 的 windeployqt.exe。请设置 QT_BIN 或传入 -QtBin。"
}

function Resolve-Objdump {
    param([string]$ResolvedQtBin)

    $candidates = @(
        (Join-Path $ResolvedQtBin "objdump.exe")
    )

    $gcc = Get-Command gcc.exe -ErrorAction SilentlyContinue
    if ($gcc) {
        $candidates += (Join-Path (Split-Path $gcc.Source -Parent) "objdump.exe")
    }

    $objdump = Find-Executable "objdump.exe" $candidates
    if ($objdump) { return $objdump }

    foreach ($toolsRoot in @("C:\Qt\Tools", "D:\Qt\Tools")) {
        if (-not (Test-Path $toolsRoot)) { continue }
        $found = Get-ChildItem $toolsRoot -Recurse -Filter objdump.exe -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }

    throw "未找到 objdump.exe。MinGW 构建应包含 binutils；无法验证发行包运行时 DLL 依赖。"
}

function Get-ProjectVersion {
    $cmake = Get-Content (Join-Path $Root "CMakeLists.txt") -Raw -Encoding UTF8
    $match = [regex]::Match($cmake, 'project\(idm-next\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)', 'IgnoreCase')
    if (-not $match.Success) { throw "无法从 CMakeLists.txt 读取项目版本" }
    return $match.Groups[1].Value
}

function Download-File {
    param([string]$Url, [string]$Destination)
    Write-Host "下载: $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    if (-not (Test-Path $Destination) -or (Get-Item $Destination).Length -eq 0) {
        throw "下载失败或文件为空：$Url"
    }
}

function Ensure-ZipTool {
    param([string]$Url, [string]$DestDir, [string]$ExeName)
    $exe = Join-Path $DestDir $ExeName
    if (Test-Path $exe) { Write-Host "已存在: $exe"; return }

    New-Item $DestDir -ItemType Directory -Force | Out-Null
    $tmp = Join-Path $env:TEMP ("idm-next-" + [guid]::NewGuid().ToString("N") + ".zip")
    $extract = Join-Path $env:TEMP ("idm-next-" + [guid]::NewGuid().ToString("N"))
    try {
        Download-File $Url $tmp
        New-Item $extract -ItemType Directory -Force | Out-Null
        Expand-Archive -Path $tmp -DestinationPath $extract -Force
        $found = Get-ChildItem $extract -Recurse -Filter $ExeName | Select-Object -First 1
        if (-not $found) { throw "压缩包中未找到 $ExeName" }
        foreach ($file in Get-ChildItem $found.DirectoryName -File) {
            Copy-Item $file.FullName -Destination $DestDir -Force
        }
    }
    finally {
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
        Remove-Item $extract -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Get-PeImports {
    param([string]$File, [string]$Objdump)
    $output = & $Objdump -p $File 2>$null
    if ($LASTEXITCODE -ne 0) { return @() }
    return @(
        $output |
            Select-String -Pattern '^\s*DLL Name:\s*(.+?)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } |
            Where-Object { $_ } |
            Sort-Object -Unique
    )
}

function Test-SystemDll {
    param([string]$Name)
    if ($Name -match '^(?i:api-ms-win-|ext-ms-win-)') { return $true }
    $system32 = Join-Path $env:SystemRoot "System32\$Name"
    $syswow64 = Join-Path $env:SystemRoot "SysWOW64\$Name"
    return (Test-Path $system32) -or (Test-Path $syswow64)
}

function Add-RuntimeDependencyClosure {
    param(
        [string]$DistDir,
        [string]$ResolvedQtBin,
        [string]$Objdump
    )

    $searchDirs = New-Object System.Collections.Generic.List[string]
    foreach ($dir in @(
        $Build,
        $ResolvedQtBin,
        (Split-Path $Objdump -Parent),
        (Join-Path $Root "third_party\libcurl\bin")
    )) {
        if ($dir -and (Test-Path $dir) -and -not $searchDirs.Contains($dir)) { $searchDirs.Add($dir) }
    }
    foreach ($dir in ($env:PATH -split ';')) {
        if ($dir -and (Test-Path $dir) -and -not $searchDirs.Contains($dir)) { $searchDirs.Add($dir) }
    }

    $queue = New-Object System.Collections.Generic.Queue[string]
    $seenFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    $resolvedNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

    Get-ChildItem $DistDir -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') } | ForEach-Object {
        $queue.Enqueue($_.FullName)
        [void]$resolvedNames.Add($_.Name)
    }

    $copied = 0
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        if (-not $seenFiles.Add($file)) { continue }

        foreach ($dependency in (Get-PeImports $file $Objdump)) {
            if ($resolvedNames.Contains($dependency) -or (Test-SystemDll $dependency)) { continue }

            $source = $null
            foreach ($dir in $searchDirs) {
                $candidate = Join-Path $dir $dependency
                if (Test-Path $candidate) {
                    $source = (Resolve-Path $candidate).Path
                    break
                }
            }

            if (-not $source) {
                throw "未解析的运行时依赖：$dependency（由 $([IO.Path]::GetFileName($file)) 引入）"
            }

            $destination = Join-Path $DistDir $dependency
            Copy-Item $source $destination -Force
            [void]$resolvedNames.Add($dependency)
            $queue.Enqueue($destination)
            $copied++
            Write-Host "运行时依赖: $dependency"
        }
    }

    Write-Host "运行时依赖闭包完成：新增 $copied 个 DLL"
}

$Version = Get-ProjectVersion
$QtBin = Resolve-QtBin
$WindeployQt = Join-Path $QtBin "windeployqt.exe"
$Objdump = Resolve-Objdump $QtBin
Write-Host "IDM Next $Version"
Write-Host "Qt bin: $QtBin"
Write-Host "objdump: $Objdump"

if (-not (Test-Path $Build)) { throw "build/ 不存在，请先完成 CMake 构建" }
$MainExe = Join-Path $Build "idm-next.exe"
$HostExe = Join-Path $Build "idm-next-host.exe"
if (-not (Test-Path $MainExe)) { throw "缺少 $MainExe" }
if (-not (Test-Path $HostExe)) { throw "缺少 $HostExe" }

Remove-Item $Dist -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $OutDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item $Dist -ItemType Directory -Force | Out-Null
New-Item $OutDir -ItemType Directory -Force | Out-Null

Copy-Item $MainExe $Dist
Copy-Item $HostExe $Dist

# Qt runtime。显式请求 compiler runtime；之后仍用 PE 依赖闭包做最终兜底。
& $WindeployQt --release --compiler-runtime --no-translations (Join-Path $Dist "idm-next.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt(main) 失败" }
& $WindeployQt --release --compiler-runtime --no-translations (Join-Path $Dist "idm-next-host.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt(host) 失败" }

$SqlDir = Join-Path $Dist "sqldrivers"
if (Test-Path $SqlDir) {
    Get-ChildItem $SqlDir -Filter "qsql*.dll" | Where-Object { $_.Name -ne "qsqlite.dll" } | Remove-Item -Force
}
if (-not (Test-Path (Join-Path $SqlDir "qsqlite.dll"))) {
    throw "发行目录缺少 qsqlite.dll"
}

# 浏览器扩展随应用发行。native-messaging-host 目录只保留正式注册文件，
# 排除开发时同步的 EXE/DLL、Python fallback、测试脚本和 Qt 插件，避免重复与污染。
$ExtensionSource = Join-Path $Root "browser-extension"
if (Test-Path $ExtensionSource) {
    $ExtensionDest = Join-Path $Dist "browser-extension"
    New-Item $ExtensionDest -ItemType Directory -Force | Out-Null
    Copy-Item (Join-Path $ExtensionSource "*") $ExtensionDest -Recurse -Force

    $NativeHostDest = Join-Path $ExtensionDest "native-messaging-host"
    if (Test-Path $NativeHostDest) {
        $NativeHostKeep = @("com.tencent.idm_next.json", "install_host.ps1")
        Get-ChildItem $NativeHostDest -Force | Where-Object {
            $_.Name -notin $NativeHostKeep
        } | Remove-Item -Recurse -Force

        foreach ($requiredHostFile in $NativeHostKeep) {
            if (-not (Test-Path (Join-Path $NativeHostDest $requiredHostFile))) {
                throw "浏览器宿主注册文件缺失：$requiredHostFile"
            }
        }
    } else {
        throw "浏览器扩展缺少 native-messaging-host 注册目录"
    }
}

# 补齐 MinGW / libcurl / OpenSSL 等非 Qt DLL，并递归验证依赖闭包。
Add-RuntimeDependencyClosure -DistDir $Dist -ResolvedQtBin $QtBin -Objdump $Objdump

if (-not $SkipTools) {
    Ensure-ZipTool "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip" (Join-Path $Dist "aria2") "aria2c.exe"
    Ensure-ZipTool "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip" (Join-Path $Dist "ffmpeg") "ffmpeg.exe"

    $YtDir = Join-Path $Dist "yt-dlp"
    New-Item $YtDir -ItemType Directory -Force | Out-Null
    $YtExe = Join-Path $YtDir "yt-dlp.exe"
    if (-not (Test-Path $YtExe)) {
        Download-File "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe" $YtExe
    }

    foreach ($requiredTool in @(
        (Join-Path $Dist "aria2\aria2c.exe"),
        (Join-Path $Dist "ffmpeg\ffmpeg.exe"),
        (Join-Path $Dist "yt-dlp\yt-dlp.exe")
    )) {
        if (-not (Test-Path $requiredTool)) { throw "发行依赖缺失：$requiredTool" }
    }
}

$PortableName = "idm-next-$Version-win64-portable.zip"
$PortablePath = Join-Path $OutDir $PortableName
Compress-Archive -Path (Join-Path $Dist "*") -DestinationPath $PortablePath -CompressionLevel Optimal
$PortableHash = (Get-FileHash $PortablePath -Algorithm SHA256).Hash.ToLowerInvariant()
"$PortableHash  $PortableName" | Set-Content (Join-Path $OutDir "SHA256SUMS.txt") -Encoding ASCII
Write-Host "便携包：$PortablePath"

$Makensis = Find-Executable "makensis.exe" @(
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe"
)
if ($Makensis) {
    & $Makensis "/DAPPVERSION=$Version" "/DOUTDIR=$OutDir" (Join-Path $Root "installer\idm-next.nsi")
    if ($LASTEXITCODE -ne 0) { throw "makensis 失败" }
} else {
    Write-Warning "未找到 NSIS：便携包已生成，但不会生成 setup.exe。"
}

Write-Host "发行产物目录：$OutDir"
Get-ChildItem $OutDir | Select-Object Name, Length
