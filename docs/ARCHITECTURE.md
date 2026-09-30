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
| 死代码 | 2026-09-19 当时：`core/network.c`（WinHTTP 874 行）**未编译**；`gui/glass_effect.cpp` 已停用但**仍在编译**。2026-09-29 复核：两个路径都已搬迁（现 `src/core/legacy/network_winhttp.c`、`src/gui/legacy/glass_effect.cpp`），`glass_effect.cpp` **也不再编译**（`grep -c glass_effect build/build.ninja` = 0，原表这一格已过期）；但 `src/gui/main_window.h:10` 仍 `#include "legacy/glass_effect.h"` ⇒ "完全断开"不成立。逐条命令与命中数见 §7「否定式断言全表核」 |
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
（2026-09-29 复核曾发现这半句不成立：阶段 1 把 `task_list_model` 搬进 `model/` 之后，
`src/model/task_list_model.cpp` 仍 `#include "app_icons.h"`，是一条真实的 L1 → L4 反向依赖。
**该条已按 (a) 修掉**（`d7a89ca`：模型只发 `FileTypeRole`，图标在 gui 层的 `FileTypeIconDelegate` 查表），
现在这条结论由 `tools/lint_layering.py` 持续守着（接在 CI 第一步，越界即红）。详见 §7 第 7 条。）
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

---

## 7. 修改记录（按轮次追加）

> §4 里那些带日期的 ✅ 行是阶段级验收记录；从这里开始是按**轮次**记的流水，
> 每条都要求写：改了什么、**怎么验的 + 实际输出**、以及**仍然没验证的东西**。
> 没有第三条的条目不算写完。

### 2026-09-29 — 并发上限有了读者；组件自愈不再冻住 GUI 线程；IPC 超时改为可诊断

仓库 `idm-next`（分支 master）。提交（按批次，探针与修复分开）：
`ef1fb7d` 引擎并发上限 · `8df07e2` IPC 转发四条出口分说 · `90ac237` UI 探针进 CI 闸门 ·
`a88cfc9`/`b47699d` 引擎自测闸门与 MSYS2 收尾 · `369bf21` **自愈挪出 GUI 线程 + 撤回重发** ·
`3213bb1` 冻结算式改测最大间隔 · `3cfe28e` 排队取证 regex ·
`85bfb84` 幂等改成断言 · `bd0305a` 契约注释 · `9538b87` 探针两处脆弱（前置改为等管道真的可连）。

**改了什么**
1. `cfg.max_concurrent` 以前在引擎里只被写入、无人读 ⇒ 「全部开始」（托盘 / CLI `start all`）
   把列表里所有待下载一次全拉起。现在 `dlmgr_start_all` 起满上限、其余进等待表，
   任务落到终态时补位；`dlmgr_pause/cancel/detach_slot/stop_all` 摘表；新增
   `dlmgr_deferred_count()` 供状态栏与 CLI 报「另有 N 个在等空位」。
2. 首次安装的组件自愈（下载 ffmpeg / aria2c）原先整段跑在 GUI 线程上：
   `QNetworkReply::finished` 的 lambda 里有「写 90MB + `tar -xf` 后
   `waitForFinished(60000/30000)` + 逐个复制」。新增 `src/utils/off_thread.{h,cpp}`
   的 `runOffThread(context, job, done)`（只用 QtCore，无新依赖）把它挪出 GUI 线程。
   这也是 CI 第 13 步那个「第二实例连得上、等不到回话」的真因；
   此前用的「幂等命令再问一次 + 窗口 9s」是掩盖，**已撤回**（恢复一次往返、窗口 5s）。
3. IPC 超时从"静默 exit 1"变成可诊断：四条出口分别报话，服务端每次回话写
   `IPC 请求: X → 成功（排队 Nms，处理 Mms，回话时客户端=在/已走）`，
   CI 第 13 步把它抓进 annotation（绿的那些轮也记）。
4. 断言化：`IDM_ACTIVATE_PROBE` 证明 **activate 连投两次都被处理、且不产生第二份副作用**
   （两次各自读到 success，再比任务列表行数与引擎任务数）；
   `IDM_OFFTHREAD_PROBE` 证明重活换线程后 **GUI 事件循环没被打断**（tick 最大间隔 <150ms）。

**验证命令与实际输出**
```
cmake --build build                                   → BUILD=0
python tools/engine_selftest_server.py 18080 idmuser idmpass &
./build/engine_selftest.exe 18080 idmuser idmpass     → 结果：142 通过，0 失败（退出码 0）
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
   → 13 个模式 rc 全 0，「UI 探针全绿：13 个模式共 50 条断言」，连跑 3 轮一致
```
闸门有效性是**演示过红**的，不是声称的：上限不挡 / 不加补位 / 暂停不摘表 /
补位不记账 / 服务端 activate 睡 2 秒 / activate 改成累加型 / 自愈退回同步执行
—— 七个变异各自打在各自该红的断言上（M1–M9，细节见各提交说明）。
CI 侧：`369bf21` 那一轮 step 8/9/13 全 success，第 13 步报
「稳态二次启动 0.0s 退出码 0」「GUI 在跑时 CLI 经 IPC 出 help 0.0s」「竞态收敛 6.8s」。

**仍然没验证的东西**
1. 首次安装那次数秒到数十秒的冻结，改前/改后各是多少秒 —— 需要真下载并解压 90MB，
   按"不打包、不装机"的约束没跑；改前的"≥9 秒"是从 CI 观察窗反推的下界。
2. CI 的 `排队 Nms` 分布（p50/p95/max）—— 取证字段 `3cfe28e` 才修好（此前正则
   要求 ASCII 空格而日志里是中文逗号，一直静默报「没抓到」），数据从后续几轮才开始有。
3. 以用户身份走一遍「首次安装 → 加任务 → 暂停/继续 → 关应用重开 → 看列表和速度」——
   没起过 GUI。引擎层有 [8]/[25]/[26] 覆盖暂停继续、[9]/[10]/[11] 覆盖状态文件往返与
   load 幂等，但 **GUI 重启后列表重建** 与 **速度列取值** 一条断言都没有。
