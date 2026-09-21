# IDM Next 分层架构设计（企业化规范版）

> 本文档是 2026-09-19 架构梳理的产物，作为后续重构的**唯一权威依据**。
> 所有结论均来自实测（`wc -l`、`grep` 依赖方向扫描、CMake 源清单核对），不凭印象。
> 配套进度/历史见根目录 `REFACTOR_PLAN.md`（已实现功能）与本文件（结构与分层）。

---

## 1. 现状盘点（实测）

| 维度 | 数值/事实 |
|---|---|
| 源码总量 | `src/**` + `tools/**` 约 **20,766 行**，C 3.7k / C++ 16.9k |
| 最大文件 | `src/gui/main_window.cpp` **1,836 行**（Stage 3 抽 traffic+web+appearance 后；Stage 2 末 2,004，初始 2,883） |
| 次大 | `core/download_core.c` 1,597 · `storage/html_parser.c` 901 · `core/network_curl.c` 573 |
| 目录 | core 7 · gui 41 · utils 5 · storage 6 · protocols 23 · cli 1 · host 1 · tools 8 |
| 死代码 | `core/network.c`（WinHTTP 874 行）**未编译**；`gui/glass_effect.cpp` 已停用但**仍在编译** |
| 构建 | 两个目标（`idm-next`、`ui_snapshot`）各抄一份源清单，易漂移 |
| 版本控制 | 重构前**无 `.git`**，本次已 `git init` 建基线（`874427e`） |

### 1.1 层间依赖方向（实测，已正确）

扫描所有 `#include`：

| 上层 → 下层 | 引用 GUI 头数 | 判定 |
|---|---|---|
| core → gui | **0** | ✓ 引擎不依赖 UI |
| storage → gui | **0** | ✓ |
| utils → gui | **0** | ✓ |
| protocols → gui | **0** | ✓ 协议插件不知道有 UI |
| cli → gui | **0** | ✓ CLI 不依赖窗口 |
| host → gui | **0** | ✓ Native Messaging Host 独立 |
| **gui → core/protocols/storage/utils** | 向下 | ✓ 唯一允许的方向 |

**结论**：依赖箭头方向整体合规，没有"下层回头依赖上层"的倒挂。
真正的结构性问题不是方向，而是**①上帝类（main_window 承担了应用编排）②文件归位错误③死代码入编④构建清单重复**。

### 1.2 边界违规（文件摆错层）

| 文件 | 现在位置 | 应是 | 性质 |
|---|---|---|---|
| `gui/web_server.{h,cpp}` | `src/gui/` | `src/web/` | Web 服务是**基础设施**，不是视图 |
| `gui/task_list_model.{h,cpp}` | `src/gui/` | `src/model/` | 数据模型层 |
| `gui/category_filter_proxy.{h,cpp}` | `src/gui/` | `src/model/` | 代理模型，数据层 |
| `gui/task_group_proxy.{h,cpp}` | `src/gui/` | `src/model/` | 代理模型，数据层 |
| `core/network.c` | `src/core/` | `src/core/legacy/` | WinHTTP 参考实现，未编译 |
| `gui/glass_effect.{h,cpp}` | `src/gui/` | `src/gui/legacy/`（移出编译） | 停用功能留档 |

---

## 2. 目标分层（L0–L4 + 入口）

依赖铁律：**只允许「大号层 → 小号层」单向依赖，禁止任何反向。**
（`app` 层是唯一允许同时编排 `core`/`infra` 与持有 `gui` 引用的层。）

```
┌──────────────────────────────────────────────────────────────┐
│  L4  Presentation（视图）      src/gui/   ← MainWindow(薄壳) + 对话框 + delegate + sidebar
│       │ 只通过 L2 的应用服务拿数据、发指令，自己不编排引擎
│       ▼
│  L2  Application（编排）      src/app/    ← TaskController / ScheduleService /
│       │                                      AutoPowerController / TrayController /
│       │                                      NotificationAggregator / IpcServer / TrafficModePolicy
│       ▼                              （Qt 对象，但无 widget，可单元测）
│  L0  Domain（引擎，纯 C）     src/core/  ← download_core / network_* / DownloadManager(边界)
│       ▲   ▲
│  L1  Infrastructure           src/storage/ (持久化·历史)   src/web/ (远程管理)
│  L3  Protocols（插件）        src/protocols/  ← 仅依赖 L0 域类型，不碰 UI
│                                src/ipc/  src/utils/ (settings/app_paths/logger)
│
│  入口（独立）：src/main.cpp(GUI/CLI 分派) · src/cli/cli_app.cpp · src/host/idm_next_host.cpp
└──────────────────────────────────────────────────────────────┘
```

