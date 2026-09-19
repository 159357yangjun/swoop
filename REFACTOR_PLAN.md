# IDM 下载器重构方案（idm-next）

> 全协议下载平台 — Qt 6 + libcurl + libtorrent，跨平台重写自原 MFC 项目。

## 当前进展（2026-08-12 更新）

| 阶段 | 状态 | 说明 |
|---|---|---|
| 阶段 1：基础骨架 | ✅ 完成 | CMake 工程、Qt 空窗口、引擎 C→Qt wrapper、API 调通 |
| 阶段 2：HTTP 下载核心 | ✅ 核心完成 | GUI 闭环 + 引擎桥接 + 持久化 + 分类过滤 + restart 按钮已全部打通 |
| 阶段 2：libcurl 替换 | ⚠️ 务实推迟 | 详见下方「务实调整」，当前保持 WinHTTP |
| 阶段 3：多协议/CLI | 🟢 部分完成 | CLI 模式 + 协议工厂 + 设置/限速/批量操作已实现；FTP/BT 待装 libcurl/libtorrent |
| 阶段 4：浏览器扩展 + 视频流 | 🟢 部分完成 | MV3 扩展 + Native Messaging + VideoDownloader(yt-dlp) + 速度曲线图(Qt Charts) 已实现；待集成测试 |
| 阶段 5：打磨发布 | 🟢 大部分完成 | QSS 主题 + 系统托盘 + 剪贴板监听 + 拖放 + 批量导入 + 定时下载 + 文件校验 已实现；SQLite/打包 待实现 |

### 务实调整：libcurl 替换
原计划阶段 2 用 libcurl 替换 WinHTTP。现实约束：开发机尚未安装 vcpkg/libcurl，
而 `network.c` 的 WinHTTP 实现在 Windows 上可正常编译运行，故阶段 2 先保持 WinHTTP，
让下载流程**完整跑通**。

后续切换方式（零改动 GUI/引擎层）：
1. `vcpkg install libcurl` 后用 `network_curl.c` 实现同样接口
   （`network_download_range` / `network_get_file_size` / `network_supports_range`）；
2. 在 `CMakeLists.txt` 把 `src/core/network.c` 换成 `src/core/network_curl.c`，
   并启用 `find_package(CURL)` + `CURL::libcurl` 链接（注释已标明）。

## 技术栈

| 层 | 旧（MFC 项目） | 新 | 备注 |
|---|---|---|---|
| GUI | MFC (C++) | **Qt 6 (C++)** | 信号槽 / QSS / 跨平台 |
| HTTP 网络 | WinHTTP | **WinHTTP（暂）→ libcurl（后续）** | 接口不变，可热切换 |
| BT 下载 | 无 | libtorrent-rasterbar（阶段3） | |
| 视频解析 | 无 | **yt-dlp（阶段4 ✅）** | VideoDownloader 封装 QProcess |
| 浏览器扩展 | 无 | **MV3 扩展（阶段4 ✅）** | webRequest 嗅探 + Native Messaging |
| 持久化 | 手写 JSON | 手写 JSON（暂）→ SQLite（阶段5） | |
| 日志 | printf | Log 封装（qDebug 暂，阶段5 换 spdlog） | |
| 构建 | VS2022 + .vcxproj | **CMake + vcpkg** | |

### 核心原则
- **下载引擎零改动复用**：`download_core.c` / `network.c` / `storage.c` 原封不动搬入 `src/core`、`src/storage`
- **只换外壳，不换心脏**：GUI 层全换 Qt 6，引擎加 C++ wrapper（`DownloadManager`）调用
- **协议可插拔**：每种协议实现统一的 `IDownloader` 接口（阶段3 填充）

## 项目结构（当前实际）