4. 重开后"刚才还在下"的任务：`dlmgr_load_state` 把 RUNNING 落成 PAUSED
   （`download_core.c:1997`），而队列调度器只自动拉起「排队中且属于启用队列」的任务
   ⇒ 它不会自己续跑，要用户点开始。这是产品决策，本轮未改。
5. 其余仍在 GUI 线程上的同步等待没动：`aria2_daemon.cpp:108`（terminate 后等 2s）、
   `video_downloader.cpp:206`（等 3s）；yt-dlp 的自愈是单文件下载、形态不同，也未改。
6. 探针那条间歇红（某轮 `ack=0/1`）的**成因没证明**：把疑似旧写法编出来连跑 30 次
   0 失败，复现不出。已移除两处结构性脆弱（客户端线程无事件循环、只泵一圈就假定
   listen 完成 + 等不到就明确报「前置不成立」），修好的版本 30/30 绿、闸门 3 轮绿，
   但这只能说"已知脆弱被拿掉"，不能说"那条 flaky 修好了"。

### 2026-09-29（续）— 探针产物不再写进用户主目录根；47 项遗留清单已交

本轮修的不是产品功能，而是**我自己留下的工作区污染**：`ui_snapshot` 的输出目录默认值
曾经是硬编码的 `C:/Users/yyyy/idm_shots`，加上"每轮临时换一个新 `IDM_SHOT_DIR`、
顺手 tee 一份构建日志到主目录根"的写法，09-11→09-29 攒出 **47 个条目 / 191.5MB**
（十几个截图目录 + 三十来份日志）堆在用户工作区根上。

改动：
1. `tools/ui_snapshot.cpp` 默认输出 → `%TEMP%/idm-next-probes/<当天日期>`；
2. 同文件新增 `sitsInHomeRoot()` 守卫：`IDM_SHOT_DIR` 直接落在主目录第一层 ⇒ **拒写、返回 2**
   （约定会被忘记，所以做成断言）；
3. `.github/workflows/ci.yml` 的产物目录统一到同一约定；
4. `.gitignore` 增加 `build-probes/`（仓库内备选位置）；
5. 新增 `tools/probe_artifact_inventory.py`（只读清点器：清单 + 仓库引用检查 + 同族去重比较
   + `--count`），产物表落在 `docs/probe-artifact-inventory-2026-09-29.md`。

验证命令与实际输出：
```
IDM_SHOT_DIR="C:/Users/yyyy/idm_shots_new" ./build/ui_snapshot.exe
  → rc=2  "IDM_SHOT_DIR 拒绝写进用户主目录根：C:/Users/yyyy/idm_shots_new"
（不传）./build/ui_snapshot.exe
  → rc=0  [out] 产物目录 = %TEMP%\idm-next-probes\2026-09-29
python tools/probe_artifact_inventory.py --count
  → COUNT=47 TOTAL_MB=191.5 HOME=C:\Users\yyyy
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
  → 13 模式 rc 全 0，「UI 探针全绿：13 个模式共 50 条断言」
```

仍然没验证 / 没做的：
1. **一个文件都没删** —— 清单交完等你拍定；`idm_ui_v2..v8` 很可能是 09-14 那轮 UI 迭代
   仅存的逐版本快照（截图从不入 git），删错无法重建。
2. 引用检查的结论是"仓库内零脚本依赖"，但**别的会话/手工习惯是否还在往这些目录写**，
   这台机器上没有账本可查，我只知道自己这轮之后不再写。
3. `--match` 的判定用同名数为主：同一份代码连跑两次，部分面板的 PNG 也会有噪声差异，
   所以"逐字节不同"不能当"界面变了"的证据（本轮实测：与当前渲染同名 14、逐字节同 6）。

### 2026-09-29（再续）— IPC「没回话」不是慢，是回话被丢掉；更正我上一轮的判断

上一段加的取证字段第一次跑就推翻了我自己的结论。CI 第 13 步（`2e54031`）同一份 annotation 里：
```
服务端  IPC 请求: activate → 成功（排队 51ms，处理 0ms，…）
副实例  IPC 转发失败: 已连上运行中的实例并送出 39 字节，3 秒内没有回话
```
51ms 就答了，客户端却没收到 ⇒ 不是"服务端慢"，是"答了没发出去"。
机制：Windows 上 QLocalSocket 写出异步（QWindowsPipeWriter），旧代码 `write → flush →
waitForBytesWritten(2000)` **只等一次**就 `disconnectFromServer()` + `deleteLater()`，
没推完的字节随 socket 一起没了。改法：循环推到 `bytesToWrite()==0`（上限 2s），
并新增 `送达=0/1` 字段作为这条机制的现行信号（`b0562b7`）。

同时更正我写进记忆里的一句话：上一轮我根据本机观测说
"`waitForBytesWritten` 返回值不能当交付证明，实测客户端读到回话时它仍返回 false" ——
因果说反了：返回 false 正是没推完，而"没推完就销毁"就是丢回话的方式。

测具侧配套（`2a30f83`）：`IDM_ACTIVATE_PROBE` 加 C 段，连续投递默认 50 次
（`IDM_ACT_STRESS` 可调）必须全部拿到回话 —— 概率性丢失量两次是量不出来的。

验证输出：
```
IDM_ACT_STRESS=300 ./build/ui_snapshot.exe   # 修好的服务端
  → [act] … C 段 ack 300/300，rc=0
IDM_ACT_STRESS=300 ./build/ui_snapshot.exe   # 临时退回旧写法（只等一次）
  → [act] … C 段 ack 300/300 ×2 轮，rc=0
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
  → 13 模式全绿，共 51 条断言；引擎自测 142/0
```

仍然没证明的：**本地复现不出这个丢弃**（旧写法两轮各 300 次，600/600 全 ack）。
所以这条是按"CI 现场数字 + Qt 异步写出语义"做的收窄，不是被变异证明过的定论。
判据留在 `送达` 字段上：CI 再出现 `送达=0` 就是同一条路；
若全是 `送达=1` 而客户端仍超时，就得转去查客户端收包侧。