### 2.1 各层职责与红线

| 层 | 允许依赖 | 禁止 | 典型内容 |
|---|---|---|---|
| **L0 Domain** | 无（纯 C 引擎） | **禁止 `#include <Q…>`**、`QObject`；不碰文件 IO 之外的系统细节 | `download_core.c`、`network_curl.c`、`download_manager.cpp`（C++ 边界，仅做桥接） |
| **L1 Infra** | L0、utils | 禁止依赖 `src/gui`、`src/app` | `storage/`、`web/`、`ipc/`（`QLocalServer` 收发壳）、`utils/` |
| **L2 App** | L0、L1、utils、L3（类型） | 禁止继承 `QWidget`/持有 UI 树；**禁止**在 MainWindow 里塞编排逻辑 | 各 `*Controller`/`*Service` |
| **L3 Protocols** | L0、utils、域类型 | 禁止 `#include` 任何 `gui` 头 | `idownloader`、`*_downloader`、`protocol_factory` |
| **L4 Presentation** | L2（首选）、L0、L1、L3（类型） | 不得直接调用 `dlmgr_*` 做编排（经 L2）；不得写持久化细节 | `main_window`、各 dialog、`*_delegate`、sidebar |
| **入口** | 各自所需 | 不互相依赖 | `main.cpp` / `cli_app` / `host` |

### 2.2 为什么需要 L2（Application）层

现状里 `MainWindow` 同时是「窗口」和「应用编排中枢」：它直连 `DownloadManager`、
亲自维护 `m_videoTasks/m_hlsTasks/m_torrentTasks` 后端实例表、跑队列调度定时器、
管 IPC、管 Web 服务器生命周期、管定时、管托盘、管自动关机。这违反单一职责，
也是 2883 行的根因。抽出 L2 后，`MainWindow` 退化成**薄壳**：
构造时把 L2 服务接好，UI 事件→调用 L2 方法，L2 信号→刷新模型/UI。

---

## 3. 当前 → 目标 映射

### 3.1 `main_window.cpp`（2,883 行）拆分去向

| 现有代码块（行号实测） | 行数 | 去向 |
|---|---|---|
| UI 构建：`setupUi/Menus/ToolBar/StatusBar/CategoryTree`(421–847) | ~430 | **保留** L4 MainWindow |
| 托盘：`setupTrayIcon/onTrayActivated`(849–898) | ~50 | → `src/app/tray_controller` |
| 主题/缩放：`applyTheme/applyZoom/onZoom*`<br>`applyNetworkProxy`(1098–1176) | ~80 | applyTheme/applyZoom 留 L4；proxy 归 `app` |
| 任务操作按钮：`onStart/…/onRemove*`<br>`onStartAll/…`(1358–1702) | ~345 | → `src/app/task_controller` |
| 状态栏聚合：`recomputeStatusAggregates/`<br>`syncStatus/updateStatusBar`(1722–1772) | ~50 | 留 L4（纯展示） |
| 引擎信号转发：`onTaskProgress/`<br>`Completed/StateChanged`(1463–1599) | ~140 | 信号桥 → `task_controller` 收 |
| 完成通知聚合：`onNotifyTimer`(1566) + 相关字段 | ~40 | → `src/app/notification_aggregator` |
| 添加任务：`addTaskFromUrl/enqueueUrl/`<br>`internalAddTask`(2013–2292) | ~280 | `internalAddTask` → `task_controller`；`addTaskFromUrl` 作 L2 入口透传 |
| 后端分派：`start/pause/…/removeTaskById`(2292–2482) | ~190 | → `src/app/task_controller`（含 video/hls/torrent 实例表） |
| 站点抓取/历史/队列管理入口(2482–2518) | ~40 | 留 L4（只是开对话框） |
| 队列调度：`onQueueScheduler`(2589) + `hasFreeDownloadSlot` | ~120 | → `src/app/queue_scheduler`（或并入 task_controller） |
| 定时：`onScheduleDownload/onCheckScheduled`<br>`Tasks/loadSchedules/saveSchedules`(969–1012, 2538–2589) | ~150 | → `src/app/schedule_service` |
| 自动关机：`windowsShutdown/Hibernate/`<br>`maybe/perform/cancelAutoPower`(125–133, 2823–2900+) | ~200 | → `src/app/auto_power` |
| 提示音：`playCompletionChime`(2789) | ~15 | 随 `auto_power` 或独立 `app/chime` |
| 空状态/拖放/剪贴板/eventFilter(1061–1200, 2692+) | ~120 | 留 L4（纯视图交互） |
| IPC：`startIpcServer/onIpcConnection`(1860–2013) | ~155 | → `src/app/ipc_server` |
| Web：`applyWebServer` → `WebServerController`（Stage 3 块#2 已抽，~71 行） | 0 | 起停/端口-令牌刷新归 `src/app/web_server_controller`；任务列表/控制/托盘经 sink 注入 |
| 外观：`applyTheme`/`applyZoom`/`onZoom*` → `AppearanceController`（Stage 3 块#3 已抽，~55 行） | 0 | 主题 QSS+自绘委托同步经 repaint sink 触发，工具栏图标尺寸经 `QToolBar*` 注入，缩放级数由组件持有 |