```
idm-next/
├── CMakeLists.txt                 # 根构建（已去掉强制 CURL，保留 winhttp）
├── vcpkg.json                     # 依赖清单
├── REFACTOR_PLAN.md               # 本文件
├── README.md
├── browser-extension/             # 浏览器扩展（MV3，独立于 Qt）
│   ├── manifest.json              # MV3 清单
│   ├── background.js              # Service Worker（webRequest 嗅探 + Native Messaging）
│   ├── content.js                 # 内容脚本（DOM 扫描 video/audio/download 链接）
│   ├── popup.html / .css / .js    # 弹窗 UI（展示嗅探资源 + 一键下载）
│   ├── README.md                  # 扩展安装指南
│   ├── icons/                     # 扩展图标（16/48/128px）
│   └── native-messaging-host/
│       ├── com.tencent.idm_next.json  # Native Messaging manifest
│       ├── idm_next_host.py       # Host 脚本（Python，仅标准库）
│       └── idm_next_host.bat      # Windows 启动器
├── resources/
│   ├── resources.qrc              # Qt 资源（qss 样式表）
│   └── qss/{light,dark}.qss       # 明暗主题（✅ 已接入 applyTheme()）
└── src/
    ├── main.cpp                   # 入口：GUI/CLI 双模式 + 引擎统一 init/destroy
    ├── core/                      # 下载引擎（复用）+ wrapper
    │   ├── download_core.c/h      # ← 原封复用（含 pause/restart/load_state/set_callbacks 修复）
    │   ├── network.c/h            # ← 原封复用（WinHTTP）
    │   └── download_manager.{h,cpp}  # Qt C++ wrapper（桥接引擎 + 信号槽 + restart）
    ├── storage/                   # 持久化层（复用）
    │   ├── storage.c/h
    │   └── html_parser.c/h
    ├── gui/                       # Qt GUI 层（全新）
    │   ├── main_window.{h,cpp}    # 主窗口：菜单/工具栏/分类树/任务列表/状态栏/主题切换/restart/双击详情/批量导入/定时下载/拖放/剪贴板
    │   ├── task_list_model.{h,cpp}# 任务列表数据模型（QAbstractTableModel）
    │   ├── new_task_dialog.{h,cpp}# 新建任务对话框（URL/目录/文件名/线程数）
    │   ├── settings_dialog.{h,cpp}# 设置对话框（默认目录/线程数/限速/主题/剪贴板监听）
    │   ├── task_detail_dialog.{h,cpp} # 任务详情对话框（双击弹出，标签页：详情+速度图表+文件校验）
    │   ├── speed_chart_widget.{h,cpp} # 速度曲线图（Qt Charts，实时 60 秒滚动）
    │   ├── batch_import_dialog.{h,cpp}# 批量导入对话框（多行文本/文件导入，统一目录+线程数）
    │   ├── schedule_dialog.{h,cpp}   # 定时下载设置对话框
    │   ├── hash_verify_dialog.{h,cpp}# 文件完整性校验（MD5/SHA-1/SHA-256，QCryptographicHash）
    │   └── category_filter_proxy.{h,cpp} # 按状态过滤的代理模型（含 moc 实现）
    ├── protocols/                 # 协议插件（IDownloader 实现）
    │   ├── idownloader.{h,cpp}    # 统一下载接口（.cpp 供 AUTOMOC 生成 moc）
    │   ├── http_downloader.{h,cpp}# HTTP/HTTPS 适配器（包装 DownloadManager）
    │   ├── video_downloader.{h,cpp} # 视频流适配器（yt-dlp 进程封装，解析进度输出）
    │   └── protocol_factory.{h,cpp}# URL scheme/域名 → 下载器 分发（http/https + 视频网站）
    ├── cli/                       # 命令行模式（直接调引擎，无 GUI 依赖）
    │   └── cli_app.{h,cpp}        # add/list/start/pause/cancel/remove/info/set-limit + 交互
    └── utils/
        ├── logger.h              # 日志封装（header-only inline）
        └── settings.{h,cpp}      # 全局设置（QSettings 持久化 + applyToEngine）
```

## 关键设计