### 2026-09-29（第 4 条）— drain helper 的本地 A/B 是**负结果**；两个未跟踪文件的处置

接上一条的"回话被丢"假设，本轮把它做成可跑的对照：helper 抽成
`src/utils/socket_drain.{h,cpp}` 的 `drainSocketBeforeClose(sock, budgetMs)`，
`IpcServer::handleClient` 发完回话调它，再新增探针 `IDM_FLUSH_PROBE`：
服务端写 2MB 回话，对端**晚 300ms 才开始读**，两个分支都以同样的方式立刻销毁 socket，
唯一区别是"只等一次"还是"推到空"。

**结果是否定的，照实记**：
```
[flush] 旧(只等一次)(delay=300) 单次等待返回=1 销毁前待发=0 字节 推完=1 用时=27ms
[flush] drain(delay=300)                销毁前待发=0 字节 推完=1 用时=30ms
[flush] 本机负结果：旧写法收到 2097152/2097152，新写法收到 2097152/2097152
```
两组尝试（对端完全不读 / 对端晚读）都**造不出"返回时仍有待发字节"** 的状态 ⇒
本机既证不了也证不伪这个假设。探针因此只断言这个 helper 本身可证的两条契约：
① 推完再销毁时晚读对端也拿到完整 2MB；② 等待被预算封顶（实测 30ms < 2600ms，
它跑在 GUI 线程上不能钉死人）。helper 保留的理由改写成"严格更保守且不多钉 GUI 线程"，
不再是"证明过的修复"；**判据留给生产字段 `送达=0/1`**：CI 出现 `送达=0` 即坐实，
若全是 `送达=1` 而客户端仍超时则此路不通、要换方向查。

同时更正 `cli_forward.cpp` 里一句过强的因果（原写"根因是 tar 解压阻塞"——
那解释不了"排队 51ms/处理 0ms 却没人收到回话"，已改成两种可能并列 + 判据指路）。

两个一直未跟踪的文件（本轮第一次处理，**一个都没删**）：
- `docs/AI-HANDOFF.md`：用户写的交接提示词，167 行、9 节（角色/环境/基线/三条硬纪律/
  环境坑/当前状态/下一步候选/路径速查/交付方式）。有内容 ⇒ 按"未提交不算完成"单独一笔进 `docs/`。
  注意其中的验证基线数字（engine 91/0 等）与 §7 候选 1、2 已经过期，**未改他的正文**。
- `idm-next-glass-ui.html`：仓根，16.6KB，标题「IDM Next · 柔雾玻璃仪表盘」，
  Tailwind + Google Fonts 的**独立 HTML 设计稿**（第三种 UI 方向，玻璃拟态）。
  **可核口径**：`src/`、`CMakeLists.txt`、`tools/` 里零引用；全仓（排除 `.git/`）唯一的
  文本命中就是本清单自己（`docs/ARCHITECTURE.md`）。原来写的"仓库内零引用"是一句
  **被自己推翻的断言**——说出这句话的这一行本身就是一次引用，命中数不是 0 而是 1。
  ```
  $ grep -rl "glass-ui" --include=*.md --include=*.cpp --include=*.h .
  ./docs/ARCHITECTURE.md                ← 命中 1 个文件，就是本文件（用户复核同结果）
  $ grep -rn "glass-ui" src/ CMakeLists.txt tools/ | wc -l
  0
  $ grep -rIn "idm-next-glass-ui" . | grep -v '^./.git/' | cut -d: -f1 | sort | uniq -c
        1 ./docs/ARCHITECTURE.md        ← 放开扩展名限制、扫全仓，仍只有本文件这 1 条
  ```
  与 `src/gui/legacy/glass_effect.cpp` 的关系**收窄为"设计意图相同（都做玻璃拟态）"**：
  代码层零联系（`grep -c "glass_effect\|MainWindow\|\.cpp" idm-next-glass-ui.html` = 0），
  所以原来那句"同一条废弃路线"作为事实断言**降级为未核实**。
  ⇒ 去向（进 `docs/design/` 留档 / 进 `.gitignore` 承认是临时产物）**仍等用户拍定**，
  本轮不动、也不写进"已处理"。

验证输出：
```
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
  → 14 个模式 rc 全 0，「UI 探针全绿：14 个模式共 53 条断言」（闸门下限同步抬到 ≥46）
./build/engine_selftest.exe …            → 结果：142 通过，0 失败（退出码 0）
```

### 2026-09-29（第 5 条）— 第 13 步**没修好**：第三条机制被本地测到（空转连接独占 GUI 线程 1090ms）

先把事实摆出来，不用"最近两轮绿"糊过去。`ci-idm-next` 第 13 步（Single-instance startup-race
and IPC smoke test）今天 9 轮的结论，按提交顺序（`369bf21` = 把组件自愈挪出 GUI 线程那笔）：

| commit | 时间 | step 13 |
| --- | --- | --- |
| `ef1fb7d` | 11:38 | failure |
| `7ee54de` | 11:45 | failure |
| `369bf21` | 15:44 | **success** |
| `3213bb1` | 15:56 | failure |
| `3cfe28e` | 15:59 | **success** |
| `2e54031` | 16:18 | failure |
| `a0ac450` | 16:33 | failure |
| `91e8f0e` | 16:44 | **success** |
| `4d7c4ab` | 17:00 | **success** |

⇒ 自愈挪出 GUI 线程之后 **7 轮里 4 绿 3 红**。若这条修复真的把问题解决了，单轮红概率应≈0，
观察到 3/7 的可能性就是 0；所以"回话被丢/自愈冻结"**只解释了部分**，第 13 步仍未闭合。
宣布"修完"的门槛我按统计定：**连续 14 轮 step 13 全绿**
（若真实红概率还有 0.2，14 连绿的概率 0.8^14≈4.4% < 5%；只凭 2 连绿不能区分"修好"与
"红概率 0.43 时连抛两次绿"，其概率约 32%）。达不到这个门槛，我就继续说"未闭合"。