> 拆分**只搬实现、不搬接口语义**：公开方法签名（`addTaskFromUrl`、`refreshFromEngine` 等）
> 与对外行为不变，确保现有自测/探针/走查截图全部可复现。

### 3.2 目录归位（零逻辑改动）

| 现有 | 目标 | 类型 |
|---|---|---|
| `gui/web_server.{h,cpp}` | `web/web_server.{h,cpp}` | 移动 |
| `gui/task_list_model.{h,cpp}` | `model/task_list_model.{h,cpp}` | 移动 |
| `gui/category_filter_proxy.{h,cpp}` | `model/category_filter_proxy.{h,cpp}` | 移动 |
| `gui/task_group_proxy.{h,cpp}` | `model/task_group_proxy.{h,cpp}` | 移动 |
| `core/network.c` | `core/legacy/network_winhttp.c` | 移动（参考实现，不编） |
| `gui/glass_effect.{h,cpp}` | `gui/legacy/glass_effect.{h,cpp}` | 移动（移出 `GUI_SOURCES`） |

### 3.3 构建收敛

新增 `add_library(idm_app_lib STATIC ${IDM_APP_SOURCES})`，
`idm-next` 与 `ui_snapshot` 都 `target_link_libraries(... idm_app_lib)`，
源清单只维护一份。`idm_app_lib` 含 AUTOMOC（`CMAKE_AUTOMOC` 对库目标同样生效）。

---

## 4. 分阶段迁移计划（每阶段带验收门）

> 铁律（来自项目三条硬纪律）：① 设置/行为必须真生效 ② 测试必须能失败 ③ **先量再改**。
> 每阶段结束必须过**同一套验收**，否则不进下一阶段：
> - `engine_selftest` **37 / 0**（先起 `tools/engine_selftest_server.py`，同 Bash 调用内起停）
> - 三个探针全 PASS：`IDM_CONCURRENCY_PROBE` / `IDM_SETTINGS_PROBE` / `IDM_SCHEDULE_PROBE`
> - `ui_snapshot` 完整走查出图（24+ 张），且与上阶段**像素级一致**（无 UI 改动则 `git diff` 仅 `main_window.cpp` 切割，无 `.qss`/布局变化）

### 阶段 0 — 安全网与底座 ✅ 已完成
- git 基线（`874427e`）+ `.gitignore/.gitattributes/.editorconfig/.clang-format`
- 换行 LF 归一，避免未来 diff 噪音

### 阶段 1 — 目录归位 + 构建收敛（纯移动，零风险） ✅ 已完成 (2026-09-19)
- 3.2 全部移动 + CMake 源清单去重（`idm_app_lib`）
- 实际移动：`gui/{web_server,task_list_model,category_filter_proxy,task_group_proxy}` → `web/`、`model/`；
  `core/network.c`（未编译） → `core/legacy/network_winhttp.c`；`gui/glass_effect.*`（已停用留档）→ `gui/legacy/`，移出编译
- `idm_app_lib` STATIC 聚合全部应用层源码，`idm-next`/`ui_snapshot` 仅保留入口 + qrc，继承 include/Qt/libcurl；vendored libcurl 运行期 DLL 拷贝落到 `idm-next`/`ui_snapshot`/`engine_selftest`
- **验收（已通过）**：全量构建通过（4 目标）；engine 37/0；三探针 PASS；走查出图 7/7 md5 一致（零视觉变化）