### 引擎 Wrapper（DownloadManager）
- 构造时 `dlmgr_init(nullptr)`，析构时 `dlmgr_destroy()`（引擎为全局单例，全程序仅一次）
- `addTask` → `dlmgr_add`（参数顺序：`url, save_dir, filename, thread_count, progress_cb, progress_ud, complete_cb, complete_ud`）
- `resumeTask` 复用 `dlmgr_start`（引擎无独立 resume API，start 兼具「开始/从暂停恢复」）
- `restartTask` 调用 `dlmgr_restart`（重置分片、删临时文件、重新探测）
- 引擎无 state 回调：在 `startTask/pauseTask/cancelTask/restartTask` 主动 `emit taskStateChanged`，
  `forwardComplete` 内补发 `COMPLETED(3)/FAILED(4)`
- 新增 `rebindTask`：调用引擎 `dlmgr_set_callbacks` 给 `load_state` 恢复的任务补绑 Qt 回调

### 跨线程安全
引擎 worker 线程触发 C 回调 → `emit` 信号 → Qt 自动以 QueuedConnection 投递到 GUI 线程，
槽函数在主线程执行，直接操作模型/UI 安全。

### 持久化
- `MainWindow` 构造末尾：`dlmgr_load_state(tasks.json)` → `refreshFromEngine()` 恢复列表
- `MainWindow` 析构（早于子对象 m_manager 销毁）：`dlmgr_save_state(tasks.json)` 保存队列
- `tasks.json` 位于 `QStandardPaths::AppDataLocation`

### CLI 双模式
- `main.cpp` 根据 `--cli` / `-c` 参数选择启动 `CliApp`（QCoreApplication）或 `MainWindow`（QApplication）
- 进入 CLI 模式时过滤掉 `--cli`/`-c` 标志，只传实际命令和参数给 `CliApp::run()`
- 引擎 `dlmgr_init` / `dlmgr_destroy` 统一在 `main.cpp` 管理（GUI/CLI 共用，避免重复初始化）；`DownloadManager` 不再自行 init/destroy
- CLI 直接用纯 C 引擎 `dlmgr_*`，不依赖 Qt 信号槽；`add` 后轮询 `dlmgr_get_task_info` 实时打印进度

### 全局设置（Settings）
- `Settings`（QSettings 持久化）保存默认下载目录、最大线程数、全局限速(KB/s)、主题
- 启动时 `load()` → `applyToEngine()` 写入引擎 `DownloadConfig` 并即时 `dlmgr_set_speed_limit`
- 设置对话框 `SettingsDialog` 编辑后写回并生效，主题变更即时 `applyTheme()`

### 引擎全局限速（已修复）
- 原 `dlmgr_set_speed_limit` 只写配置变量，未在下载循环节流 → 限速无效
- 修复：新增 100ms 滑动窗口节流器 `throttle_limit()`，在 `chunk_thread` 每小块下载后调用，
  跨分片/跨任务共享全局带宽上限（令牌桶风格）

### QSS 主题（已接入）
- `MainWindow::applyTheme()` 从 Qt 资源系统 `:/qss/{theme}.qss` 加载样式表
- 启动时根据 `Settings::theme()` 自动应用；设置对话框修改后即时刷新
- 亮色/暗色两套完整样式（含菜单/工具栏/表格/按钮/滚动条/进度条等全控件）

### 浏览器扩展架构
- **background.js**（MV3 Service Worker）：通过 `webRequest.onBeforeRequest` 嗅探网络请求，
  按文件扩展名匹配（mp4/mkv/mp3/zip/exe/magnet 等），存入 `chrome.storage.session`
- **content.js**：MutationObserver 监听 DOM 变化，扫描 `<video>`/`<audio>`/`<a download>` 元素
  及直链，上报给 background
- **popup**：展示当前标签页嗅探到的资源列表，点击「下载」通过 `chrome.runtime.sendNativeMessage`
  发送给 Native Messaging Host
- **idm_next_host.py**：接收扩展消息，调用 `idm-next --cli add <url>` 添加下载任务

### VideoDownloader（yt-dlp 封装）
- 通过 `QProcess` 启动 yt-dlp 子进程，传入 URL + 格式选项 + 输出路径
- 正则解析 yt-dlp stdout 进度行（百分比/大小/速度/ETA），转换为 `IDownloader` 信号
- 支持 20+ 视频网站域名识别（YouTube/Bilibili/TikTok/抖音 等）
- `ProtocolFactory` 优先匹配视频网站 URL → VideoDownloader，其余 http/https → HttpDownloader