第三条机制是用户提出假设、我本地测到的：`handleClient` 那个 10×100ms 有界读循环跑在
GUI 线程上，而 `onNewConnection` 是 `while (hasPendingConnections()) handleClient(...)`
一次取尽 ⇒ **一条连上却不发消息的客户端**（扩展轮询超时、副实例被杀留下的半开连接）
能让 GUI 线程整秒不回事件循环，后面每条连接都跟着排队。两条三条就越过客户端 3 秒死线 ——
这正好是"服务端日志显示很快、客户端却什么都没等到"的形状。

* 测具：`IDM_ACTIVATE_PROBE` D 段，量**事件循环单轮最长耗时**。
  第一版写成"空转连接之后再问一次量往返"，测出 14ms —— 测具错了：阻塞整个在
  某一次 `processEvents()` 调用内部，我那 150ms 等待循环把这一秒吞在了计时之外。
* 修复前实测：**1088 / 1089 / 1090ms**（3/3 次，断言 <500ms 稳定红）。
* 修复：只在"已经收到半条消息"时才继续等下一片，一个字节都没到过就 200ms 宽限后放手
  （分片兜底保留：data 非空仍等满 10 轮）。修复后实测：**216 / 217 / 217 / 218ms**（4/4 绿）。
* 同批更正：上一笔我在回话那段注释里写"这条是真因"和"IDM_FLUSH_PROBE 双向证明旧写法必丢"
  —— 两句都过度声称，本地 A/B 是负结果（见第 4 条），调用点没跟 header 同步 = 同一轮里
  两份制品互相矛盾。现在调用点写成"保守做法 + 判据交给 `送达=0/1`"，
  并把这条已测到的机制并列列出。

验证命令与实际输出：
```
IDM_ACTIVATE_PROBE=1 ./build/ui_snapshot.exe
  [act] A 段两次 ack=1/1 模型行数 5→5 引擎任务数 5→5 | B 段放弃后往返=14ms（需 <1500）
        | C 段 ack 50/50 | D 段 事件循环单轮最长=217ms（需 <500）      → 7 条全 PASS，rc=0
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
  → 14 个模式 rc 全 0，「UI 探针全绿：14 个模式共 54 条断言」
./build/engine_selftest.exe 18130 idmuser idmpass   → 结果：142 通过，0 失败
```

仍未闭合：第 13 步的红。`送达` 字段（第 4 条）现在也还在场上 —— 下一轮若再红，
annotation 里会同时有"排队/处理/送达/客户端在不在"和第 13 步自己的计时行，
可以据此判剩余的红属于哪一类，而不是再猜。

> **2026-09-30 更正（第 5 条里那句"修复后 216/217/217/218ms"不成立）**：同一台机器、同一份二进制，
> 隔一天把 `IDM_ACTIVATE_PROBE` 单独跑 8 次，D 段实测
> `353 / 236 / 215 / 1636 / 221 / 748 / 736 / 209 ms`，**3/8 超过 500ms 门槛**（套件里另有一次 530ms）。
> 昨天的 4 个数是安静时刻的 n=4 快照，不能当"稳定"。读数呈 ~217ms 的整数倍（748≈3×217、1636≈7×217），
> 与"每条半开连接各付一次 200ms 宽限、而 `onNewConnection` 是串行取尽"这个机制一致 ——
> 也就是说那条修复**只把单条的成本封顶了，没有把 N 条的总成本封顶**。
> 状态：未定位到能定论的证据，**不记为已排除**；门槛也不许为了绿而放宽。
>
> **还原点脆弱性**：`backup/pre-squash-68e38de` 只存在于本机，远端没有这 4 笔；
> 下一次 rebase/force 操作之前必须先确认它还在（`git rev-parse backup/pre-squash-68e38de`），
> 否则压扁前的历史无处可回。

### 2026-09-29（第 6 条）— 本文档的否定式断言逐条核（有一句正在被自己推翻）

**起因（用户复核，我确认）**：§7 第 4 条介绍 `idm-next-glass-ui.html` 时写"仓库内零引用"，
而这句话本身就是一次引用 —— 跑 `grep -rl "glass-ui" --include=*.md --include=*.cpp --include=*.h .`
命中的那个文件就是 `docs/ARCHITECTURE.md`。"零命中≠不存在"这次是反方向：**声称零命中，
而命中就是声称本身**。据此把全文"零引用 / 无人依赖 / 已完全废弃 / 仍在编译"类断言扫一遍。
以下所有命令都在仓库根 `D:\visual studio\lianxi\MFC\idm-next` 执行，命中数是当次原文输出
（原文见本条末尾的取证块）。