### 阶段 2 — 抽出 L2 独立组件（行为不变）
按隔离度从易到难，每个单独提交、单独验收：
1. ✅ `app/auto_power`（关机/休眠/权限/倒计时/提示音）— 已完成 (2026-09-19)：`AutoPowerController` 注入回调解耦 MainWindow 内部状态；engine 37/0 + 三探针 PASS + 走查 7/7 md5 一致
2. ✅ `app/ipc_server`（QLocalServer 收发壳 + JSON 命令解析 + 响应）— 已完成 (2026-09-19)：`IpcServer` 注入 `addTask` 回调与 `taskCount` 提供器解耦 MainWindow；`addTaskFromUrl`/`enqueueUrl` 保留为公开 API 不动；engine 37/0 + 三探针 PASS + 走查 7/7 md5 一致
3. ✅ `app/schedule_service`（定时持久化 + 触发 + 每日重复）— 已完成 (2026-09-19)：`ScheduleService` 拥有定时表与每日重复集合，注入 `taskExists` 提供器 + `taskDue` 信号解耦；`schedulesRestored` 信号沿用原「已恢复 N 条」状态栏提示；engine 37/0 + 三探针 PASS + 走查 A/B 构建对照渲染零差异（01/04/05 逐字节一致，02/03/03b/06 的差异被同构建两次运行的噪声完全复现）
4. ✅ `app/tray_controller`（托盘图标 + 菜单 + 双击恢复）— 已完成 (2026-09-19)：`TrayController` 拥有图标 + 右键菜单 + 流量档位子菜单，菜单动作经 6 个回调解耦（显示/恢复/新建/全开始/全暂停/流量档位）；`m_trayIcon` 经 `trayIcon()` 交给 MainWindow 供各处 showMessage；engine 37/0 + 三探针 PASS + A/B 构建对照渲染零差异（01/04/05 逐字节一致）
5. ✅ `app/task_controller`（任务 CRUD + 后端分派表 + 队列调度）—— 已完成 (2026-09-20)：
   - 作为**单个组件**抽取（未再拆成 media 注册表 + CRUD 两步，降低提交数与风险）。
   - `TaskController` 拥有 media 后端实例表（`m_videoTasks`/`m_hlsTasks`/`m_torrentTasks`）+ id 序号（`m_videoIdSeq=2000000`）+ `m_pendingStream`；`internalAddTask`（aria2/HLS/yt-dlp/引擎四分支）与 6 个分派动词（`startTaskById`/`pauseTaskById`/`resumeTaskById`/`cancelTaskById`/`restartTaskById`/`removeTaskById`）整体搬入。
   - **低风险边界**：`m_states`/`m_speeds` 仍留在 MainWindow，经注入的 `setStateSink`/`setSpeedSink` 回调写回；引擎信号桥（`onTaskProgress/Completed/StateChanged`）与状态聚合（`recomputeStatusAggregates`/`syncStatus`）零改动 → 并发探针（依赖 QueueScheduler + 任务态）与状态栏文案自测不受影响。
   - media downloader（Video/Hls/Torrent）信号经 `TaskController::mediaProgress/mediaCompleted/mediaStateChanged` 转发到 MainWindow 既有桥槽，与 C 引擎信号汇到同一 UI 刷新路径。
   - `ScheduleService::removeTaskSchedule` 经注入回调解耦（ScheduleService 创建晚于 TaskController，调用点 `m_scheduleService` 做 null 守卫）。
   - engine 37/0 + 三探针 PASS + A/B 构建渲染零差异（01/04/05 逐字节一致，02/03/03b/06 差异被同构建两次运行的噪声完全复现）。
6. ✅ `app/notification_aggregator`（完成气泡聚合）—— 已完成 (2026-09-20)：
   - `NotificationAggregator` 拥有 750ms 单发聚合定时器 + 完成/失败计数 + 任务名列表 + 最近错误；
     `enqueue(success, name, error)` 累计，`onTimeout` 合并为单条气泡经 `notify(title, body, chime)` 信号交回 MainWindow。
   - MainWindow 经 `m_trayIcon->showMessage` 显示气泡、按 `chime` 调 `playCompletionChime()`；托盘不可用时静默跳过（与原 `if (!m_trayIcon)` 早退一致）。
   - 历史持久化（`HistoryStore::recordFinished`）与自动关机检查（`maybeAutoPowerAction`）保留在 `onTaskCompleted`，不搬。
   - engine 37/0 + 三探针 PASS + A/B 构建渲染零差异（01/04/05 逐字节一致）。
- **阶段 2 收口**：6 件 L2 组件全部抽出，`main_window.cpp` 2883 → **2004 行**（纯视图 + 信号桥）。
- **验收**：每个抽取后 engine 37/0 + 三探针 PASS + 走查出图一致