### 速度曲线图（Qt Charts）
- `SpeedChartWidget`：QChart + QLineSeries，500ms 采样间隔，保留最近 60 秒数据（120 点）
- Y 轴自适应：跟随最大速度动态扩展/收缩，最小 1KB
- 嵌入 TaskDetailDialog 的第二个标签页，仅下载中时喂数据

### 批量导入
- `BatchImportDialog`：多行文本框粘贴 URL + 文件导入（.txt），每行一个链接
- 自动过滤空行和 `#` 注释行，实时统计有效链接数
- 统一设置保存目录和线程数，一键添加所有有效任务

### 定时下载
- `ScheduleDialog` 设置 QDateTimeEdit 开始时间 + 每日重复开关
- MainWindow 维护 `QMap<int, QDateTime> m_scheduledTasks`，QTimer 5 秒轮询
- 到期自动 startTask + 托盘通知；每日重复的任务次日自动重新调度

### 文件完整性校验
- `HashVerifyDialog`：QCryptographicHash 计算 MD5/SHA-1/SHA-256
- 分块读取（4MB/块）+ processEvents 保持 UI 响应，支持大文件
- 输入预期哈希值后自动比对，显示匹配/不匹配结果（绿/红背景）

## 开发阶段

### 阶段 1：基础骨架 ✅
- [x] CMake 工程结构
- [x] Qt 6 空窗口编译运行
- [x] 复制 download_core.c，编写 C++ wrapper
- [x] 验证 dlmgr_add/start API 调通

### 阶段 2：HTTP 下载核心 ✅
- [x] MainWindow 布局（菜单栏 + 工具栏 + 分类树 + 任务列表 + 状态栏）
- [x] TaskListModel（QAbstractTableModel）+ CategoryFilterProxy（按状态过滤）
- [x] NewTaskDialog（URL + 保存路径 + 线程数）
- [ ] libcurl 替换 WinHTTP（network.c 内部实现）— 务实推迟，见上
- [x] 进度 / 速度 / 剩余 实时刷新（引擎 progress 回调 → 模型 → 视图）
- [x] 持久化：dlmgr_load_state / save_state + rebindTask（断点续传重启恢复）
- [x] 暂停/恢复/取消/重启 GUI 按钮（全部已实现，restart 接入 dlmgr_restart）
- [x] 状态栏聚合总速度 + 活跃任务数
- [x] 任务详情对话框（双击弹出，500ms 轮询引擎实时数据）

### 阶段 3：多协议扩展 🟢（不依赖第三方库部分已完成）
- [x] IDownloader 接口落地（HttpDownloader 已实现，FtpDownloader 待 libcurl）
- [ ] BtDownloader（libtorrent，待安装依赖）
- [x] CLI 命令行模式（单命令 + 交互两种，直接调 dlmgr_* 引擎）
- [x] 协议自动识别（ProtocolFactory 按 URL scheme/域名分发，http/https + 视频网站已支持）
- [x] 设置管理器（QSettings：默认目录/线程数/限速/主题）+ 设置对话框
- [x] 引擎全局限速真正生效（chunk_thread 增加 100ms 滑动窗口节流器）
- [x] 批量操作（开始全部/暂停全部/清除已完成，接入 dlmgr_start_all/stop_all/remove_completed）
- [ ] FTP/SFTP（libcurl，待安装 vcpkg 依赖后实现 FtpDownloader）

### 阶段 4：浏览器扩展 + 视频流 + 速度图表 🟢（代码完成，待集成测试）
- [x] MV3 扩展（background.js + content.js + popup）+ 资源嗅探（webRequest + DOM 扫描）
- [x] Native Messaging 对接（Python host + manifest + Windows .bat 启动器）
- [x] yt-dlp 封装 + VideoDownloader（QProcess + 进度解析 + 20+ 视频网站识别）
- [x] 速度曲线图（Qt Charts QLineSeries，60 秒滚动，Y 轴自适应，嵌入 TaskDetailDialog 标签页）
- [ ] 集成测试（需安装扩展 + yt-dlp + 注册 Native Messaging）
- [ ] 扩展图标设计（需 16/48/128px PNG）