| # | 断言（位置） | grep 命令 | 命中数 | 结论 |
|---|---|---|---|---|
| 1 | `idm-next-glass-ui.html` 仓库内零引用（§7 第 4 条「两个未跟踪文件」段） | `grep -rl "glass-ui" --include=*.md --include=*.cpp --include=*.h .`（V1） | **1** 个文件 = `./docs/ARCHITECTURE.md` | **原口径不成立（自指）**。改为可核口径：除本清单自身外零引用；`src/`+`CMakeLists.txt`+`tools/` 命中 **0**（V2）；放开扩展名扫全仓仍只有本文件 **1** 处（V3） |
| 2 | 该 HTML "与已停用的 `glass_effect.cpp` 同一条废弃路线"（§7 第 4 条） | 在 HTML 内找代码符号：`grep -c "glass_effect\|MainWindow\|\.cpp" idm-next-glass-ui.html`（V4） | **0** | **降级为未核实**：代码层零联系，只有"玻璃拟态"这个设计意图相同。已把措辞收窄为"设计意图相同" |
| 3 | `core/network.c`（WinHTTP 874 行）**未编译**（§1「死代码」行、§1.2 表、§3.2 表） | `ls src/core/network.c`（V5a）· `wc -l src/core/legacy/network_winhttp.c`（V5b）· `grep -c network_winhttp build/build.ninja`（V5c） | 原路径**不存在**；行数 **874**；当前构建脚本 **0** | **成立且更强**，但路径过期：文件现为 `src/core/legacy/network_winhttp.c`，874 行复核一致 |
| 4 | `gui/glass_effect.cpp` 已停用但**仍在编译**（§1「死代码」行） | `grep -c glass_effect CMakeLists.txt`（V6a）· `grep -c glass_effect build/build.ninja`（V6b）· `ls build/CMakeFiles/idm-next.dir/src/gui/glass_effect.cpp.obj`（V6c） | **0 / 0**（旧路径 obj 存在，日期 2026-09-14） | **不成立**（这条"现状盘点"在 09-19 阶段 1 之后就已过期）。那个 `.obj` 是搬迁前的遗留产物，不能当"仍在编译"的证据 —— 与本仓库踩过的 stale-binary 同一个坑 |
| 5 | `glass_effect.*` 移出编译、停用留档 ⇒ 可理解为"已完全废弃"（§1.2 表、§4 阶段 1） | `grep -rn glass_effect src/ --include=*.h --include=*.cpp`（V7） | **2**：`src/gui/legacy/glass_effect.cpp:1`（自身）+ **`src/gui/main_window.h:10` 仍 `#include "legacy/glass_effect.h"`** | **"移出编译"成立；"完全废弃/无人引用"不成立**：头文件仍被一个参与编译的翻译单元 include（`main_window.cpp` 内无 glass 符号，命中 0，所以只是 include 未使用） |
| 6 | §1.1 六条"下层 → gui 引用数 = 0"（§1.1 表） | 按 `src/gui/*.h` 的 17 个头名做 include 扫描（V8） | core **0** · storage **0** · utils **0** · protocols **0** · cli **0** · host **0** | **今日复核仍成立**。对照：app **5**、model **1** |
| 6b | §1.1 结论"没有'下层回头依赖上层'的倒挂"（§1.1 结论行） | 同 V8，再看非零明细（V12） | app 5 条**合规**（§2 的铁律允许 app 层持 gui 引用）；model **1 条不合规** | 当时**不成立**：`src/model/task_list_model.cpp:2` → `src/gui/app_icons.h`（`fileTypeIcon` 画文件名列图标）。阶段 1 搬迁后才出现，写 §1.1 时该文件还在 `gui/` ⇒ 当时为真。**本轮（同日 `d7a89ca`）已修**：模型改发 `FileTypeRole`，图标在 gui 层查表；V8 重跑 model=0，且这条判据从此由 `tools/lint_layering.py` 常驻把关（见 §7 第 7 条） |
| 7 | `cfg.max_concurrent` 在引擎里"只被写入、无人读"（§7 第 1 条「改了什么」1.） | `grep -rn max_concurrent src/ tools/`（V9） | **8**，其中唯一读者 `src/core/download_core.c:550 concurrent_limit()` | **作为历史陈述成立、作为现状不成立**：`ef1fb7d` 起已有人读。原文有"以前"限定，保留；引用时须知道它已过期 |
| 8 | 探针产物目录"仓库内零脚本依赖"（§7「（续）」的"仍然没验证"2.） | `grep -rn idm_shots --include=*.py --include=*.yml --include=*.sh --include=*.ps1 .`（V10） | **4**，全部落在 `tools/probe_artifact_inventory.py`（3 条是它自己的注释/名字族匹配逻辑 = 自指，1 条正文描述旧默认值） | **排除自指后 0 ⇒ 成立**。同一条自己留的那句"别的会话/手工习惯是否还在往这些目录写"**本机无账本可查，仍未核实** |
| 9 | 重构前**无 `.git`**（§1「版本控制」行） | `git log --reverse` 取首笔（V11） | 首笔 `874427e`，日期 2026-09-19 | **半核实 → 降级**：只能证明"09-19 之前没有提交"，证明不了"当时目录里没有 .git"（也可能被删过）。可核写法是"首笔提交在 2026-09-19" |
| 10 | "零逻辑改动 / 纯移动零风险 / 零视觉变化 / 渲染零差异"（§3.2 标题、§4 阶段 1 的标题与验收行、阶段 2-3 各段） | 非引用类：要靠 2026-09-19 那次的 A/B 构建与 md5 对照复现 | — | **本轮未核实**：那批 md5/PNG 对照的原始产物已不在（截图从未入 git），无法复跑。保留原文，但按"当时记录"读，不当作今天的可核事实 |

