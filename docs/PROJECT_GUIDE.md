# IDM Next 项目指南

> 面向开发者的「上手即懂」文档：技术栈、目录职责、构建运行、浏览器扩展部署全链路、
> 已知坑清单、核心功能模块速查、调试技巧。
> 配套参考：[fdm_compare.md](fdm_compare.md)（FDM 对标分析报告）。

---

## 1. 技术栈（以代码为准；README 标题句已修正为 WinHTTP + aria2c）

| 层 | 技术 | 说明 |
|---|---|---|
| GUI | Qt 6 Widgets (C++17) | 非 QML；主题用 `resources/qss/{light,dark}.qss` |
| HTTP/HTTPS | **自研纯 C 引擎**（WinHTTP，系统库） | 见 `src/core/network.c`，**不依赖 libcurl** |
| FTP / BT / 磁力 | **aria2c 单守护进程**（JSON-RPC 托管） | 见 `src/protocols/aria2_daemon.cpp`；**不依赖 libtorrent** |
| 视频流 | yt-dlp（＋ 内置 HLS 解析 + AES-128 纯 C++ 解密） | 见 `src/protocols/video_*`、`hls_downloader.cpp` |
| 持久化 | SQLite（Qt6::Sql 自带 qsqlite 驱动） | 下载历史 `#46` |
| 日志 | 自研 header-only logger | `src/utils/logger.h`，无需 .cpp |
| 构建 | CMake（MinGW-w64，Qt6 自编译） | `find_package(Qt6 ...)`，无 vcpkg 依赖 |
| 浏览器扩展 | Chrome/Edge MV3（纯 JavaScript） | 见 `browser-extension/` |

**平台**：当前仅 Windows 构建可用（manifest 含 Win10/11 `supportedOS`，WinHTTP/aria2 均为 Windows 友好）。
Qt6 本身跨平台，但纯 C 引擎与 Native Messaging Host 做了 Windows 特化。

---

## 2. 目录结构（每个文件夹的职责）

```
idm-next/
├── CMakeLists.txt              # 构建脚本：双 exe（idm-next / idm-next-host）、host POST_BUILD 自动部署
├── README.md                   # 项目说明（标题句 libcurl+libtorrent 已修正为 WinHTTP + aria2c）
├── REFACTOR_PLAN.md            # 重构路线图（#27–#53 全部完成）
├── 开发文档.md                  # 详细开发笔记
├── src/
│   ├── core/                   # ★ 下载引擎核心（纯 C 复用，跨平台）
│   │   ├── network.c/.h         #   WinHTTP 网络层：协议/Range/重定向/代理/HTTP-2 协商
│   │   ├── download_core.c/.h   #   分段分片调度、断点续传、限速、Content-Disposition 嗅探
│   │   └── download_manager.cpp #   C 引擎的 C++ Wrapper（DownloadManager，Qt 信号回 GUI）
│   ├── storage/                # 持久化 + HTML 解析（纯 C 复用）
│   │   ├── storage.c            #   SQLite 封装（历史库）
│   │   ├── html_parser.c        #   站点抓取 HTML 解析
│   │   └── history_store.cpp    #   历史读取 C++ 封装
│   ├── protocols/             # 协议适配层（IDownloader 接口 + 各协议实现）
│   │   ├── idownloader.cpp      #   下载器统一接口
│   │   ├── http_downloader.cpp  #   HTTP 协议实现（转发到 core 引擎）
│   │   ├── video_downloader.cpp / video_backend.cpp / hls_downloader.cpp  # 视频/yt-dlp/HLS
│   │   ├── torrent_downloader.cpp / torrent_backend.cpp  # BT/磁力（复用 aria2）
│   │   ├── aria2_daemon.cpp     #   ★ aria2c 单守护进程（懒启动/崩溃自愈/空闲自退）
│   │   ├── ffmpeg_backend.cpp   #   ffmpeg 自动下载与合并
│   │   ├── plugin_manager.cpp / protocol_factory.cpp  # 站点解析插件（热更 #35）
│   ├── gui/                   # Qt GUI 层（所有窗口/对话框/模型/委托）
│   │   ├── main_window.cpp/.h   #   主窗口、工具栏、托盘、设置注入入口
│   │   ├── settings_dialog.cpp  #   设置对话框（常规/连接/代理/.../远程）
│   │   ├── task_list_model.cpp / progress_delegate.cpp / speed_chart_widget.cpp
│   │   ├── new_task_dialog / batch_import_dialog / schedule_dialog / hash_verify_dialog
│   │   ├── queue_manager*.cpp   #   队列管理（并发上限、自动建队列）
│   │   ├── site_explorer_dialog.cpp  # 站点登录/资源抓取
│   │   ├── web_server.cpp        #   Web 远程管理（默认关）
│   │   └── glass_effect.cpp      #   玻璃拟态（主窗口已停用，文件保留备用）
│   ├── host/                  # ★ 浏览器扩展 Native Messaging Host（idm-next-host.exe 源码）
│   │   └── idm_next_host.cpp    #   桥接：扩展↔GUI（QLocalServer idm-next-ipc）
│   ├── cli/                   # 命令行模式（--cli 经 AllocConsole；单实例时 IPC 转发 GUI）
│   │   └── cli_app.cpp
│   └── utils/                 # 工具
│       ├── settings.cpp/.h     #   ★ 全局设置（ini 持久化、流量档位、HTTP/2 开关）
│       └── app_paths.cpp/.h    #   ★ 便携模式路径重定向（AppPaths）
├── resources/
│   ├── qss/                   # light.qss / dark.qss 主题
│   ├── plugins/               # 站点解析插件（热更存放）
│   └── resources.qrc          # 图标/样式资源（编译进 exe）
├── browser-extension/        # ★ 浏览器扩展（MV3，纯 JS）
│   ├── manifest.json          #   扩展清单（name: "IDM Next 资源嗅探器"，v0.3.0）
│   ├── background.js           #   后台：Native Messaging 客户端 + 动作分发
│   ├── content.js             #   内容脚本：嗅探资源、注入「下载确认」卡片、画质下拉
│   ├── popup.html/js/css       #   弹窗：资源列表、过滤、批量/入队、画质选择
│   ├── icons/                 #   扩展图标
│   └── native-messaging-host/ #   ★ host 运行时目录（CMake 自动同步）
│       ├── idm-next-host.exe   #     由 CMake POST_BUILD 自动拷贝（勿手改）
│       ├── com.tencent.idm_next.json  # Native Messaging 清单（path 指同目录 exe）
│       ├── install_host.ps1    #     注册表写入脚本（HKCU NativeMessagingHosts）
│       └── *.dll               #     Qt6Core/Qt6Network + MinGW 运行时（windeployqt 部署）
├── installer/
│   ├── idm-next.nsi           # NSIS 脚本
│   ├── package.ps1            #   打包脚本（Release → windeployqt → NSIS）
│   └── dist/ + idm-next-setup-x64.exe  # 已生成安装包
├── docs/
│   ├── fdm_compare.md         # FDM 对标分析
│   └── PROJECT_GUIDE.md       # 本文件
└── build/                     # CMake 生成目录（不入库）
```