### 阶段 3 — MainWindow 瘦身（收口）
- 阶段 2 抽干净后，`main_window.cpp` 应降至 ~900–1100 行（纯视图 + 信号桥）；本机实际抽完 6 件后已 2,004 行，剩余主要为 UI 构建 + 信号桥 + 各 downloader 完成/进度桥槽。
- 公开 API（`addTaskFromUrl`/`refreshFromEngine`/`enqueueUrl`）签名不变
- **验收**：每个抽取后 engine 37/0 + 三探针 PASS + A/B（worktree 取上一提交）01/04/05 逐字节一致
- **已抽块 #1 — 流量档位应用 → `TrafficModeController`**：`onTrafficModeChanged`/`applyTrafficMode`/`syncTrafficCombo`/`updateTrafficIndicator` 四个方法 + 匿名命名空间 `trafficKbpsForMode`/`trafficModeName` 搬入 `src/app/traffic_mode_controller.{h,cpp}`。引擎/aria2/托盘动作经 sink 注入（`setMaxSpeedSink`/`setTorrentLimitSink`/`setTraySyncSink`），下拉框+任务模型经 `setWidgets` 注入，`attachCombo()` 接 `currentIndexChanged`。`main_window.cpp` 2,004 → **1,936 行**。验收全过（构建 + 引擎 37/0 + 三探针 + A/B 01/04/05 一致）。
- **已抽块 #2 — Web 管理界面起停 → `WebServerController`**：`applyWebServer` 方法（~71 行）整块搬入 `src/app/web_server_controller.{h,cpp}`。任务列表经 `TaskListModel` 提供器注入、`addTask`/`control` 经 sink 注入、托盘通知经 sink 注入；`buildProviders()` 构造 WebServer 请求回调，`apply()` 逐行等价原方法。`main_window.cpp` 1,936 → **1,891 行**。验收全过（构建 + 引擎 37/0 + 三探针 + A/B 01/04/05 一致）。
- **已抽块 #3 — 外观应用（主题 QSS + 视图缩放）→ `AppearanceController`**：`applyTheme`/`applyZoom`/`onZoomIn`/`onZoomOut`/`onZoomReset` 五个方法与 `m_zoomLevel`/`BASE_FONT_PT`/`BASE_ICON_SZ` 成员整块搬入 `src/app/appearance_controller.{h,cpp}`。主题加载后需重绘的自绘控件（`ProgressDelegate`/`SidebarPanel`）经 repaint sink 触发、工具栏图标尺寸经 `QToolBar*` 注入、缩放级数由组件持有。`main_window.cpp` 1,891 → **1,836 行**。验收全过（构建 + 引擎 37/0 + 三探针 + A/B 01/04/05 一致）。
- **下一块候选**：category-tree/queue-tree 构建（须先量爆炸半径，遵守全局多视角要求）、其余纯视图桥槽（onNetworkProxy/applyGroupMode 等）视收益再定。

### 阶段 4 — 协议层与基础设施强化（可选，按需求）
- `protocols` 补 `FtpDownloader`/`BtDownloader`（需 vcpkg 依赖，见 REFACTOR_PLAN 务实调整）
- `storage` 由 JSON 切 SQLite（历史库已在用 Qt Sql）
- 日志 `logger.h` 增加级别/过滤/文件落盘（替代散落的 `qDebug`）
- CI：`.github/workflows/build.yml`（CMake + Ninja 构建 + 引擎自测）

---

## 5. 命名与接口约定（全工程统一）

- **类名**：L2 服务用 `*Controller` / `*Service`；L1 用 `*Store` / `*Server`；模型用 `*Model` / `*Proxy`
- **信号桥**：L0 引擎回调 → `DownloadManager` 的 `Qt::QueuedConnection` 信号 → L2 → L4，
  保持线程安全（现状已成立，迁移不得破坏）
- **持久化键**：集中在 `AppPaths::settings()` 一处，L2 不直接 `QSettings`
- **入口单一**：引擎 `dlmgr_init/destroy` 仅在 `main.cpp`（GUI/CLI 共用）调用一次

---

## 6. 反模式（禁止，CI 可加 clang-tidy 规则）

1. ❌ `core/*.c` 出现 `#include <QObject>` 或任何 Qt 头
2. ❌ `protocols/` 出现 `#include "main_window.h"` 或任何 `gui` 头
3. ❌ `MainWindow` 里直接 `dlmgr_*` 做编排（必须过 L2）
4. ❌ 一个 `.cpp` 超过 ~1000 行且还在涨（触发拆分）
5. ❌ 走查/自测工具写进用户真实 `tasks.json`（已由 `IDM_DATA_DIR` 隔离 + 探针护栏解决）