取证块（**命令逐字可复跑**，在仓库根执行；输出为当次原文。`←` 与 `（）` 内是我加的注释；
`ls` / `wc` 这类单值结果写成"→ 值"的形式）：
```
V1 $ grep -rl "glass-ui" --include=*.md --include=*.cpp --include=*.h .
   ./docs/ARCHITECTURE.md
V2 $ grep -rn "glass-ui" src/ CMakeLists.txt tools/ | wc -l
   0
V3 $ grep -rIn "idm-next-glass-ui" . | grep -v '^./.git/' | cut -d: -f1 | sort | uniq -c
         1 ./docs/ARCHITECTURE.md
V4 $ grep -c "glass_effect\|MainWindow\|\.cpp" idm-next-glass-ui.html
   0
V5 $ ls src/core/network.c                        → No such file or directory
   $ wc -l src/core/legacy/network_winhttp.c      → 874
   $ grep -c network_winhttp build/build.ninja    → 0
V6 $ grep -c glass_effect CMakeLists.txt          → 0
   $ grep -c glass_effect build/build.ninja       → 0
   $ ls build/CMakeFiles/idm-next.dir/src/gui/glass_effect.cpp.obj
     → 存在，36000 字节，2026-09-14 08:11（搬迁前遗留）
V7 $ grep -rn "glass_effect" src/ --include=*.h --include=*.cpp
   src/gui/legacy/glass_effect.cpp:1:#include "glass_effect.h"
   src/gui/main_window.h:10:#include "legacy/glass_effect.h"   // 玻璃拟态特效（已停用，留档…）
   （另：grep -n "glass\|Glass" src/gui/main_window.cpp → 0 命中 ⇒ 只 include 未使用）
V8 $ HDRS=$(find src/gui -name '*.h' -printf '%f\n'|sort -u); for d in core storage utils \
       protocols cli host app model; do printf '%-9s %s\n' "$d" \
       "$(grep -rh '#include' src/$d | grep -F -w -f <(echo "$HDRS") | wc -l)"; done
   core      0
   storage   0
   utils     0
   protocols 0
   cli       0
   host      0
   app       5
   model     1
   （同一批命令里另跑 find … | wc -l ⇒ gui 头 17 个，是被匹配的名单规模）
V9 $ grep -rn "max_concurrent" src/ tools/
   src/core/download_core.c:550:    return g_cfg.max_concurrent > 0 ? g_cfg.max_concurrent : 0;   ← 唯一读者
   src/core/download_core.c:1300:        g_cfg.max_concurrent     = 3;
   src/core/download_core.c:2114:    g_cfg.max_concurrent  = cfg->max_concurrent;
   src/core/download_core.h:147:    int max_concurrent;     /* 同时下载的任务数 */
   src/utils/settings.cpp:152:    cfg.max_concurrent     = m_maxConcurrent;
   tools/engine_selftest.c:2082:        cfg.max_concurrent = A_LIMIT;
   tools/engine_selftest.c:2141:        cfg.max_concurrent = B_LIMIT;
   tools/engine_selftest.c:2163:        cfg.max_concurrent = 3;
V10 $ grep -rn "idm_shots" --include=*.py --include=*.yml --include=*.sh --include=*.ps1 .
    ./tools/probe_artifact_inventory.py:6:`C:/Users/yyyy/idm_shots`，配合"每轮临时换一个新 IDM_SHOT_DIR / 顺手 tee 一份构建日志"
    ./tools/probe_artifact_inventory.py:137:    """按名字族（idm_ui_vN / idm_shots*）分组并比较内容。"""
    ./tools/probe_artifact_inventory.py:144:        elif name.startswith("idm_shots"):
    ./tools/probe_artifact_inventory.py:145:            key = "idm_shots*"
    （4 条命中全在这一个文件 = 检查工具自身的注释与名字族逻辑；再 grep -v 该文件 → 0）
V11 $ git log --reverse | head -1
    874427e chore: 建立工程基线（v0.1.0 源码 + 工程规范底座）  （2026-09-19）
V12 $ HDRS=$(find src/gui -name '*.h' -printf '%f\n'|sort -u)
    $ grep -rn "#include" src/model src/app | grep -F -w -f <(echo "$HDRS")
    src/model/task_list_model.cpp:2:#include "app_icons.h"   // fileTypeIcon：文件名列的类型图标
    src/app/appearance_controller.cpp:11:#include "progress_delegate.h"
    src/app/appearance_controller.cpp:12:#include "sidebar_panel.h"
    src/app/appearance_controller.cpp:13:#include "app_icons.h"
    src/app/queue_scheduler.h:10:#include "queue_manager.h"     // DownloadQueue（队列定义）
    src/app/task_controller.cpp:9:#include "queue_manager.h"
```

本轮**没做**的（按用户指令）：`idm-next-glass-ui.html` 一个字节没动、没搬、没进 `.gitignore`、
没删 —— 去向仍等拍板，这条不算已处理（`git status -sb` 里它仍是 `??`）。
第 13 步的 3 个红本轮**不收**，仍按第 5 条的门槛写作**未闭合**。

### 2026-09-29（第 7 条）— 分层门常驻闸门；model→gui 那条反向依赖按「模型只发数据」修掉

用户从三条候选里选了 (a)：`model` 不发图标、只发数据，view/delegate 侧查表换 `QIcon`。
(b) 把 `app_icons.h` 下移到 model 被否（那是把越界合理化），(c) 登记豁免被否（后来人会以为这是设计）。

**先立门，再改代码**（`tools/lint_layering.py`，纯标准库；层号与许可都取自本文 §2/§2.1）。
改代码之前的基线输出（`46461b4`，恰好只有那一条，且用临时文件自证不是硬编码）：
```
$ python tools/lint_layering.py
[layer-gate] 发现 1 条「低层反向依赖高层」：
  VIOLATION model/task_list_model.cpp:2  include "app_icons.h" → gui/app_icons.h  (L1 model → L4 gui)
LAYER_GATE_RC=1
```
改完之后（同一道门，未放宽任何判据）：
```
[layer-gate] 通过：扫了 210 条引号 include，反向依赖 0 条；文档许可的反向引用 10 条
             （L2 app → L3 protocols ×5 · L2 app → L4 gui ×5；明细见 --count-allowed）
LAYER_GATE_RC=0
```
第一版门曾报 6 条，多出的 5 条 `app→protocols` 是**我的数字比较比文档更严**（§2.1 明文允许
L2 依赖「L3（类型）」），不是真越界；许可对登记在 `SANCTIONED` 并用 `--count-allowed` 逐条打印，
不做静默豁免。接进 CI 的第一步（在工具链之前，一行文本就能判），yml 里留了配置例外说明。

**代码**：`TaskListModel` 新增 `FileTypeRole`（第 0 列返回 `detectFileType` 的 int），
删掉 `Qt::DecorationRole` 那一支；`AppIcons::fileTypeIcon` 改收 `FileType`；
新增 `AppIcons::FileTypeIconDelegate` 在 `initStyleOption` 里把键翻成图标；MainWindow 装到第 0 列。

⚠ **记一条 Qt 细节，它会静默吃功能**：光设 `option->icon` 画不出图标。`QCommonStyle` 的
`drawItemData` 判的是 `QStyleOptionViewItem::HasDecoration` 特征位，而基类只在「模型给了
DecorationRole」时才置它。所以第一版编译过、`initStyleOption` 确实被调（现场打印
`iconNull=0 deco=16x16`）、退出码也 0，界面上文件名列的图标却**全没了**（文字左移到图标位）。
必须连 `features |= HasDecoration` 与 `decorationSize = icon.actualSize(...)` 一起补。