### 阶段 5：打磨发布 🟢（大部分完成）
- [x] QSS 样式表接入（亮/暗，applyTheme() 启动+设置变更即时生效）
- [x] 系统托盘图标（双击恢复、右键菜单、关闭最小化到托盘、下载完成通知）
- [x] 剪贴板监听（自动捕获 http/https/ftp/magnet 链接，设置中可开关）
- [x] 拖放支持（拖 URL 到窗口 → NewTaskDialog 预填链接）
- [x] NewTaskDialog 集成修复（传默认目录/线程数到引擎）
- [x] 批量导入 URL（多行文本/文件导入，统一目录+线程数，自动过滤注释行）
- [x] 定时下载（QTimer 5 秒检查到期任务，支持每日重复，托盘通知）
- [x] 文件完整性校验（MD5/SHA-1/SHA-256，分块计算支持大文件，预期值比对）
- [ ] SQLite 持久化替换 JSON
- [ ] 安装包打包（NSIS / dmg / AppImage）
- [ ] spdlog 替换 qDebug 日志

## 环境准备
1. **Qt 6.11.1**（MinGW 13.1.0 64-bit + Charts 模块）—— ✅ 已安装
2. CMake 3.30.5（Qt 安装器附带）—— ✅ 已安装
3. Ninja 1.12.1（Qt 安装器附带）—— ✅ 已安装
4. vcpkg（阶段3 起需要 libcurl / libtorrent / sqlite3 / spdlog）—— 待安装
5. **yt-dlp**（视频下载用，`pip install yt-dlp`，需在 PATH 中）

## 编译验证步骤
```bash
# 使用 Qt 6.11.1 MinGW 64-bit 环境
cd "D:/visual studio/lianxi/MFC/idm-next"
cmake -B build -S . -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/mingw_64"
cmake --build build
./build/idm-next.exe          # GUI 模式
./build/idm-next.exe --cli    # CLI 模式
```

### 首次编译成功 (2026-08-12)
全项目 **首次编译链接成功**，生成 `idm-next.exe`（1.6 MB），GUI 和 CLI 双模式均验证通过。
编译过程修复的问题：

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| 1 | `download_core.h` | `dlmgr_set_callbacks` 重复声明 | 删除第二次声明 |
| 2 | `idownloader.h` | 缺少 `#include <QVariantMap>` | 添加 include |
| 3 | `network.c` | Cookie 头只发 `"Cookie:"` 不带值 | 构造完整 `"Cookie: <value>"` 宽字符头 |
| 4 | `network.c` | `setup_proxy` 死代码产生 unused-function 警告 | 删除 |
| 5 | `schedule_dialog.h/cpp` | 缺少 `setScheduledTime()/setEnabled()` 等预填方法 | 添加 3 个 setter |
| 6 | `main_window.cpp` | `onScheduleDownload()` 用 `findChild<QCheckBox*>()` 太脆弱 | 改为调 `dlg.setEnabled()` / `dlg.setScheduledTime()` |
| 7 | `speed_chart_widget.h/cpp` | Qt 6 移除了 `QtCharts::` 命名空间 | 去掉 `QtCharts::` 前缀和 `QT_CHARTS_USE_NAMESPACE` |
| 8 | `main_window.cpp` | 缺少 `#include <QToolBar>` `<QIcon>` `<QTimer>` | 添加 3 个 include |
| 9 | `idownloader.cpp` / `category_filter_proxy.cpp` | header-only Q_OBJECT 类没有 moc 实现 | 创建 .cpp 文件让 AUTOMOC 生成 moc |
| 10 | `main.cpp` | CLI 模式把 `--cli`/`-c` 当命令传给 CliApp | 过滤掉标志后再传 args |

## 浏览器扩展安装
详见 `browser-extension/README.md`。
