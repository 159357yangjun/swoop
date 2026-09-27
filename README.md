# IDM Next

全协议 Windows 下载平台：**Qt 6 + C++17 GUI + libcurl HTTP 引擎 + aria2 / yt-dlp / FFmpeg 后端**。

> 本 README 对应仓库的 `master` 分支。轻量原生 Win32/C 项目 **Swoop** 位于 `main` 分支；两者是不同产品方向，分别构建、测试和发布。

## 定位

IDM Next 面向完整桌面下载管理体验，支持 HTTP/HTTPS、FTP、BT/磁力、视频站点与 HLS，并提供浏览器扩展、命令行、SQLite 历史记录和 Windows 安装包。

与 `main` 分支的 Swoop 不同：

- Swoop：极轻量、纯 Win32/C、便携优先。
- IDM Next：完整功能、Qt GUI、多协议、安装版 + 便携版。

## 技术栈

| 层 | 技术 |
|---|---|
| GUI | Qt 6 Widgets / Charts（C++17） |
| HTTP/HTTPS | libcurl + 现有 C 下载核心 |
| FTP / BT / 磁力 | aria2c JSON-RPC 后端 |
| 视频站点 | yt-dlp |
| HLS / 媒体处理 | 内置 HLS 逻辑 + FFmpeg |
| 持久化 | SQLite / Qt6::Sql |
| 浏览器扩展 | Chrome / Edge Manifest V3 |
| Native Messaging | `idm-next-host.exe` |
| 构建 | CMake + Ninja / MinGW-w64 |
| Windows 部署 | windeployqt |
| 安装器 | NSIS |

## 当前状态

现有代码已经覆盖：

- 分段 HTTP 下载
- 全局限速
- 队列与调度
- 代理
- 校验
- 托盘
- SQLite 历史
- 浏览器扩展
- Native Messaging Host
- BT / 磁力
- FTP
- HLS
- yt-dlp 视频后端
- FFmpeg 后端
- Windows 安装器

详细阶段记录见 [REFACTOR_PLAN.md](REFACTOR_PLAN.md) 与 `开发文档.md`。

## 从源码构建

需要：

- Windows x64
- Qt 6 MinGW 版本（Core / Widgets / Network / Charts / Sql）
- MinGW-w64 GCC
- CMake
- Ninja（推荐）
- libcurl（能被 CMake 找到即可；否则回退 `third_party/libcurl`）

示例：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

主要产物：

```text
build/idm-next.exe
build/idm-next-host.exe
```

`idm-next.exe` 是 GUI 主程序；`idm-next-host.exe` 是 Chrome/Edge Native Messaging 桥接进程。

## Windows 打包

打包脚本不会再绑定固定的 `C:\Qt\...` 路径。Qt 可通过以下方式定位：

1. `-QtBin <path>` 参数
2. `QT_BIN` 环境变量
3. 当前 PATH 中的 `windeployqt.exe`
4. 自动搜索 `C:\Qt` / `D:\Qt`

构建完成后执行：

```powershell
powershell -ExecutionPolicy Bypass -File installer/package.ps1
```

如果需要明确指定 Qt：

```powershell
powershell -File installer/package.ps1 -QtBin D:\Qt\6.x.x\mingw_64\bin
```

脚本会：

```text
检查 idm-next.exe / idm-next-host.exe
→ windeployqt 收集 Qt Runtime
→ 保留 SQLite 驱动
→ 收录浏览器扩展
→ 准备 aria2c / FFmpeg / yt-dlp
→ 生成 portable ZIP
→ 计算 SHA256
→ 如果安装了 NSIS，再生成 setup.exe
```

输出目录：

```text
installer/out/
  idm-next-<版本>-win64-portable.zip
  idm-next-<版本>-setup-x64.exe
  SHA256SUMS.txt
```

版本号统一读取 `CMakeLists.txt` 中的 `project(idm-next VERSION x.y.z)`，NSIS 不再单独维护一份版本号。

## CI

`master` 和相关开发分支会在 GitHub Actions 的干净 Windows 环境中执行：

```text
安装 MSYS2 / Qt
→ 动态探测 Qt 路径
→ CMake Configure
→ 编译主程序 + Host
→ 打便携包
→ SHA256 校验
→ 上传 CI artifact
```

因此每次 PR 都可以直接从 Actions 下载测试包，而不是只能在开发机上验证。

## 正式发布

IDM Next 使用独立 tag 命名空间：

```text
idm-next-v<版本>
```

例如版本 `0.1.1`：

```bash
git tag idm-next-v0.1.1
git push origin idm-next-v0.1.1
```

Release Actions 会自动完成：

```text
构建
→ windeployqt
→ 下载并校验运行辅助工具
→ 生成 portable ZIP
→ 安装 NSIS
→ 生成 setup EXE
→ SHA256
→ GitHub Release
```

仓库 `main` 分支的 Swoop 使用 `swoop-v*`，不要使用通用 `v*`，避免两个产品发布流程互相干扰。

## 目录结构

```text
src/
  core/        下载核心 + Qt wrapper
  protocols/   HTTP / 视频 / HLS / aria2 / FFmpeg / 插件适配
  gui/         Qt GUI
  model/       任务列表与筛选模型
  app/         应用编排与控制器
  storage/     历史与解析
  web/         Web 控制服务
  browser/     浏览器通信
  host/        Native Messaging Host
  cli/         命令行模式
  utils/       设置、路径、日志等
browser-extension/
installer/
  package.ps1
  idm-next.nsi
```

## 发布注意事项

- 正式发布包应同时包含 portable ZIP 和 setup EXE。
- `qsqlite.dll` 缺失会直接让打包失败，避免产生看似能启动但历史数据库不可用的残缺包。
- `aria2c`、FFmpeg、yt-dlp 在正式打包时缺失也会失败。
- 当前没有代码签名，因此 Windows SmartScreen 仍可能提示未知发布者。
- 浏览器扩展 / Native Messaging 的最终安装体验仍是后续继续优化重点。