**测具**（`IDM_FILEICON_PROBE`，第一版 5 条、同日补到 8 条，已接进 CI 的 UI 探针闸门；
接进去之后闸门是 15 个模式共 62 条）：
[3] 的判法是「装生产委托的单元格」与「模型直接给 DecorationRole 的控制表格」**逐像素相同**——
控制表格复刻的正是改前那条通路，所以这条相等就是"用户看到的没变"，且不依赖 PNG 金样。
两次变异各只打红该红的那条：
```
删 features |= HasDecoration 那行 → [3][4] FAIL，rc=1（其余三条仍 PASS）
模型恢复发 DecorationRole        → 只有 [1] FAIL，rc=1
```

**改前/改后视觉对照**（同一任务列表、同一组 7 个扩展名；走查图 `02_main_tasks` 取
文件名列区域 `240,140..602,790`；同构建重跑两次做噪声基线，该区域差异像素 0）：
```
before1 sha1 = d7fd23d603b298d9d9108fde2d6c6f36072d0cc6   ← 改前
before2 sha1 = d7fd23d603b298d9d9108fde2d6c6f36072d0cc6   ← 改前重跑（噪声基线，差异 0）
after1  sha1 = ea5002641265ef8c911d4704d0d7dd99a9bddfb7   ← 第一版（缺 HasDecoration）：差异 7165 像素
after2  sha1 = d7fd23d603b298d9d9108fde2d6c6f36072d0cc6   ← 修好：与改前**逐像素相同**，差异 0
```
也就是说：A/B 既能分辨"真坏了"（7165 像素），也对"修好了"给出与改前同一哈希——
这条不是我声称"没变化"，是同一区域两次哈希相等。

**补（同日第二轮，用户复核后）**：门与代码压成**一笔** `9f04742`（原来分三笔会让中间提交红在门上），
还原点 `backup/pre-squash-68e38de`；探针从 5 条加到 **8 条**，门加了两处可核性：
根路径改从 `__file__` 推（原来靠 cwd，跑错目录会"扫到 0 个文件"而退 0＝永远绿的假门），
许可对钉上限（`app→gui ≤5`、`app→protocols ≤5`），`--count-allowed` 从"给人看的清单"变成有牙的判据：
```
临时在 src/app/auto_power.cpp 加第 6 条 app→gui：
  OVERCAP app → gui：6 条，上限 5 条 —— 许可对被扩大了。…  → EXIT=1
还原后                                                   → EXIT=0
```
新增的第 [7] 条（六个类型两两不同）是这轮真正的回归网，因为**像素对照抓不到静默降级**：
```
删掉 case FileType::Compressed: 之后
  [fileicon] 类型 Compressed 与 Other 画出来一模一样（switch 漏了 case？）
  [FAIL] 六个取值的图标两两不同（漏 case 会撞成同一个）
  [PASS] 装了生产委托的单元格与改前通路逐像素相同      ← 注意这条照样绿！
```
控制表格用的是同一张坏表，所以"与改前一致"式的断言对漏 case 完全无感；只有逐类型点名能抓到。
第 [8] 条钉 `FileType::All`（筛选器的"全部"档，10 种输入下都不会被 `detectFileType` 返回；
真返回时 default 画成 OtherFile，用户看到"未知类型"而不是空白）——所以它不参与 [7] 的互异比对。

A/B 取证已挪到 `%TEMP%\idm-probes\2026-09-30\`（`before_46461b4/` 是改前那两次的图，
`after_9f04742/run1|run2/` 是改后当前构建的两次）：文件名列区域三张图 sha1 全等于
`d7fd23d603b298d9d9108fde2d6c6f36072d0cc6`，两两差异像素 0；整图 sha1 不同（差异在速度/状态列，属噪声）。

验证命令与实际输出：
```
cmake --build build
  → 三个改动文件都出现对应的重编行（步号随增量构建范围变化，这里只记文件名）：
    Building CXX object CMakeFiles/idm_app_lib.dir/src/gui/app_icons.cpp.obj
    Building CXX object CMakeFiles/idm_app_lib.dir/src/model/task_list_model.cpp.obj
    Building CXX object CMakeFiles/idm_app_lib.dir/src/gui/main_window.cpp.obj
    （⚠ 中途一次我把构建输出 grep 掉了，结果构建其实失败、我读的是陈旧 exe，结论作废重做）
python tools/lint_layering.py                       → EXIT=0（改前同一道门 EXIT=1，见上）
python tools/lint_layering.py --count-allowed       → app→gui 5/5 · app→protocols 5/5（去重后仍 5）
bash --noprofile --norc -eo pipefail <UI probes 步骤原文>
  → 15 个模式 rc 全 0，「UI 探针全绿：15 个模式共 62 条断言」（下限 ≥50）