> **注意**：`src/core`、`src/storage` 的 `.c` 源码是从原 MFC 项目复用的纯 C 代码，
> 由 `download_manager.cpp` / `history_store.cpp` 以 C++ 封装后接入 Qt。改动引擎层请保持 C 语言兼容。

---

## 3. 构建与运行

### 3.1 前置条件
- Windows 10/11 + MinGW-w64（GCC）或 MSVC
- Qt 6（Core / Widgets / Network / Charts / Sql），CMake 3.16+
- 无需 vcpkg；HTTP 走系统 WinHTTP，BT/FTP 走 aria2c（缺失时程序自动下载，见 §6）

### 3.2 配置 + 构建
```bash
cd idm-next
cmake -B build -S .            # 生成构建系统（Qt6 由 PATH/CMAKE_PREFIX_PATH 找到）
cmake --build build --config Release
```
产物：
- `build/idm-next.exe`          主程序（GUI，`WIN32_EXECUTABLE`，双击无黑窗）
- `build/idm-next-host.exe`     浏览器扩展宿主（POST_BUILD 已自动同步到 `browser-extension/native-messaging-host/`）

### 3.3 运行
- 双击 `idm-next.exe` 启动 GUI。
- CLI 模式：`idm-next.exe --cli "<url>"`（单实例 GUI 已运行时经 IPC 转发）。
- `--cli` 下 `network.c` 会在 stderr 打印网络层信息，包括 **HTTP/2 协商结果**（见 §6）。

### 3.4 打包发布
```bash
cd installer
powershell -ExecutionPolicy Bypass -File package.ps1
```
→ `windeployqt` 收集 Qt 运行时 + NSIS 生成 `idm-next-setup-x64.exe`。

---

## 4. 浏览器扩展部署（完整链路 + 坑）

这是最容易卡住新人的部分，务必按顺序走。

