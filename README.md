# IDM Next

全协议下载平台 — Qt 6 + 自研纯 C 引擎(WinHTTP) + aria2c 跨平台重构版

对标 IDM 的下载管理器，支持 HTTP/HTTPS、FTP/SFTP、BT/磁力、视频流解析，
内置浏览器扩展嗅探和命令行模式。下载引擎层复用自原 MFC 项目的纯 C 代码。

## 技术栈

| 层 | 技术 |
|---|---|
| GUI | Qt 6 Widgets (C++17) |
| HTTP/HTTPS | 自研纯 C 引擎（WinHTTP，系统库，不依赖 libcurl） |
| FTP/BT/磁力 | aria2c（JSON-RPC 托管，torrent_backend/torrent_downloader） |
| 视频流 | yt-dlp（＋ 内置 HLS 解析 + AES-128 解密） |
| 持久化 | SQLite（Qt6::Sql 自带 qsqlite 驱动） |
| 日志 | 自研 header-only logger（src/utils/logger.h） |
| 构建 | CMake + vcpkg |
| 浏览器扩展 | Chrome/Edge MV3（纯 JavaScript，非 TypeScript） |

## 开发状态

> 注：以下阶段实际均已落地（#27–#53 全部完成），包括分段 HTTP 引擎、全局限速、yt-dlp 视频后端、浏览器扩展、站点抓取、队列、代理、校验、托盘、SQLite 历史、NSIS 安装包、BT/磁力、HLS、FTP 等。详细进度见 [REFACTOR_PLAN.md](REFACTOR_PLAN.md) 与 `开发文档.md`。

- [x] **阶段1** 基础骨架：CMake + Qt 空窗口 + 引擎 wrapper
- [x] **阶段2** HTTP 下载核心：GUI 任务列表 + 自研纯 C 引擎（WinHTTP）
- [x] **阶段3** 多协议：FTP + BT/磁力（aria2c 托管）+ CLI
- [x] **阶段4** 浏览器扩展 + 视频流解析（yt-dlp / HLS）
- [x] **阶段5** 打磨发布：UI（QSS 主题）+ SQLite 历史 + NSIS 安装包

详见 [REFACTOR_PLAN.md](REFACTOR_PLAN.md)

## 构建

```bash
# 前置：Qt 6（MinGW-w64 构建）+ CMake，无需 vcpkg。
# 网络层用 Windows 系统库 WinHTTP（自动链接，不依赖 libcurl）。

# 配置 + 构建
cmake -B build -S .
cmake --build build
```

## 目录结构

```
src/
  core/        下载引擎（纯 C 复用）+ Qt wrapper
  protocols/   协议适配层（IDownloader 接口 + 各协议实现）
  gui/         Qt GUI 层
  storage/     持久化 + HTML 解析（纯 C 复用）
  browser/     浏览器通信
  cli/         命令行模式
  utils/       工具（日志等）
```