./build/engine_selftest.exe 18190 idmuser idmpass   → 结果：142 通过，0 失败（未受影响）
```

仍然没验证 / 没做的：
0. **一次 `IDM_FORWARD_PROBE` 的红没定位**：套件某轮它红，之后 1 次单独 + 2 次全量都绿（5/5、rc=0），
   当时我只 `tail -6` 没留全量日志，**那次红的原文已经拿不回来了**。本机当前无残留
   `idm-next.exe`/`ui_snapshot.exe` 进程。它与第 13 步属同一家族（单实例 IPC 的时序/残留），
   不记为"已排除"。CI 那侧失败会打 `tail -25`，下次红就有现场。
1. **真机界面上的观感**：像素对照只覆盖"与改前是否一致"，不等价于"人眼看着对"。
   要看界面请用户本人跑一次（`build/idm-next.exe`，本轮没装机、没打包）。
2. 分组模式（按类型/按队列）下的分组头行：`FileTypeRole` 在头行返回无效 ⇒ 委托不画，
   与改前行为一致，但**这条我只在离屏单元格里验了非分组路径**，分组视图没进断言。
3. `app→gui` 那 5 条许可里，`appearance_controller.cpp:12` 依赖的 `sidebar_panel.h` 是 QWidget 子类，
   与 §2.1「L2 禁止持有 UI 树」相抵触——门按 §2 的括号放行了，张力留在工具注释里，本轮没动。
4. 第 13 步仍**未闭合**（门槛：连续 14 轮 step 13 全绿）。

### 2026-09-30 — D 段那条断言不是"偶发红"，是**掷硬币**（6/10，门槛落在分布体内）

同一份二进制（`build/ui_snapshot.exe`，离屏，无 GUI 窗口）连跑 10 次 `IDM_ACTIVATE_PROBE`，
D 段「事件循环单轮最长」读数（ms）：

    515  219  214  208  533  509  650  2558  2500  218

- **超 500ms 门槛：6/10**（上一轮 8 次是 3/8；合起来 **9/18**）
- min **208** · 中位 **509** · max **2558**
⇒ 中位数就压在门槛上 ⇒ 这条断言**不是尾部偶发，而是门槛切在分布体内**：拿它当闸门，
CI 约一半概率红。之前写"3/8 超门槛、未定位"仍然成立，但**性质要说清**：
不是"偶尔抖一下"，是"这条判据目前没有区分能力"。
⇒ 两条高读数 2500/2558 ≈ **200ms 宽限的 12~13 倍**，与"N 条半开连接串行、每条各付一次宽限"
这个机制一致（不是机器噪声的形状：噪声不会刚好落在同一个倍数上）。
**未做**：没为变绿放宽门槛、没改判据。下一步要么把"总成本"也封顶（而不是只封单条），
要么把判据换成"排队请求的**总**等待"而不是"单轮最长"。

**红演示已做（2026-09-30）**：把 `dark.qss` 主按钮块的字色从 `#ffffff` 改成 `#18191c` 后，
`python tools/contrast_matrix.py` 两道各咬一次 ——
`dark: #18191c on #2f6fe0 = 3.74 < 4.5 块=QToolButton#primaryToolBtn`（GATE-A）与
`dark: QToolButton#primaryToolBtn 块里的实际配对不是 #ffffff on #2f6fe0，而是 [('#18191c','#2f6fe0')]`（GATE-B），
`RED_EXIT=1`；`git checkout -- resources/qss/dark.qss` 还原（`git status --porcelain resources/` 空、
sha256 由 `cc5b2cc6631f` 回到 `c4632d38b6b5`）后 `GREEN_EXIT=0`。
**「从未红过 ⇒ OK 不可信」这条横幅因此摘除。**
**残余盲区（横幅摘了但它还在）**：GATE-A 只覆盖「同一个块里自己声明了 background 的文字」；
文字底色来自**祖先块**的配对（例如 `QLabel` 无自身 background，坐在窗体上）不在覆盖内 ——
那需要控件→表面的包含关系模型，仍未建。

### 未开工，入参写定于 2026-09-30 —— D 段要量的不是「总等待」，而是「这一轮的时间是谁花的」

**今日状态**：`ahead 24`、`contrast_matrix` 的「OK 不可信」横幅**已摘**（红演示见下）、
`?? idm-next-glass-ui.html` 等用户出清单批（未动未删）、不 push／不 tag／不改版本号。

**只读代码得到的三句**（`src/app/ipc_server.cpp`）：
① 取尽循环在 `:83 while (m_server->hasPendingConnections()) handleClient(client)`，
   宽限在 `:105 if (data.isEmpty() && i > 1) break;` + `:107 waitForReadyRead(100)`
   ⇒ **每条连接每周期最多付 ~200ms，且 `handleClient` 每条只被调一次**（没有"同一条连接同一周期
   多次付宽限"的路径，至少在这两个函数里没有）。
② 于是 2500ms 反推 **N ≥ 12**（2500/200）。**「N 恒 1 ⇒ 总量判据无意义」这个分支已经被旧数据否掉了**
   —— 前提是时间真花在这条路径上。
③ 但 ② 的前提**没有被证明过**：D 段量的是「单轮 `processEvents()` 最长」，**这一轮里任何别的事
   （重绘、定时器、别的槽函数）都算在里面**。所以 2500ms 有三种可能，不是两种：
   (a) N≥12 条半开连接串行；(b) 少数几条 + 别的东西占了大头；(c) 上一段（C 段 50 次往返）的残留
   被 D 段算进来了 —— **代码里 D 段之前没有"排空 + 静默"这一步**，而 `hasPendingConnections()`
   何时转假取决于 Qt 内部 accept 队列时机，**这一条读代码读不出来**。

**⇒ 下一会话的入参（顺序与约束都定死，不要照我上一版那三段执行，那版已被 ②③ 改形）**：
1. **先插"排空 + 静默"段**（D 段开始前把 pending 取尽并静置一个可说明的时长），否则测的是残留，
   测具在测自己上一段造的东西。
2. **打印归因，不只打印时长**：同一轮里分别记 `handleClient 调用次数=N`、`宽限总耗时=Σms`、
   `本轮其它耗时=单轮最长 − Σ`。三个都要印，且**变异范围**要能非恒（N 至少出现过 >1、
   其它耗时至少出现过 >0），否则字段是装饰（今日三仓各自撞过"写了字段但从未响"）。
3. **判据换向**：主判据应是 **`宽限总耗时` 或 `N`（串行宽度）**，而不是"单轮最长"——
   后者混进了 (b) 类噪声，正是它现在中位数压在门槛上、变成掷硬币的原因。
4. **T 不许取样本 max**：`T = max(样本) + 余量`，余量要理由（例如按 N 的上界 × 单条宽限推导），
   并把推导写进注释；否则就是把同一个错从"单轮"搬到"总量"。
5. 全程**先只量、不改断言、不放宽 500**；量完拿着 N 的实测分布再来定要不要换判据。