### 4.1 架构链路
```
浏览器扩展 (MV3)
   │  Native Messaging（名称 com.tencent.idm_next）
   ▼
idm-next-host.exe  (browser-extension/native-messaging-host/)
   │  QLocalServer 命名管道 "idm-next-ipc"
   ▼
主程序 GUI  onIpcConnection(command=add)  →  internalAddTask
```
Host 支持的 action：`add_download`（透传 url/filename/saveDir/queue/**format**）、
`batch_add`、`list_formats`（yt-dlp -J 解析画质）、`ping`、`get_status`。

### 4.2 三步部署
1. **加载扩展**：Chrome/Edge 打开 `chrome://extensions` → 开发者模式 →
   「加载已解压的扩展程序」→ 选择 `browser-extension/` **这个目录**
   （不要选 `native-messaging-host/` 子目录，否则报目录无效）。
2. **注册 Native Messaging Host**：以**普通用户**运行一次
   `browser-extension/native-messaging-host/install_host.ps1`，
   它向 `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.tencent.idm_next`
   写入指向 `com.tencent.idm_next.json` 的路径。
   - `com.tencent.idm_next.json` 的 `path` 必须指向**同目录**的 `idm-next-host.exe`
     （不要指向 `build/` 下的任何路径）。
3. **确认 host 运行时齐全**：`native-messaging-host/` 下应有
   `idm-next-host.exe` + `Qt6Core.dll` + `Qt6Network.dll` + `libgcc_s_seh-1.dll` +
   `libstdc++-6.dll` + `libwinpthread-1.dll`。
   - 这些由 CMake 的 `idm-next-host` POST_BUILD 钩子（`copy_if_different` + `windeployqt`）
     **每次编译后自动同步**，正常无需手动复制。
   - 若你手动改了 host 源码，只需 `cmake --build build`，钩子会自动重部署。

### 4.3 冒烟测试
扩展加载后，在 `native-messaging-host/` 目录运行 `test_host.py`（或 `ping` 动作），
应收到 `idm-next-host` 回包；GUI 运行时 `ping` 返回含版本号与「GUI 在线」状态。

### 4.4 常见坑（部署相关，详见 §5）
- host 启动失败：缺 `Qt6Core.dll` 等运行时，或 `manifest path` 错指 `build/` → 重跑 CMake 构建 + 重跑 `install_host.ps1`。
- 扩展不显示：选错了目录（选了 `native-messaging-host` 而非 `browser-extension`）。
- 改 JS 后不生效：需在 `chrome://extensions` 点「重新加载」扩展（开发者模式）。
- 改 host C++ 后不生效：需 `cmake --build build` 让 POST_BUILD 重部署 exe。

---

## 5. 已知坑清单（开发者必读）

| # | 坑 | 现状 / 处理 |
|---|---|---|
| 1 | ~~README 标题句过时~~（已修复） | 第 3 行原「Qt 6 + libcurl + libtorrent」已改为「Qt 6 + 自研纯 C 引擎(WinHTTP) + aria2c」；技术栈表（第 10–19 行）始终正确。 |
| 2 | **历史空目录** | 早前存在 `src/browser/`、`resources/icons/`、`resources/translations/`、`tests/` 空目录，已于清理轮次删除。新增代码勿放回这些路径。 |
| 3 | **build/ 生成物** | `build/` 为 CMake 产物，**不入库**；`idm-next-setup-x64.exe` 等安装包在 `installer/`（可入库但体积大）。 |
| 4 | **双 exe** | 项目产出两个可执行文件：`idm-next.exe`（GUI）与 `idm-next-host.exe`（扩展宿主）。调试扩展问题时两个都要关注。 |
| 5 | **扩展叫什么** | 名称「**IDM Next 资源嗅探器**」，Native Messaging ID `com.tencent.idm_next`。host 的 `applicationName` 必须与 GUI 的 `AppPaths::dataDir()` 一致（均用 "IDM Next"），否则数据目录错位。 |
| 6 | **host 部署链路** | 见 §4：缺运行时 / manifest path 错指 build / 选错加载目录 / 改源码后未重部署，是四类高频失败。 |
| 7 | **FDM 对标已落地优化** | 见 §6：WinHTTP 并发 2→100、Content-Disposition 文件名嗅探、流量档位、HTTP/2 协商。 |

---

## 6. 核心功能模块速查

### 6.1 流量档位（对标 FDM 工具栏流量模式）
- 设置字段：`Settings::trafficMode`（0=自动 / 1=轻量 / 2=中等 / 3=重量 / -1=自定义）。
- UI：主窗口工具栏 `QComboBox`（自动/轻量/中等/重量/自定义）+ 托盘「流量档位」子菜单（互斥勾选）。
- 预设限速：自动=0、轻量=512、中等=2048、重量=4096 KB/s。
- 实现：`applyTrafficMode(mode)` 统一入口 → 写设置 → 实时下发纯 C 引擎 `m_manager->setMaxSpeed`
  + aria2 `TorrentDownloader::setGlobalSpeedLimit` → 落盘 → 同步工具栏与托盘。
- 在「设置」里手动改限速值 → 自动记为「自定义」档位。

### 6.2 HTTP/2 协商（HTTPS / ALPN）
- 引擎层：`src/core/network.c`
  - 全局开关 `g_http2_enabled`（默认开）；`apply_http2_to_session()` 在 WinHTTP session 上
    设 `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`（启用发 HTTP/2 标志，关闭发 0）。
  - 老系统（< Win10 1607）不支持则自动回退 HTTP/1.1。
  - `do_http_request` 在 HEAD 探测后查询 `WINHTTP_OPTION_HTTP_PROTOCOL_USED`，
    协商成功时 `fprintf("[net] HTTP/2 协商成功: <host>")`（仅 HEAD，避免分片下载刷屏）。
  - 运行时开关：`network_set_http2_enabled(int)`（extern "C"），启动与设置变更即时应用。
- 设置项：`Settings::http2Enabled`（默认 true），「连接」页复选框；`accept` 内实时应用。
- 注意：**HTTP/2 仅在 HTTPS（ALPN）协商**；明文 HTTP 自动回退 1.1。**验证**：`--cli` 模式下
  对支持 HTTP/2 的 HTTPS 站点发起下载，看 stderr 是否打印「HTTP/2 协商成功」。

### 6.3 aria2 单守护进程（BT / FTP / 磁力）
- `src/protocols/aria2_daemon.cpp`（`aria2_daemon` 单例）
  - 取代旧版「每任务一进程」：1 进程 + 1 QNetworkAccessManager + 1 轮询定时器。
  - 全局限速经 `--max-overall-download-limit` + `changeGlobalOption` 聚合。
  - 懒启动：仅 BT/FTP/磁力触发；纯 HTTP 不拉起 aria2c。
  - 崩溃自愈：进程消失后清空句柄允许重启；空闲 15s 自退，下次任务即时拉起。
  - RPC `id` 编码 `tag|reqId`，回包 `lastIndexOf("|")` 还原；`getVersion` 能力校验。

### 6.4 便携模式
- `src/utils/app_paths.cpp`（`AppPaths`）：若 exe 同目录存在 `idm-next.portable` 或 `portable`，
  则 ini / 历史 DB / helpers（aria2c·yt-dlp·ffmpeg）/ 插件全部重定向到 exe 目录，
  U 盘即插即跑。

### 6.5 视频画质选择
- 扩展 `listFormats` 跑 `yt-dlp -J` 解析格式 → 确认卡片/批量面板下拉（1080p/720p/仅音频…）
  → 选中的 `fmt` 串经 host → GUI 落到 yt-dlp `-f`。
- 后端：`video_backend.cpp`（yt-dlp）/ `hls_downloader.cpp`（内置 HLS + AES-128 纯 C++ 解密）。

### 6.6 helpers 自动下载
- aria2c / yt-dlp / ffmpeg 缺失时，程序按 `Settings::autoDownloadAria2/YtDlp/Ffmpeg`（默认开）
  自动下载到 `AppPaths` 对应目录，开箱即用。

---

## 7. 调试与日志

- **引擎日志**：纯 C 层用 `fprintf(stderr, ...)`（如 HTTP/2 协商结果）。`--cli` 模式下可见；
  GUI 模式建议用 `--cli` 跑单任务验证网络细节。
- **应用日志**：`src/utils/logger.h`（header-only），级别见 `Log::info/warn/error`。
- **单实例 + IPC**：已运行 GUI 时，第二个 `idm-next`（含 `--cli`）经 `QLocalServer idm-next-ipc`
  转发命令，不会起第二个窗口。
- **HTTP/2 验证**：`idm-next.exe --cli "https://<支持http2的站点>/file"` → 看 stderr。
- **扩展联调**：`native-messaging-host/test_host.py` 单独测 host；浏览器端开 DevTools 看扩展 console。
- **崩溃定位**：纯 C 引擎问题可用 x64dbg（`D:\x64dbg\release\x96dbg.exe`）附加 `idm-next.exe`。

---

## 8. 下一步可选项（截至 2026-08-30 状态）

- [ ] 把「流量档位 / HTTP/2」等 UI 状态在任务详情中可视化（需给 `NetworkProbe`/`NetResponse` 加 `http_version` 字段）。
- [ ] 跨平台构建验证（Linux/macOS 需替换 WinHTTP/aria2 的 Windows 特化部分）。
- [ ] Win7（Qt6 不支持）、x86 32 位、ARM64 等新增构建目标（需对应 Qt 工具链 + helpers 架构）。
