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
| 死代码 | 2026-09-19 当时：`core/network.c`（WinHTTP 874 行）**未编译**；`gui/glass_effect.cpp` 已停用但**仍在编译**。2026-09-29 复核：两个路径都已搬迁（现 `src/core/legacy/network_winhttp.c`、`src/gui/legacy/glass_effect.cpp`），`glass_effect.cpp` **也不再编译**（`grep -c glass_effect build/build.ninja` = 0，原表这一格已过期）；但 `src/gui/main_window.h:10` 仍 `#include "legacy/glass_effect.h"` ⇒ "完全断开"不成立。逐条命令与命中数见 §7「否定式断言全表核」；**活/死名单与复算命令见 §1.3（有门）** |
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
现在这条结论由 `tools/lint_layering.py` 守着（**入口已接在 CI，未执行过** —— 见 §7「CI 执行状态」）。详见 §7 第 7 条。）
真正的结构性问题不是方向，而是**①上帝类（main_window 承担了应用编排）②文件归位错误③死代码入编④构建清单重复**。

### 1.2 边界违规（文件摆错层）

| 原文件（阶段 1 之前） | 原违规 | 现在位置（2026-10-01 实测） | 状态 |
|---|---|---|---|
| `gui/web_server.{h,cpp}` | Web 服务是**基础设施**，不是视图 | `src/web/web_server.{h,cpp}`；控制器在 `src/app/web_server_controller.{h,cpp}` | 已归位 |
| `gui/task_list_model.{h,cpp}` | 数据模型层 | `src/model/task_list_model.{h,cpp}` | 已归位 |
| `gui/category_filter_proxy.{h,cpp}` | 代理模型，数据层 | `src/model/category_filter_proxy.{h,cpp}` | 已归位 |
| `gui/task_group_proxy.{h,cpp}` | 代理模型，数据层 | `src/model/task_group_proxy.{h,cpp}` | 已归位 |
| `core/network.c`（WinHTTP 参考实现） | 未编译却躺在 `core/` 顶层，读起来像活的 | `src/core/legacy/network_winhttp.c` | 已归位（未编译，见 §1.3） |
| `gui/glass_effect.{h,cpp}` | 停用功能留档 | `src/gui/legacy/glass_effect.{h,cpp}` | **半归位**：文件已移出编译，但 `src/gui/main_window.h:10` 仍 `#include "legacy/glass_effect.h"` |

复算命令**两个方向都要成立**（新路径存在 **且** 旧路径不存在）：

```bash
for p in src/web/web_server.cpp src/model/task_list_model.cpp src/model/category_filter_proxy.cpp \
         src/model/task_group_proxy.cpp src/core/legacy/network_winhttp.c src/gui/legacy/glass_effect.cpp; do
    [ -f "$p" ] && echo "LIVE $p" || echo "★MISSING $p"; done
for p in src/gui/web_server.cpp src/gui/task_list_model.cpp src/gui/task_list_model.h \
         src/gui/category_filter_proxy.cpp src/gui/task_group_proxy.cpp src/core/network.c src/gui/glass_effect.cpp; do
    [ -f "$p" ] && echo "★STILL-AT-OLD $p" || echo "GONE $p"; done
grep -rn "legacy/glass_effect.h" src/ | grep -v "^src/gui/legacy/"
```

本轮实测：前一个循环六行全 `LIVE`，第二个循环七行全 `GONE`，最后一条 `grep` 命中 **1 行**
（`main_window.h:10`）。那一条是这张表里唯一还开着的项，措辞要准：**不是"归位没做完"，是"include 没清"**——
`main_window.cpp` 里 glass 符号命中 0，所以它是只声明未使用的残留；判"已移出编译"要看 `CMakeLists.txt`
与当前 `build.ninja`，**不要看旧 `.obj`**（搬迁前的 `build/CMakeFiles/idm-next.dir/src/gui/glass_effect.cpp.obj`
是遗留物，拿它当"仍在编译"的证据会得出反的结论）。

> 这张表**是文档，不是门**：上面三条命令要人跑。`tools/live_roster.py` 只核 §1.3 那份"哪份是活的"名单
> （它的解析锚点是列名与「类别」四个取值），**不核这张表的归位状态**。所以不许把 `live_roster OK`
> 读成"1.2 的六行也验过了"。要升成门的形状与 §1.3 相同：判据从表里读、两个方向都断言、漂移即红。

### 1.3 哪一份是活的（名单 + 复算命令，漂移即红）

这一节存在的理由：仓里有**读起来像活的、其实一行都不进编译**的文件，也有**五份名字像新旧版、其实是并存分派**
的文件。以前"哪份是活的"只能靠反推 `CMakeLists.txt` 得出 —— 没登记、没门。现在登记在这里，旁边就是复算命令，
不一致由 `tools/live_roster.py` 报红（**入口已接 CI，未执行过**，见 §7「CI 执行状态」）。**列名与「类别」四个取值是门的解析锚点，改表必须同批改门。**

未编译集的复算命令（原样可跑，输出应当与下表 `未编译` 桶逐字相同）：

```bash
comm -23 <(find src tools -type f \( -name '*.c' -o -name '*.cpp' \) | sort) \
         <(grep -vE '^[[:space:]]*#' CMakeLists.txt | grep -oE '(src|tools)/[A-Za-z0-9_./-]+\.(c|cpp)' | sort -u)
```

```
src/core/legacy/network_winhttp.c
src/gui/legacy/glass_effect.cpp
```

命令里 `grep -vE '^[[:space:]]*#'` 不是装饰：**先把 CMake 注释行去掉再匹配**。少了它，谁在注释里写一句
`# network_winhttp.c 已停用`，该文件就会被当成"被 CMake 点名"而从未编译集里蒸发 —— 门反而把真陷阱洗白。
（本仓已入库的规矩：任何靠文本锚点工作的工具，输入必须先过去注释。）

| 文件 | 类别 | 判活依据（可复算） |
|---|---|---|
| `src/core/legacy/network_winhttp.c` | 未编译 | 874 行、不在任何 target；与活的 `src/core/network_curl.c` **对外符号同名 9 个**（`network_init`、`network_cleanup`、`network_set_http2_enabled`、`network_default_options`、`network_get_html`、`network_post`、`network_probe`、`network_download_range`、`network_download_range_fp`）⇒ 改网络行为只改 curl 那份，改这里不会生效也不会报错 |
| `src/gui/legacy/glass_effect.cpp` | 未编译 | 140 行、不在任何 target；`grep -c glass_effect build/build.ninja` = 0 |
| `src/gui/legacy/glass_effect.h` | 仅声明型死头 | 被 `src/gui/main_window.h:10` include ⇒ **声明进编译、实现不进**；`Glass::` 在去注释后的全 `src/` 调用点 = 0 ⇒ 谁去调 `Glass::` 就是链接错误。别把它读成"玻璃拟态还在用" |
| `tools/ui_snapshot.cpp` | 开关才编译 | `option(IDM_BUILD_UI_SNAPSHOT "Build offscreen UI snapshot tool" OFF)` ⇒ 默认构建里没有它是正常的，探针/截图要 `-DIDM_BUILD_UI_SNAPSHOT=ON` |
| `tools/engine_selftest.c` | 开关才编译 | `option(IDM_BUILD_ENGINE_SELFTEST "Build engine self-test (needs local auth HTTP server)" OFF)` |
| `src/protocols/idownloader.cpp、src/protocols/*_downloader.cpp` | 并存分派，不是两版 | 同一个名字家族 5 份、**同时进编译**：`src/protocols/protocol_factory.cpp` 按 URL 谓词择一分派（HLS → Torrent → Video → Http 兜底）的是 4 份实现；第 5 份 `idownloader.cpp` 不是"第 5 种下载器"，全文只有 `#include "idownloader.h"`，是给 header-only 的 `IDownloader`（带 Q_OBJECT）喂 AUTOMOC 的占位 TU ⇒ 5 份之间没有"新旧版"关系，不许"合并去重" |
| `src/protocols/video_backend.cpp、src/protocols/torrent_backend.cpp` | 并存分派，不是两版 | 名字与上一行只差一个词、职责不同：`*_backend` 管外部工具（yt-dlp / aria2 式）自检与自动下载，`*_downloader` 才是 `IDownloader` 实现。两份都在用，调用点 `src/gui/main_window.cpp:342` 与 `src/gui/main_window.cpp:351`；分派点 `src/gui/main_window.cpp` |

门（跑这个，退出码 1 = 表与集合不一致）：

```bash
python tools/live_roster.py
```

**范围**：只覆盖 `src/` + `tools/` 的 C/C++ 翻译单元（磁盘 59 个 TU = 编译 57 + 未编译 2，恒等式由门自己印）。
另有三处也会让人读错"哪份是活的"，不进表，各给一句判据：`fdm-ref/` 是外部参考树，`.gitignore:38` 已忽略，
`git ls-files fdm-ref | wc -l` = 0 ⇒ 它 617 个 `.c/.cpp` 一份都不跟踪、也不进编译；`third_party/libcurl/`
是 vendored 依赖，由 `CMakeLists.txt` 的 `LIBCURL_*` 变量指路；`browser-extension/*.js` 不是 TU，本命令根本不涉及。

**残余盲区（明写，不当已覆盖）**：命令判的是"CMake 有没有点名"，不是"跑起来有没有用到"。头文件是否活要靠
包含图（所以 `仅声明型死头` 单列一类，由门做传递可达性判断），而"同名双定义 9 个符号"这类**人读错**的陷阱只能靠
这张表登记 —— 门能证明集合没漂移，不能证明依据列里那句话还成立；依据列的符号名改动不会被抓到。

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
- CI：工作流文件名以现算为准 —— `ls .github/workflows/` 当前只有 `ci.yml` 与 `release.yml`，**没有 `build.yml`**（本行原写 `build.yml`，是找不到的名字，2026-09-30 更正）。其中"引擎自测入闸门"这半句已落地（`ci.yml` 第 9 步，入口存在、**未执行过**，见本节末「CI 执行状态」）；剩下的候选是给三道文本门加覆盖率类检查。

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
不做静默豁免。入口已接在 CI（工具链之前，一行文本就能判），yml 里留了配置例外说明；**该步从未在 runner 上执行过**。

**代码**：`TaskListModel` 新增 `FileTypeRole`（第 0 列返回 `detectFileType` 的 int），
删掉 `Qt::DecorationRole` 那一支；`AppIcons::fileTypeIcon` 改收 `FileType`；
新增 `AppIcons::FileTypeIconDelegate` 在 `initStyleOption` 里把键翻成图标；MainWindow 装到第 0 列。

⚠ **记一条 Qt 细节，它会静默吃功能**：光设 `option->icon` 画不出图标。`QCommonStyle` 的
`drawItemData` 判的是 `QStyleOptionViewItem::HasDecoration` 特征位，而基类只在「模型给了
DecorationRole」时才置它。所以第一版编译过、`initStyleOption` 确实被调（现场打印
`iconNull=0 deco=16x16`）、退出码也 0，界面上文件名列的图标却**全没了**（文字左移到图标位）。
必须连 `features |= HasDecoration` 与 `decorationSize = icon.actualSize(...)` 一起补。

**测具**（`IDM_FILEICON_PROBE`，第一版 5 条、同日补到 8 条，入口已接进 CI 的 UI 探针闸门、**该探针从未在 runner 上跑过**（见本节末「CI 执行状态」）；
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

**今日状态（`ahead` 是活数字，别信字面量）**：现测以
`git log origin/master..HEAD --oneline | wc -l` 为准（2026-09-30 主控巡视那轮 = 30，本笔提交后会变 31 ——
原写死的 `ahead 24` 就是这类会随交接传下去的陈旧数，已删）；`contrast_matrix` 的「OK 不可信」横幅**已摘**（红演示见下）、
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

### 2026-09-30 — 那 1.5~2s 的排查：归因测具已就位，但**未复现 ⇒ 仍未定位**

按 `a6afef8` 入参做了 D 段测具改造（`dbbf312` + 本轮），**断言与 500ms 门槛一律没动**：
`IpcServer` 加只读测具量（一次取尽几条 / 取尽自身耗时 / 历史最大），探针 D 段加
「排空 + 静默」与 A/B/A 消融（`IDM_ROUND_ATTR=1` 才跑）。

**读数（带分母）**：
- 早先 4 次：`peakN=1`、取尽自身 211~217ms、单轮最长 **1708~2071ms** ⇒ IPC 只占 **10~13%**
- 修好消融之后 6 次：单轮最长 **214~218ms**、IPC 占 **100%**、复现率 **0/6**
- 再往前 10 次（`8b139e3` 记的）：6/10 超 500ms
⇒ 合起来 **6/16 超门槛，且当前状态下不复现**。那 1.5~2s 是**间歇的**，不是每条请求的固定成本。

**两次我自己造的假读数（都靠"荒谬结果"暴露，不是靠它绿）**：
1. 消融窗口放在 D 段**之后** ⇒ 空转连接早被处理完，之后无论停哪个定时器都是 0ms，
   **4 个定时器全被判成"元凶"**。正解：消融必须**重开一份被测事件**（`sampleWithIdle()` 自己
   新建空转连接），不能拿"事件已过去"的空窗口当对照。
2. 上面那版"停掉全部定时器 ⇒ 0ms"因此同样作废 —— 我当时差点把它当结论写。
⇒ 现在测具在**未复现时拒绝下结论**（打印"本轮未复现 ⇒ 跳过消融"），复现时才做 A/B/A。

**仍然没定位**。下一步不是继续猜，而是等它复现：测具已armed，复现那一轮会自动给出
「有定时器 / 停定时器 / 再有定时器」三个数。另一条待排除的外因：这台机器上有别的会话/进程时，
计时证据会被污染（同机干扰），复现率从 6/10 掉到 0/6 与"我改了测具"无关（改动都在测量**之后**执行）。

验证：`bash <UI probes 步骤原文>` → 15 模式 rc 全 0、62 条全绿；`lint_layering` EXIT=0；
构建有 `Building CXX object .../ipc_server.cpp.obj` 与 `.../ui_snapshot.cpp.obj` 两行。

### 2026-09-30（再续）— 「哪一份是活的」进了名单，并且配了一道会咬的门

起因是用户实跑点出的那条：`src/core/legacy/network_winhttp.c` 874 行**一行都不进编译**，却与活的
`network_curl.c` 对外符号同名 9 个 —— 这种文件的存在方式就是"读代码的人会改错那一份，而且改了不报错"。
名单落在 §1.3，判据按用户给的条件：**表旁边就是可跑的复算命令，且"表里列的未编译集 == 命令算出的集"，不一致就红。**

三笔：`6421a66`（§1.3 表 + `tools/live_roster.py` + `tools/mutation_fixture_live_roster.py` + 夹具登记）、
`4ffeffa`（CI 第 3 步的**入口**，该步未执行过，见下「CI 执行状态」）、本笔（台账）。门与表放在同一笔里，是为了不留下"门红着提交"的中间状态。

**先红后绿（原文）**：

```
[RED] ROSTER-ANCHOR: 文档里找不到 §1.3 这一节（表被删了/被注释了/编号改了）
[RED] ROSTER-ANCHOR: 未编译集现算 2 条：src/core/legacy/network_winhttp.c, src/gui/legacy/glass_effect.cpp
[RED] ROSTER-ANCHOR 是**红**，不是跳过：本门退出码 1
live_roster EXIT=1 cost=22ms
```

加完表之后：

```
TU 恒等式：磁盘 59 = 编译 57 + 未编译 2  [OK]
表行数=7（未编译桶 2）｜现算未编译=2：src/core/legacy/network_winhttp.c, src/gui/legacy/glass_effect.cpp
live_roster OK: 表与复算集合一致（未编译 2 条全部登记，四个类别全部建模）｜成本 53ms
```

**九道变异全按预期**（`python tools/mutation_fixture_live_roster.py` → `10/10 道按预期（红/绿）`，
完整 stdout 在 `%TEMP%\idm-probes\2026-09-30\live_roster\mutation-*.log`）：M1 漏登记 MISSING、
M2 标错死活 LIVENESS、M3 名单过期 STALE、M4 类别没登记 UNKNOWN-CATEGORY、**M5 两面**（往 CMakeLists
加一行注释点名死文件 ⇒ 门仍绿，而不去注释的 naive grep 把它洗白 ⇒ 证明 `grep -vE '^[[:space:]]*#'` 承重）、
M6 依据列删符号名 BASIS、M7 整表被 HTML 注释藏起 ANCHOR、M8 开关默认翻 ON OPTION-STALE、M9 表头列名被改 ANCHOR。
还原核对：**干净（逐字一致）**，还原后再跑门 rc=0。

**这一轮我自己错过的两处，都写在这儿而不是只写结论**：
1. **M6 第一次跑是绿的** —— 依据列里删掉 `Glass::` 之后，"调用点 = 0"这条检查没有对象，静默跳过，门照样
   退 0。判据（`ROSTER-BASIS`）是看到这个洞之后才补的，补完同一道变异退 1。
   第一次尝试 M6 还不算变异：那行里另有一处 `Glass::enableGlassWindow` 留下了符号名，输出里"调用点=0"
   那行照印 ⇒ 靠这条尾巴认出空变异。
2. **M7 第一次的红是插错地方换来的** —— 夹具用 `| 文件` 定位表头，而 §1.2 的表头也叫 `| 文件 | 现在位置 |`，
   于是注释插到 §1.2 上、§1.3 的表还活着，门退 0，读起来像"这条判据咬不动"。定位串换成 `| 文件 | 类别 |` 整串
   之后才拿到真的 ROSTER-ANCHOR 红。
⇒ 共同教训：**跑错的夹具比不跑夹具更容易让人相信门是好的**；夹具自身也要有"锚点必须存在"的断言。

**新登记的数（都在 §1.3，且门只保证集合不漂移，不保证下面这些文字）**：
`src/` + `tools/` 共 **59** 个 TU = 编译 **57** + 未编译 **2**；winhttp 与 curl 同名对外符号 **9** 个；
`*_downloader.cpp` 名字家族 **5** 份里只有 **4** 份是下载器实现，第 5 份 `idownloader.cpp` 全文只有
`#include "idownloader.h"`，是给 header-only 的 `IDownloader`（带 `Q_OBJECT`）喂 AUTOMOC 的占位 TU；
`fdm-ref/` **617** 个 `.c/.cpp` 一份都不跟踪（`git ls-files fdm-ref | wc -l` = 0，`.gitignore:38`）。

**明写的降级与残余盲区**：
- 门判的是"CMake 有没有点名"，不是"跑起来有没有用到"；依据列里那句"同名 9 个符号""调用点=0"里的**文字**
  不会被证明，符号名本身由 M6 强制必须存在。
- 头文件可达性用 basename 兜底（对应 CMake 那 9 个 include 目录），同名头会让判断**偏松**（少报红方向）。
- 恒等式只覆盖 `src/` + `tools/`；`fdm-ref/`、`third_party/`、`browser-extension/` 不进表，§1.3 末尾各给了一句判据。
- §1.2 那张表我只加了"已搬完"的复算命令与指针，**没有重写整张表的"现在位置"列** —— 那是另一件事，没顺手做。

验证：`live_roster` EXIT=0、`lint_layering --count-allowed` EXIT=0（210 条 include，反向 0，许可 10 条 =
app→gui 5/5 + app→protocols 5/5）、PyYAML 解析 ci.yml 通过（windows-build 16 步，新步为第 3 步）、
夹具 10/10 且还原干净。本轮没动任何 C/C++ 源文件 ⇒ 没有新的 `Building CXX object` 行可贴，UI 探针套件不重跑。

### CI 执行状态（2026-09-30 主控巡视时核，写在这里防止被读成「CI 保住了」）

**判据（一条命令现算，别信本文件里任何写死的 ahead）**：
`git log origin/master..HEAD --oneline | wc -l` = 本轮实测 **30**，`origin/master = 4d7c4ab`。

- **入口已接、未执行过**：分层门（`46461b4`）、`IDM_FILEICON_PROBE`（`d7a89ca`）、名单门 `live_roster`
  （`6421a66` + `4ffeffa`）——这三件全在**未推的 30 笔里**，所以它们**一次都没在 runner 上跑过**。
  文档/提交信息里凡是读起来像"CI 会拦住"的措辞，一律按「入口已接，未执行过」理解。
- **确实执行过的**：UI 探针闸门与引擎自测这**两步本身**在 `90ac237`（已在 `origin/master`）那一轮
  runner 上按 step-level conclusions 核实为 success —— 但那时探针名单是 14 模式 / 54 条，
  不含 FILEICON，也不含上面三道门。**"这一步曾成功"不等于"这一步现在的内容曾成功"。**
- 本地已跑的等价验证（可复算，见上"验证"段）：两道门 + 夹具 + PyYAML。**这只证明脚本本身，不证明流水线。**
- **要把这三道门变成被验证的闸门，需要用户单独授权一次 push**（云端执行不是我能启动的动作）。



### 2026-09-30（第 3 条）— 主用户闭环第一次被量：`[32]` 三面，**当场红出一条"报完成、尺寸对、内容错、不报错"**

主控巡视问的第一件是「添加 URL → 落到磁盘 → 暂停/恢复/取消 → 重启后列表还在」这条闭环**被真跑过没有**。
读码结论先给：**引擎侧落盘早就有断言**（11 处逐字节比对 + 6 处尺寸/路径，具名见
`tools/engine_selftest.c:577/621/704/972/1000/1086/1291/1392/1451/1477/1724/1794/1921/1976/2035/2111`），
但**带进度的重启恢复一条都没有**：三条老 load 用例（[10]/[10b]/[11]）的状态文件写死
`"downloaded":0, "chunks":[]`（`:733-734`、`:788-792`、`:837-841`），而 `dlmgr_load_state()`
（`:1921-2079`）**从不核对盘上真实字节** —— 该函数体内尺寸类调用命中 0。
（`r+b`→`w+b` 那处兜底在 `:793-794` 与 `:1127-1128`，属**写入路径**，不在 load 里，别去 load 里找。）

补的 `tools/engine_selftest.c` `[32]` 三面（分工是用户定的，面 B 不许冒充面 C）：

| 面 | 动什么 | 结果 | 读数 |
|---|---|---|---|
| A | 盘没动，纯 save→load→继续 | **PASS** | 12,582,912 字节逐字节一致，`Range=24`（是续传不是整份重下） |
| B | load 前把 `.idmtmp` 截到 4096 | **FAIL** | `status=3 成品=12582912 内容=0 error_msg=空`，首个错位=4096，错位 9,395,506 字节 |
| C | load 前**删掉** `.idmtmp`（真走 `w+b`） | **FAIL** | 同上形状，首个错位=1，错位 9,399,585 字节 |

**机制不是猜的，是算术对上的**：`错位字节 = 漏检区长度 − 该区恰好命中 i%251==0 的位置数` ⇒
面 B `9,433,088 − 37,582 = 9,395,506`、面 C `9,437,184 − 37,599 = 9,399,585`，**两侧差都是 0**。
⇒ 错的字节**正好**是状态里 `done=1` 那两片（面 B 再加被截掉的头）：引擎信了状态文件，
一片没重新取回，也没往调用方送任何错误。用户视角是"下载完成、文件大小对、打开来是坏的"。

T3 与身份：2026-09-30 22:36:36→22:37:33，`build/engine_selftest.exe` mtime 22:36:31 /
`sha256[:12]=cce045f91ddd`，构建日志有 `Building C object …engine_selftest.c.obj` + `Linking` 两行
（**真重编**；上一轮 `ninja: no work to do` 时不得引用行为读数）。**结果 155 通过 / 2 失败**
（旧基线 142/0，本轮 +15 条）。S0 干净基线另计：22:19:51→22:20:41，142/0，
exe `4acc6689e385`。

**没做的、不记成已做的**：① S2（真·跨进程重启）本轮未做；② GUI 那一跑（`S3`）需要真人；
③ 三面收尾把自己产出的坏文件 `utf8_delete` 了 ⇒ **坏文件本体没留档**，留的是完整读数
（`%TEMP%\idm-probes\2026-09-30\loop-evidence\KEEP-s1e-selftest.log`）；④ 面 A 出现过一次
"20s 一个字节没下"（`deferred=0`，不支持"取消后槽位没归还"那个猜测）⇒ 属怀疑级，
只加了 `[前提读数]` 行进报告，**不进退码**。

**交回他定的一件事**：这条红要不要留在闸门里。它现在使本地套件从 142/0 变 153→**155/2**，
CI 第 9 步的判据（`N≥100 且 M=0`）若真去跑就会红 —— 但 CI 从未执行过（见上节），所以红只在本地可见。
按「不替产品代码做决定」的边界，`download_core.c` 一个字节我没动。

### 2026-09-30（第 4 条）— S2 真·跨进程重启：那条 P0 跨过了进程边界，以及我自己极性写反的一次

`1b858b9`。判据按「当场必须能演示会红」来做，**一面绿一面红在同一次跑里都给出来**：

- 面 A（state 与盘匹配）：子进程问题数 0、退码 0；父进程独立复核 12,582,912 字节**逐字节一致** ⇒ **绿**。
- 面 B（state 说 `done=1` 两片、`.idmtmp` 已删）：子进程 `status=3` 尺寸对、错位 9,399,585 字节、
  `error_msg` 空 ⇒ 退码 1 ⇒ 父进程那条"必须被妥善处理（退码==0）"**红**。
  算术从归档件复算：`done=1` 片数=2 ⇒ 漏洞区 0..9,437,183 ⇒ `9,437,184 − 37,599 = 9,399,585`，
  与子进程实测**逐字相等** ⇒ 与 `[32]` 面 C 是同一条机制，**不是同进程的假象**。

做法：`argv[1]=="--resume"` 时**同一个 exe** 走子进程分支（`dlmgr_init` + `dlmgr_load_state`，
手上只有两个文件），结论逐行写进 resultfile、退码=它数出的问题数；设 `IDM_SELFTEST_EVIDENCE_DIR`
就把两面的 state + result 各复制一份进证据目录（本轮四份，446/762/446/675 字节，
sha256 前 12 位 `4c2d435aca59 / f1af2d61b635 / 55dedb94c500 / fd437776563e`，
目录 `%TEMP%\idm-probes\2026-09-30\loop-evidence\s2-evidence-225723`）。

**两条我自己的错，都是这一笔里被抓出来的**：
1. **判据极性写反**：一开始写成"子进程必须发现不匹配 ⇒ 退码 != 0"，今天它确实非 0 ⇒ 我这条**绿**了 ——
   等于把当前缺陷钉成通过，谁把引擎修好这条反而会红。改成"这个组合必须被妥善处理 ⇒ 退码 == 0"。
   代码注释里写了"别改回去"和理由。
2. **变异根本没落地**：`utf8_delete()` 原来返回 `void` 并丢掉 `DeleteFileW` 的结果；而父进程在
   "已暂停"之后**仍在往文件写字节** ⇒ 句柄还开着 ⇒ 删除分享冲突失败 ⇒ 面 B 静默退化成面 A，照样全绿。
   现在 `utf8_delete` 交回 `BOOL`，并加两条断言钉住：「交出前父进程确实停了笔（状态=已暂停
   且尺寸不再增长）」与「面 B：不匹配是**真的**造出来了（删后 `.idmtmp` 必须为 -1）」。
   顺带一条**未下结论的新观测**：`state` 记 `downloaded=9,437,184` 时盘上 `.idmtmp` 已是整份
   12,582,912 ⇒ 任务级计数器落后于盘（只固定读数，没据此改任何判据）。

**S0 那两轮作废读数按主控要求留在账上，不删**（日期要给下一个读者）：
① 2026-09-30 22:17 那一跑：`preexisting_server=200` ⇒ 我上一轮起的 python 服务还活着，
**两个服务同占 18080**（Windows `SO_REUSEADDR`），那次"142/0"不引用；自己 CommandLine 核过
（`PID=12060 = …\engine_selftest_server.py 18080 …`）才终止，它的计数器停在 `conns=1, ranges=0`。
② 22:14 那一次 `cmake --build` 返回 0 而重定向日志 **0 字节**：真因是我的变异夹具把
`CMakeLists.txt` 还原成相同字节但推后了 mtime ⇒ cmake 顺手重跑并重生成了 `build/build.ninja`
（mtime 22:14:10），而**那次运行的输出一个字都没进我重定向的文件**。没有重编（exe mtime 未变），
但它是一次真实的"我的文本工具碰到人构建目录"的副作用。

T3 汇总：S0 干净基线 22:19:51→22:20:41 = 142/0（exe `4acc6689e385`，`ninja: no work to do` ⇒ 不引行为读数）；
`[32]` 三面 22:36:36→22:37:33 = 155/2（exe `cce045f91ddd`，真重编）；
`[33]` 两面 22:54:25→22:55:49 = **164/5**（exe `d38dd59b10de`，真重编）。
5 条红的构成：同一条 P0 的三个面（`[32]`B、`[32]`C、`[33]`B）+ `[32]` 面 A 的前提抖动 2 条
（"暂停时 0 字节"5 轮里 3 次，`deferred=0`，`[前提读数]` 已就位——**那是另一件事，没并进本笔判据**）。
产品代码 `src/` 一个字节没动。

### 2026-09-30（第 5 条）— `[33]` 那两条新断言补做变异；顺带答一条"进度读的是 state 还是盘"

`tools/mutation_fixture_restart.py`（可重跑）。**基线必须先绿**才允许谈变异：
`T3 23:09:00→23:10:02`，`ninja: no work to do`（⇒ exe 就是盘上源码），`结果：166 通过，3 失败`
（那 3 条就是 P0 的三个面，已知）。两道变异：

| 变异 | 怎么造 | 目标那条 | 该轮读数 |
|---|---|---|---|
| MUT-1 | 拿掉 `[33]` 的 `dlmgr_pause(id)` ⇒ 父进程继续写盘 | 「交出前父进程确实停了笔」**红 ×2**（`status=3 已下=…`） | 162/7，`T3 23:10:08→23:11:31` |
| MUT-2 | `deleted = utf8_delete(tmpp)` 改成 `deleted = 1`（把删除失败当成功） | 「面 B：不匹配是**真的**造出来了」**红** | `T3 23:11:37→23:12:39` |

每条都满足：锚点恰好命中 1 次、`mut != orig`、构建输出里有 `Building C object …engine_selftest.c.obj`、
起服务前 0 个监听/起后 1 个、跑完按字节还原（`git status tools/engine_selftest.c` 干净）且**还原后再编一次**
（否则盘上留着的就是变异体二进制）。完整 stdout 与红项清单各自落档：
`%TEMP%\idm-probes\2026-09-30\loop-evidence\restart-fixture-230856\MUT-*-verdict.txt`。
⇒ **这两条不再是"不占功"**：它们各自被证明会咬。（MUT-1 还顺手多红了 `面 B：不匹配是真的造出来了`
——不暂停时句柄一直被握着，删除必然分享冲突失败，正是 S2 第一次没落地的成因。）

**进度显示读的是 state 还是盘？都不是——它在完成那一刻被改写**（读码定，未进任何判据）：
暂停态显示的是 state 里那个数（`download_core.c:1989` 读回 → `task_list_model.cpp:75` 的
`downloaded * 100 / fileSize`），**没有任何一处去问盘**；而 `download_core.c:1212-1213`
在 `utf8_move` 成功之后直接 `status = TASK_COMPLETED; downloaded = file_size`。
⇒ 用户担心的"重启后长期看到 9.4/12.5 MB"这个形状**不成立**：点开始之后那串数会被 1213 抹平成 100%。
代价更糟：**进度条不是独立证人**——`done=1` 的片被跳过、文件只有 9.4 MiB 真字节时，
它照样显示 12,582,912/12,582,912。这是 P0 在界面上的表现，不另算一个缺陷。

`[32]` 面 A 的"新加任务 20s 零字节"**开独立一案，不并进 P0**：真实分母 = 修掉 `break` 误用之后的 5 轮里 3 轮
（22:33 / 22:49 / 22:54 红，22:36 / 23:09 绿）。**与 S0 那次作废读数不同族**，判据不是猜测：
同一次跑里 `[32]` 面 B/C 与 `[33]` 两面都在同一个端口上成功下到 9.4 MB ⇒ "两个服务同口"解释不了它
（那个形状污染的是**计数类**断言，不是"第一个任务拿不到字节"）。目前唯一站得住的相关性：
它只发生在 `[31]` 的 cancel+remove 之后**起的第一个**任务上，而每次 `dlmgr_load_state`（会 `reset_all_tasks`）
之后的任务都不发生（0/10+）。机理未定 ⇒ 不进退码，`[前提读数]` 行留着自动交代。

### 2026-09-30（第 6 条）— 撤回一条判读：那条"20s 零字节"不是产品抖动，是我的量具被自己的残留污染

主控按他给的代理量（`.idmtmp` 存在性 + 尺寸 + 时间，500ms 一采、印采样数当分母）落了 `tools/engine_selftest.c`
的 `[分桶]` 行，跑出来三件事：

1. **我先前报的"5 轮里 3 次"这个速率作废**。补齐清理名单之后，面 A 的现象是 **4/4 稳定复现**
   （23:39~23:43 连四轮，读数逐字相同）；之前"有时绿"的那几轮，差别在盘上残留让引擎收尾改名
   （`pick_free_filename` 挑中 `rst_a (1).bin`）撞没撞上 ⇒ **那个抖动本身是量具污染，不是产品行为**。
   成因具体：`clean_outputs()` 走的是**硬编码精确名单**（`:252` 起），我 `[32]/[33]` 的新文件名不在里面，
   而每轮收尾又只删得到原名 ⇒ 实测盘上攒出 `rst_a (1)..(5).bin` 五份 12,582,912 字节，
   我采的那个 `.idmtmp` 根本不是引擎在用的路径。已补：名单加上 `rst_*/s2_*/throttle_*`，
   并加 `sweep_leftovers()`（按通配 `FindFirstFileW` 扫，精确删除打不到改名产物）。
2. **补完之后现象仍在，而且更尖**：四轮里面 A 每次都是 `采样=40/从未创建=40/tmp最大=-1/
   downloaded 全程 0/status 区间=[0,1]/deferred 最大=0`，**而同一窗口内服务端 `/big.bin` 的
   GET 计数达到 12**（12 × 1 MiB = 整份）。同一轮后面那个"面 A 接得上并下完（逐字节一致）"却仍然通过。
   ⇒ 三个桶里它落在**桶 1b：字节被取回来了，但我按 `dir+filename+.idmtmp` 拼的那个路径从没出现、
   任务计数器也全程不动**。这不是"排队不推"（`deferred=0`），也不是"没发请求"（GET=12）。
3. **还没定因**，剩两个候选、只差一次采样就能分开：(i) 它写的是另一个路径/另一个槽位；
   (ii) 写入与 `downloaded` 上报解耦（下完了计数器才动）。为此要加的读数是
   `成品路径的最大尺寸` + `mid.filename / task_id 有没有被引擎填过` —— 我这次补丁的锚点没对上、
   脚本在写文件之前就退出（所以那四轮跑的二进制里**没有**这两列，别把它当成已采）。**已采到的只有上面那些。**

判据地位：`[分桶]` 与 `[前提读数]` 一样**只进报告行，不进退码**（机理未定 ⇒ 它没资格当护栏）。
`src/` 仍然零改动；套件计数在这一轮是 164/5（P0 三面 + 面 A 的两条前提），与 22:xx 的 166/3 并列，
**差别不在"抖动"而在残留**——这句也撤回我上一轮"差别只在抖动面"的说法。

### 2026-09-30（第 7 条）— 清扫曾在静默空转；修好之后"第一个任务"的现象仍在，且换了形状

**先记我自己的第二条"以为修好了"**：上一笔加的 `sweep_leftovers()` 里那行
`snprintf(pat, ..., "%s\%s", ...)` —— python 注入时 `\` 掉成一个反斜杠，C 里成未知转义 `\%`，
模式变成"目录和文件名粘在一起" ⇒ `FindFirstFileW` 永远匹配不到，**清扫静默零命中**，
盘上攒出 `rst_a (1).bin`、`(2)`、`(3)`（时间戳 23:57 / 23:59 / 00:01，正好三次停滞跑）。
形状与仓里老 bug `:2391 "%s\cap%d.bin"` 完全同一类（那条也顺手改了，`unknown escape` 少一条）。
⇒ 撤回上一笔里"4/4 稳定复现 ⇒ 量具已干净"的说法：那时量具确实没干净。

**给清扫配了会红的自检**（不是口头"我看过了"）：数 `rst_a*` 与 `s2_ok*` 通配，残留 >0 就红。
正例验证：跑之前手动摆 4 个残留（含一个 canary `rst_a (7).bin`）⇒ 该条 **PASS**，
跑完盘上只剩本跑自己产出的 `rst_a (1).bin` ⇒ 清扫真在删东西，自检不是装饰。
（这条判据的"会咬"是**用正例反证**的：模式写坏时它数到 4 ⇒ 必红；没再手工演一次坏模式，
所以严格说它是"有正例支撑、未做变异演示"，下一笔若要升档再补。）

**修好之后重采（`T3 00:05→00:06`，165 通过 / 5 失败）**：面 A 仍然 40 次采样全空
（`.idmtmp` 从未出现、`downloaded` 全程 0、status 区间 [0,1]、`deferred=0`、
`big.bin GET最大=12`），**但新加的 `成品路径最大=12582912` 说：那个窗口里 `rst_a.bin`
已经整份完整了**。⇒ 现象不是"没下载"，而是
**"字节已经全部落盘、成品就位，而任务对象既不报进度也不进终态 ≥20s"**；
`.idmtmp` 之所以采不到，很可能是它存在时间小于 500ms 采样间隔 ⇒ 这一列属**采样分辨率不足**，
我上一轮把它读成"桶1b：取了字节却没落到文件"是**读过头了，撤回**。

下一步要验的唯一候选（还没验）：**同一个任务上有两代写入者** —— 跑完盘上留下的是
`rst_a (1).bin` 而不是 `rst_a.bin`，说明收尾改名那一刻 `rst_a.bin` 已经被占；
这正好落在 [25]/[26] 那条"单代监督线程"的边界上。分开它只需要一次读数：
把 `dlmgr_get_task_info` 与 `server_stat(conns)` 在同一窗口内一起按 100ms 采（不是 500ms），
看 GET 与 tmp 是否出现"两拨"。判据地位仍是报告行，不进退码。

### 2026-10-01 — 清扫自检升档：变异演示过了；顺带量到"编译器抓不到这一类"

`tools/mutation_fixture_sweep.py`（可重跑，两面各留档：日志 + 盘上事实）。canary 用手工摆进去的
1 字节 `rst_a (9).bin` / `s2_ok (9).bin`，**不依赖停滞跑偶现的产物** ⇒ 两面都是确定性的。

| 面 | 造法 | 期望 | 读数 |
|---|---|---|---|
| 基线 | 不动源码 | 自检不红 + canary 被删 | `T3 00:11:26→00:12:51`，`165 通过 / 5 失败`，自检红=0，canary 存活=无 |
| MUT-ESC | 把 `sweep_leftovers` 里那行 `snprintf(pat, ..., "%s\%s", ...)` 的反斜杠改回一个（**就是本次事故原样**） | 自检必须红 + canary 必须还在 | `T3 00:13:02→00:14:06`，`166 通过 / 4 失败`，**自检红=1**，canary 存活=`rst_a (9).bin, s2_ok (9).bin` |

⇒ **这条自检从"只有正例"升成"有牙"**。两面的判定都要求"日志读数 + 盘上文件"同时成立，只看一边不算。
`166/4` 与基线 `165/5` 差的那一条不是本判据：那一轮 `[32]` 面 A 的前提抖动没复现（红项构成不同，具名在此说明，免得下一个人以为差异来自变异）。

**顺带量到一条有用的负结果**：变异体的构建输出里**没有**关于那行的任何警告——我另做了一次对照，
在一个临时文件里同时摆 `\c` 与 `\%` 两个坏转义，GCC 只报了 **1 处**（`\c`），**`\%` 零警告**。
⇒ 这一类 bug 编译器不抓，只能靠行为自检 ⇒ 上一节那条"清扫静默空转"不可能靠加编译警告拦住。

**同形具名清单：没拿到，且我知道为什么拿不到**（记下来免得被当成"已扫过，没有"）：
① 我自己的正则扫描器不合格——它把合法的 `"%s\%s"` 也报成坏（`\` 之后没有整体跳过两个字符），
拿已知正例照自己时 4 个控件错了 2 个 ⇒ 结果不可信，作废。
② 改走"让编译器报"这条路，覆盖率也不够：59 个 TU 里用我手搓的 `-fsyntax-only -xc++` + 猜测的 include 串
只有 **6 个**解析成功、53 个报错退出 ⇒ 那个"0 命中"是瞎出来的，不能引用。
⇒ 想要真清单，正确做法是**用项目自己的 flags 全量重建一次并收集 warning**（本笔不跑，边界是"做完就停"）。
已知的两处（`:2391` 的 `"%s\cap%d.bin"` 与 `sweep_leftovers` 的那行）本笔已修；
另修掉一处我自己上一笔埋进源码的**裸 NUL 字节**（`fn_seen[0] = '<NUL>';`，python 把 `\0` 吃成实体），
现在 `engine_selftest.c` 的构建是 **0 warning / 0 error**。

### 2026-10-01（第 2 条）— 裸控制字节门 `tools/lint_control_bytes.py`（两面自检 + 真实文件变异演示）

**先量分母再定规则**：`git ls-files` = 315 个文件，取文本扩展名交集 = **266** 个（**这是登记那一刻的观测值，
不是基准**：分母随"新文件入库"单调增长，2026-10-01 03:0x 实测 271，五个增项已逐文件具名，见本节末「分母增项」）；扫下来
**4 处命中，全部在 `docs/visual-baseline-2026-09-30.md`**（0x01×2、0x02×2），源码侧 0 命中。
成因可复原不是猜：那四处原本是 Windows 路径 `btn_after\01_main_empty_dark.png`，
**`\01` 被当成八进制转义整体吃掉**（反斜杠和两个数字一起没了）。已按字节还原成 `\01`/`\02`，
并且**盘上核对过那些图真的在**：`/tmp/idm-probes/2026-09-30/btn_after/` 与 `planB/` 下都有
`01_main_empty_dark.png`、`02_main_tasks_dark.png` ⇒ 文档写的是可指认的路径，不是记忆。

门本身：允许 `09/0A/0D`，其余 C0 与 0x7F 一律红；每轮印**分母**（登记那一轮打印过 `扫了 266 个跟踪文本文件`）与自身
**成本**（约 1.0–1.3s）。两面自检 `--selftest`：干净副本⇒绿、种了 `0x01` 的副本⇒红、外加一条
"**真实分母非空**"（否则门可以靠"没东西可扫"永久绿）。真实仓现况 **0 处，EXIT=0**。

**分母增项（266→271，逐文件具名，不是"上一轮忘了记"）**：用门自己的 `TEXT_EXT` 与它同一条
`os.path.splitext(...).lower()` 判据，对 `git ls-tree -r --name-only <sha>` 逐笔回算；并在 HEAD 上核对
"`isfile` 那一项是惰性的"（`byext=271` == `byext_and_isfile=271` ⇒ 我用 ls-tree 复算与门用 ls-files 实扫两个口径在本仓不矛盾）：

| 提交 | Δ | 带进来的文件 |
|---|---|---|
| `6d7bf42` | +1 | `tools/lint_control_bytes.py`（门自己入库） |
| `9dc837f` | +2 | `tools/warning_inventory.py`、`docs/warnings-2026-10-01.txt` |
| `ea4e7c7` | +1 | `docs/p0-red-baseline-2026-10-01.txt` |
| `89b832b` | +1 | `docs/p0-red-baseline-recheck-2026-10-01.txt` |

⇒ **扫描范围没变，是被扫集合随入库增长**；五个全是本会话我自己新建的跟踪文件。
⇒ 规矩：**规则数可以写死，实测数不许**——分母的正确写法是"当前跟踪文本文件数（随入库增长）"，
具体数字只印在结果行；台账里那两处 266/315 已就地标成"登记那一刻的观测值"。

**真实文件上的变异演示**（不是临时副本）：往 `tools/engine_selftest.c` 种 1 个裸 NUL ⇒
门报 `off=130357 行=2464 字节=0x00 NUL` 带上下文、`EXIT=1`；随后按字节剔除并核对
`git diff --numstat` 为空 ⇒ 逐字还原；再编一次让 exe 对应源码。

**顺带纠正我自己上一条过头的危害表述**（写进门头了）：我原说"让 grep 静默失明"，实测不准确 ——
`grep -c` 照样数到 3。可演示的危害是**含该字节的读数会被工具层当成二进制整段丢掉**
（本轮就地撞到两次：一次 grep 输出变成 `[Binary output: 188 bytes]`，一次是门自己的读数显示为空）。

**写这条门的过程中我又犯了同一族错，两次，都记下来**：① 我往 `ci.yml` 的注释里写了 `\01`，
python 当场把它吃成一个 0x01 字节 ⇒ `yaml.safe_load` 直接拒绝解析该文件（"我自己就是那个 bug"）；
② 我在门脚本的字符串里写 `chr(0)+chr(92)+...` 拼接时又引入一次同类歧义，把脚本改出语法错。
⇒ 结论：**YAML/Python 里凡出现"反斜杠+数字/百分号"的示例文字，一律用 `chr(92)` 拼或干脆不写**，
注释里的示例不值得冒这个险（CI 那条注释最终改成不含任何反斜杠的措辞）。
CI 接线：`Control-byte gate (no bare control bytes in tracked text files)`，先 `--selftest` 再正式扫。
**入口已接，未执行过**（见「CI 执行状态」一节）。

### 2026-10-01（第 3 条）— 全量 warning 清单入仓：`tools/warning_inventory.py` + `docs/warnings-2026-10-01.txt`

**做法（被前两版否掉之后才定的）**：不手搓 flags、不自己写正则扫源码 ——
命令由构建系统自己交出来：`ninja -C build -t commands <target>` 给出每个 TU 的**真实编译命令**，
我只做两件事：去掉 `-o/-MD/-MT/-MF`（不然第一条就死在 `opening dependency file ...`），
加 `-fsyntax-only`（不写产物）。目标 = `idm-next`、`idm-next-host`、`ui_snapshot`、`engine_selftest`。

**清单（可复算，产物已入仓）**：`扫描 TU 数=64 ｜干净通过=64 ｜源码里报 error=0 ｜未覆盖=0 ｜warning 行数=0`，
成本约 **129–131s**，`--verify` 连跑两次逐字一致 ⇒ 现况是**全仓在自己的 flags 下 0 warning**。
复算：`python tools/warning_inventory.py --verify docs/warnings-2026-10-01.txt`。

**扫描器自己被抓到两次缺陷，都是"覆盖率计数"发现的，不是我发现的**（这条比清单本身重要）：
1. 第一版全部 64 条命令都 `fatal error: opening dependency file ...` ⇒ 打印出来是
   **`干净通过=0`** —— 那个 0 立刻暴露"命令没跑成"，而不是"没 warning"。
2. 第二版 `src/main.cpp` 报 6 条 `<command-line>: error: stray '\'`，被我归进"未覆盖"并**具名印出文件**；
   追下去是我的分词器把 `-DIDM_NEXT_VERSION=\"0.1.0\"` 里的**转义引号当引号开关丢掉**，
   定义值变成 `\0.1.0\` ⇒ 那 6 条错是我造的，不是源码的。修完 64/64 覆盖。
   ⇒ 通用形状：**凡"扫出来是 0"，必须有一分母 + 一个未覆盖桶 + 一次正例对照**，三样缺一就不能引用那个 0。

**正例对照（证明扫描器不瞎）**：往 `tools/engine_selftest.c` 种一个
`snprintf(b, sizeof b, "%s\cCTRL-MARK")` ⇒ 清单如实报出
`tools/engine_selftest.c:503:91: warning: unknown escape sequence: '\c'`，类别直方图 `1 unknown escape sequence: '\c'`、
`warning 行数=1`；随后 `git checkout -- tools/engine_selftest.c` 拔除，`grep -c CTRL-MARK` = 0、
`git diff --numstat` 为空 ⇒ 仓回到 HEAD 内容（二进制也仍对应这份内容，本轮没有未编译漂移）。

**与前两条门的接缝**：这条清单也回答了上一轮"同形具名清单"的欠账 —— 编译器路径下
`unknown escape sequence` 在 64 个 TU 里 **0 处**（我先前那两处已修），而 GCC 对 `\%` 依旧零警告
（见 `mutation_fixture_sweep.py` 那节）⇒ 所以那类 bug 仍只能靠 `lint_control_bytes` 的行为自检拦。
边界：只编现有目标、不新增依赖、不出包、不 push；产物入仓可复算。

### 2026-10-01（第 4 条）— 给"0 warning"标射程：项目**一条 -W 都没开**，且 `-fsyntax-only` 无 codegen

主控追问两句，第一句当场量出结果，而且比措辞更要紧：**四个目标的真命令里抽 `-W*`，只有链接器用的 `-Wl,`，
`CMakeLists.txt` 里也没有 `add_compile_options`** ⇒ 上一节那条"全仓 0 warning"的准确读法是
**「在只要求 GCC 默认诊断的前提下 0」，不是"代码干净"**。同一条清单也不是死的：零 `-W` 条件下它照样抓到
我种进去的未知转义（那属默认诊断）⇒ 上限=默认开的那部分，写清楚了才不会被下一个人当"全量已清"。

第二句是射程：`-fsyntax-only` 不走 codegen ⇒ 只在生成阶段报的那类（典型 `-Wmaybe-uninitialized`）**不在内**。

两条落法（都不重跑那 130s）：① 头部固定印 `warning 开关=…` 与 `适用域=…` 两行；② 抬上限不需要动构建——
`--extra-w "-Wall -Wextra"` 只加在**本工具自己拼的命令**上，`build/` 与 `CMakeLists.txt` 一字不改；
**默认不开**（不开才与历史清单可比），也**本笔没跑**（那是"要不要引入 -Wall 语境"的决定，属主控/用户，
而且第一次开必然冒出一批旧 warning，属"当场变红的门交回他定"）。
`--verify` 的归一化里把这两行排除在外：它们是静态射程说明、不来自测量，改它们不该逼一次全量重跑；
真正参与比对的仍是计数行、类别直方图与逐条清单。本轮**没有**重跑 `--verify`（成本 130s，主控明令不为此重跑）。

### 2026-10-01（第 5 条）— Task #17 用 100ms 重采完：**进度字段对一次已完整落盘的下载恒为 0**

**读数（r5、r6 两轮逐字相同，T3 01:35–01:37 与 01:37–01:38，`结果：165 通过，5 失败`）**：
`采样=200 次(每100ms)`｜`从未创建=197`｜`tmp最大=12,582,912`｜`成品路径最大=12,582,912`｜
`GET首次非零=i=5`(≈0.5s)｜`tmp首次>0=i=1`(≈0.1s)｜**`计数器首次>0=i=-1`（20s 里一次都没非零）**｜
`GET最大=12`｜`status区间=[0,1]`｜`deferred最大=0`｜`conns最大=84(探针自占约 80)`。

⇒ 三个候选里剩下的是**"根本没上报"**，不是"起得晚"也不是"排队不推"：字节从 0.1 秒就开始落、0.5 秒服务端已计到 GET、
tmp 与成品都到达完整 12,582,912，而 `TaskInfo.downloaded` 与 `chunks[].downloaded` 在这 20 秒里恒为 0。
另一面同一字段：这一轮的 save 把"已完整落盘"写成 `"downloaded":0, "done":0` ⇒ 下次启动会**整份重下**；
而 `done=1` 那个方向（[32]/[33]）是**跳过重下**——同一个字段的两处失真，一个造成浪费、一个造成坏文件。

**边界与自纠**：① 中间那轮 `r3` 的读数（`GET首次=12/计数器首次=40`）**作废** —— 它来自我写歪的 `printf`
（格式与参数顺序错位 ⇒ `成品路径最大=4294967295`，且该轮日志只有 13KB、没有结果行 = 中途就死了）。
② 那轮死掉没喊 `/_shutdown` ⇒ 服务留在 18080 上，下一轮的 `preflight_listening` 当场把它挡住（护栏这次起作用了）；
按老规矩先读 `CommandLine`（`…\python.exe tools/engine_selftest_server.py 18080 …`）确认是我自己的，才 `taskkill`。
③ `conns` 这一列**不能当产品信号**：40 次网络采样 × 2 个数 ≈ 80 条连接是探针自己开的，已在读数里并列印出。
⇒ 判据地位不变：**只进报告行，不进退码**（`[分桶]`/`[前提读数]` 都不参与退出码）。
`src/` 仍零改动；本轮只动 `tools/engine_selftest.c`。

### 2026-10-01（第 6 条）— MC-3 第 1 步：P0「修前红」逐字节基线入档（产品代码未动一行）

制品 `docs/p0-red-baseline-2026-10-01.txt`。T3 `01:41:11→01:42:37`，`exe mtime 01:35:02 / sha256[:12]=a7da6c1973a4`，
`preflight_listening=0`、`after_start_listening=1`，`SELFTEST_RC=5`，`结果：165 通过，5 失败`，
**其中 P0 三条**（逐字入档）：`[32]`B 截短、`[32]`C 删除、`[33]`B 跨进程。修完要求"恰好这三条转绿"，
可拿制品里那三行 `[FAIL]` 原文逐字比。

**这一步里我自己犯了一次、并被自己的对账抓住**：第一版制品把两个算术等式的**区间写死了上一轮的数**
（`[4096,9437184)`），而本轮面 B 的暂停时机不同 ⇒ 实测错位 8,351,108 ≠ 我硬编码的 9,395,506。
⇒ 改成**从本轮读数解析区间**，三面各自 差=0：面B `8,384,512 − 33,404 = 8,351,108`、面C `9,437,184 − 37,599 = 9,399,585`。
第三行（跨进程）的区间不是独立量到的（子进程读数里没有"最大写偏移"字段），我在制品里如实标了 ※。
教训同形：**带来源名的数字必须真指到那个来源**；把上一轮的数抄进这一轮的"对账"里，比没有对账更坏。

### 2026-10-01（第 7 条）— MC-3 第 2 步：两候选的量化对比与选定（仍未动产品代码）

代价的分母**取自本轮归档读数**（`docs/p0-red-baseline-2026-10-01.txt`）：面 B 记录的最大写偏移 8,388,608、
盘上被截到 4,096 ⇒ 缺口 **8,384,512**；面 C 记录 9,437,184、盘上为 0 ⇒ 缺口 **9,437,184**
（两者都等于实测错位数 + 该区里零填恰好命中 `i%251==0` 的位置数）。分片：`DEFAULT_CHUNK_SIZE` 切 3 片，本片 4,194,304 字节。

| 候选 | 做什么 | 补拉字节（本轮两面） | 代价/边界 |
|---|---|---|---|
| **①-clamp（选它）** | load 时对每片算 `avail = 盘上实际长 − 该片 start`，把 `downloaded` **夹到 `min(claimed, max(0,avail))`**，被夹掉的位一律重新拉 | 面B **8,384,512**；面C **9,437,184**（另有 3,145,728 未声称的尾段本来就要下） | 补拉量 = 缺口本身，不多拉一个字节；`avail > claimed` 时**保持 claimed** ⇒ 天生满足"盘比 state 多不是数据丢失" |
| ①-chunk-granular | 该片有任何一位不被盘支持就**整片重来** | 面B ≤ 8,384,512 + 4,194,304 = 12,578,816（≈整份）；面C 同 9,437,184 | 比 clamp 多拉到接近一倍，唯一好处是实现少两行 |
| ② detect+报错不自愈 | 发现不符就把该片降级为未完成并**停下、给用户可见错误**，等他手动重来 | 0（自动不拉） | 坏文件确实不会再被交付，但"能自己修好"没做到；且要把新文案接进 gui 层（L2→L4 那一类张力），面更广 |

**⚠ 表里那两个「补拉字节」是快照，不是常数**（2026-10-01 前置① 复跑实测）：面 B 的缺口由**暂停落点**决定，
制品那轮盘上截到 4,096、最大写偏移 8,388,608 ⇒ 8,384,512；复跑那轮暂停落得晚，最大写偏移 9,437,184 ⇒ **9,433,088**
（两个观测，各自 差=0，等式没坏，坏的是把某一轮的数当常量）。⇒ 引用这一行时必须读成"该轮观测值"，
要当预算用就重跑 `docs/p0-red-baseline-recheck-2026-10-01.txt`「复算命令」那一节的代码块（不写行号——我刚写过的"第 5 行"就已经是错的，命令在第 10–13 行）、现量现用。
面 C 不受影响（它由"盘上为 0"定义，缺口恒等于最大写偏移）。候选之间的**排序**与快照无关：
clamp 与 chunk-granular 都随缺口线性放大。

**选 ①-clamp 而不选另两个的理由**：① 它是三者里唯一"补拉量恰好等于缺口"的；② 它对主控点名的衍生风险是**结构性免疫**而不是靠额外分支——`avail > claimed` 走"保持 claimed"这一支，不会把完整文件重拉一遍；③ 它不需要新增用户可见文案就能满足"不许静默"：把"发现 N 片进度被盘否证"写进 `TaskInfo.error_msg`（状态仍是进行中，因为它能自愈），这正是第 3 步要加的那条断言的落点。

**这个候选的已知残余（明写，不当已覆盖）**：尺寸对、内容错（同一长度里的稀疏洞）**size 判据看不见** —— 那要把每片校验和写进状态文件（`idmFormat` 2→3 的格式变更）才能封掉，不在这次修法范围内。

**读代码读出来的一个新风险，必须先解决再动手**：现在 load 只把 `tmp_path`（`dir\filename.idmtmp`）当唯一对象（`:2042`）。
存在这样一支真实场景——**成品已经 rename 到位（`filename` 存在且长度 == `file_size`）、`.idmtmp` 已消失、状态没落**——
那时 `avail = 0` ⇒ ①-clamp 会把整份重拉一遍，把"盘比 state 多"这一支误伤成"数据丢失"。
⇒ 第 3 步的实现必须**同时看两条路径**（`.idmtmp` 与成品 `filename`），并在两面各自验一遍：
`tmp 缺失 + 成品完整` ⇒ 判定已完成、补拉 0 字节；`tmp 缺失 + 成品也不在` ⇒ 才允许全量重拉。

### 未开工，入参写定于 2026-10-01 —— MC-3 第 3 步（唯一改 `src/core/download_core.c` 的那一步）

1. ~~先对账红基线~~ **已完成（2026-10-01 第 8 条）：三条 P0 红逐字对上 3/3**，制品
   `docs/p0-red-baseline-recheck-2026-10-01.txt`。⚠ 那条命令**会把上一轮的日志原地覆盖**
   （`s0_run.sh` 写死 `s0b-selftest.log`）——重跑前先 `cp` 走，或改带时间戳的输出名。
2. 落点：`dlmgr_load_state()`（`:1921..2079`，在 `:2042` 拼完 `t->tmp_path` 之后）。对**两条路径**取尺寸：`.idmtmp` 与成品 `filename`；按片 `avail = 实际长 − start`，`downloaded = min(claimed, max(0, avail))`，`avail > claimed` ⇒ **保持 claimed**（这一支就是"盘比 state 多不是数据丢失"）。
3. 新断言：被夹掉时把"发现 N 片进度被盘否证"写进 `TaskInfo.error_msg`（状态仍进行中）——**必须引擎自己报**，测具的内容比对不算。
4. 两桶各演一次会红：A `tmp 缺失 + 成品完整` ⇒ 判已完成、补拉 0 字节；B `tmp 缺失 + 成品也不在` ⇒ 才全量重拉（面 C 现成）。A 桶需新造夹具。
5. 收尾：三条红转绿 + 两面输出入档；明写残余（尺寸对、内容错的稀疏洞 size 判据看不见，封它需 `idmFormat` 2→3 的每片校验和）。
边界：不 push、不 tag、不改版本号、不出包、不碰 `N≥100 且 M=0`；`src/` 除本步外不动。

**⚠ 2026-10-01 03:52 实测推翻了这个落点的一半**（改过→跑→回退，全过程产物在 `docs/p0-mc3-step3-red-2026-10-01.txt`）：
按上面第 2 行在 `dlmgr_load_state()` 里夹之后，**三条红只关掉了跨进程那一条**（`166 通过，4 失败`）。
剩下 `[32]`B 截短、`[32]`C 删除**照红**，成因不是夹得不对，是**这两条根本不经过 load_state**：
它们是同进程内暂停→续传，被信的 `done=1` 活在**内存里的 task 结构**上，而盘上那份 `tasks.json` 里
`chunks_done=0`、`downloaded=0`（就是 #17 那条恒为 0）。⇒ 在 load 边界夹，夹到的是"什么都没声称"的那份。
三条更正，下一轮开工直接照这三条做，不必重新发现：
1. **判据必须挂在 `c->done` 上，不是 `c->downloaded` 上**——`asserted = done ? 整片长 : 0`，再与 `downloaded` 取大值；
   只夹 `downloaded` 等于没夹（我第一版就是这么写的，`STATE-CLAMP` 一次都没触发过）。
2. **同一判据要有第二个调用点**：续传入口（`dlmgr_resume` / `start_task_threads` 之前）对**内存里的 chunk**
   复验一次 backing；load 那段抽成共享 helper，两处调用，别写两份。
   `avail >= asserted ⇒ 保持不动` 那一支在两处都保持不动（"盘比 state 多不是数据丢失"由它承担）。
3. **基线的承重集是"三条 P0 红"，不是总数 5**：那两条前提读数会闪——同一份 HEAD 源码，02:26 那轮红、
   03:50 这轮绿（`167 通过，3 失败`，红正是逐字的三条）。所以别拿"N 通过 M 失败"的 N/M 当身份，
   身份只认那三行 `[FAIL]` 原文（`docs/p0-red-baseline-2026-10-01.txt:10-12`）。
   `前置①` 当时记的 3/3 仍然成立；**这条只是把"5"这个数降级成非承重读数**。

另：`STATE_CLAMP_rows=0` **不能**读成"夹没触发"——测具只在失败行印 `error_msg`，通过行不印 ⇒ 这是测具盲区，
也正是前置② 那条断言必须存在、且要在**绿的那一侧也印一次读数**的理由。
本轮留下的实物：`docs/p0-mc3-step3-red-2026-10-01.txt`（失败原文）、`tools/p0_rerun.sh`（§8.1 形状落地：
输出名带时刻戳、写前断言目标不存在、起服务前后各断言监听数 0/1、退码取引擎的）。`src/` 已 `git checkout` 回 HEAD 并**重编一次**，
使 build/ 里的二进制与 HEAD 源码一致（否则下一个会话会把我改过那版的行为当现行）。

### 2026-10-01（第 8 条）— MC-3 第 3 步·前置①：红基线逐字对账 = 3/3（`src/` 一字未动）

制品 `docs/p0-red-baseline-recheck-2026-10-01.txt`。T3 `02:26:48→02:28:14`（86s），
`preflight_listening=0 / after_start_listening=1 / windows_pid=9808 / SELFTEST_RC=5 /
结果：165 通过，5 失败`。三条 P0 红与 `docs/p0-red-baseline-2026-10-01.txt:10-12` **逐字相等 3/3**
（口径：两侧都只剥行尾 `\r\n`、保留行首两空格、整行相等）。面 A 仍绿（`内容=1 错位字节=0`）
⇒ 这三条红只由"盘被动过"触发。

- **二进制身份**：02:25 实测 `exe sha12=a7da6c1973a4` 与制品逐字相同；随后 `ninja` 因
  `build log version is too old` 重编 4 个 obj ⇒ 新 `sha12=f7ac4e1568f6`。`git status --porcelain`
  只有 `?? idm-next-glass-ui.html` ⇒ **源码未变，哈希变的是链接时间戳**。所以严格说法是
  "同一份源码重编后的新二进制复现了同样三条红"——这比"同一个二进制再跑一遍"多排除了一种可能
  （红依赖旧 obj）。
- **面 B 的算术行没有复现，而且本来就不该期待它复现**：制品那轮 `[4096,8388608)→8,351,108`，
  本轮暂停落得晚 ⇒ `[4096,9437184)→9,395,506`，两者各自 **差=0**。⇒ 第 2 步代价表里"面 B 补拉量"
  是随暂停时机变的量（现有两个观测 8,384,512 / 9,433,088），候选之间的排序不受影响。
- **本轮查出的三条制品缺陷（都记在我名下）**：① 原制品第 2 行的复算命令反斜杠被写入它的 shell 吃掉，
  照抄不可跑；② 那三行算术**没有随仓的生成器**（`tools/` 里无任何解析器，当时是 Bash 内联 python），
  **本轮（02:40）已按他给的第二个选项在两份制品旁如实标注"无生成器、不可复算"**（公式逐字写进复算制品第 2 节），
  落成 `tools/` 脚本这件事仍开着，没人替它占功；
  ③ `s0_run.sh` 日志名写死 `s0b-selftest.log`，**本轮重跑把 01:41 那轮的源日志覆盖了**，制品第 7 行引用的
  `ce59a2f0362b` 已不在盘上（三条红与三面读数因已进 git 仍可引）。复算脚本落仓时必须改名带时间戳。
- **我自己这一轮的两个量具 bug，都在发出前被抓住**：① 严格口径第一次报 **0/3**，是我在制品侧用
  `splitlines()`、在日志侧直接迭代文件对象（留 `\n`）造成的单边剥行；② 解析代码块用
  `l.strip()=="```"` 判围栏，带语言标记的 ` ```bash ` 不匹配 ⇒ 奇偶错位，报"三行都不在文件里"。
  两次都是"对账指着一条不存在的缺失喊红"的同形错误 ⇒ 记进本条，不靠"下次注意"。
- **前置② 没做，也不是忘了**：给"引擎自己发现 state 与盘不符"装新断言，本轮只能演示红的一侧，
  绿的一侧要改 `download_core.c` 才能演——只验一侧的门按既有口径就是不占功的半坏门。停在这里。

### 2026-10-01（第 8.1 条）— 主控拔的规矩：被引用过的读数文件**不得用固定名重写**

规矩原文（他入档、会抄给全舰队）：**任何被制品/台账引用过的读数文件，不得用固定名重写**——
名字里带轮次或时刻戳，或在写前断言目标不存在。

这次违它的实物就是我自己的 `s0_run.sh`：`> "$D/s0b-selftest.log"` 一个固定名，把 01:41 那轮
（制品第 7 行引用 `sha12=ce59a2f0362b`）原地覆盖了。同族的还有 `%TEMP%\...\bkt*.sh` 那一串——
它们各自写死自己的名字，只是还没被制品引用过，所以暂时没造成毁档。

**为什么本轮不加那条会红的自检（不是"下次注意"）**：自检的形态要求它两侧都可证——
"同名二次写入 ⇒ 红" 与 "换名写入 ⇒ 绿"。本轮只造得出红的一侧（绿的一侧要靠先把 runner 改成带戳命名，
而 runner 现在整个人在 `%TEMP%` 里，不在仓内 ⇒ 一条依赖仓外文件的门本身就违反"一道门不得依赖别人的文件"）。
⇒ 装它=半坏的门，同前置② 那一次同形。

**下一轮一次能装完的形状，写在这里免得重新设计**：
1. 先把 runner 落仓 `tools/p0_rerun.sh`，输出名带戳：`"$D/selftest-$(date +%m%d-%H%M%S).log"`，
   并保留一条 `latest` 只作**指针**（`cp` 而非重定向），制品引用戳名不引用 latest。
2. 门放在 `tools/lint_control_bytes.py` 旁边新起一支，或并进入仓 runner 的开头：**写前断言目标不存在**，
   存在则退 3 并把已存在文件的 sha12 打出来。
3. 两面夹具：A 面——同名连写两次 ⇒ 必须退非 0 且第二次的字节没盖掉第一次；B 面——不同名 ⇒ 必须退 0。
   A 面的红必须由**门**给，不能由 shell 的覆盖行为给（否则红的是文件系统，不是断言）。

**同一条规矩差点被我自己第三次违（这次毁的是文档）**：装 8.1 这一节时我把 `old_string` 填成了
「### …（第 9 条）— …」整行标题，`new_string` 里没带上它 ⇒ **第 9 条标题被删**，正文挂到了 8.1 底下。
发出前用 `git diff | grep '^-'` 查出来（那次显示"零删除行"才是修好的证据，而不是"看着还在"）。
⇒ 自取的教训与上面同源、形状不同：**插入型编辑只要把锚点行写进 `old_string` 却没写进 `new_string`，
删的就是别人的内容而不是我的草稿**；写完必须看删除行计数，`0 deletions` 才算"纯插入"。



### 2026-10-01（第 9 条）— #17 定性升为独立一条 **P2**（与 P0 并列，不混写）

主控原话采纳的表述：`TaskInfo.downloaded` 与 `chunks[].downloaded` 对**一次已完整落盘的下载**恒为 0，
而字节确实到位（`.idmtmp` 首采即非零、最终 12,582,912；r5/r6 逐字相同）。
定性从"显示问题"改重：用户看到 0 B、下次启动把 12,582,912 字节整份重下 ⇒ **带宽与时间的真代价**。
与 P0 的关系用我那句原话引用：**"两者共用同一族『自我声明、无人核盘』的字段，但没有实测证据指向同一根"**
——不自创"同一个根"。报给用户时与 P0 并列成两条，不合并。

### 2026-10-01 02:58（接手快照）— 上下文将尽：欠账 / 别碰 / 已撤回（下一轮先读这一节）

**欠账，按顺序**
1. MC-3 前置②：引擎侧新断言（`TaskInfo.error_msg` 里"发现 N 片进度被盘否证"）——形状与"为什么不在只验得了红的一侧时装"见第 8.1 条。
2. MC-3 第 3 步本体（唯一改 `src/core/download_core.c` 的那一步）：5 行入参在「未开工…MC-3 第 3 步」那一节；
   动手前先解决"load 只看 `.idmtmp` ⇒ 成品已 rename 会被误判成数据丢失"（两桶各演一次会红）。
3. `tools/` 里补算术生成器；补不上就维持"无生成器、不可复算"的标注，别把它读成已验。
4. §1.2 表最后一列未重写（主控排在最末）。

**别碰**：不 push、不 tag、不改版本号、不出包、不动 CI 判据 `N≥100 且 M=0`；`idm-next-glass-ui.html` 保留、
不入库、不删（等用户拍板）；S3 归用户本人跑；UI 探针会写 `C:\Users\yyyy\idm_shots\`（截图 + `_data\tasks.json`
+ `history.db`），跑完清这三样，**别删 `_data\aria2|ffmpeg|yt-dlp`**（用户自己的工具缓存）。

**读法纪律**：`165/5` 的 5 = **3 条 P0 红 + 2 条前提读数**，别当成 5 个缺陷；三条红的**基线是 git 制品**
（`docs/p0-red-baseline-2026-10-01.txt:10-12`），不是那张被覆盖过的日志；重跑 `s0_run.sh` 前**先 `cp` 走旧日志**
（它写死输出名）；`ninja: no work to do` 不是行为证据，反过来 `build log version is too old` 会整棵重编、
换掉 exe 哈希（源码没变也要报，附 `sha12`+`mtime`）；面 B 的补拉量**是快照不是常数**（两个观测 8,384,512 / 9,433,088）。

**已撤回、别当欠账复活**：① "3/5 间歇性"与"4/4 稳定"（夹具漏清产物、我量的不是引擎那条路径）；
② `7ee54de` 的"幂等命令再问一次 + 窗口 5s→9s"（是掩盖，已由 `369bf21` 撤回）；③ "跨 host 重定向泄漏凭据"
（libcurl 8.22 默认就不带过去，自测 [27] 守着）；④ "磁盘满/写失败链路有缺口"与"`IpcServer` 的
`waitForReadyRead×10` 是可感知卡顿"（复核后都不是缺陷）；⑤ 更早的基线数 166/3、155/2、142/0 均已标时期作废；
⑥ 我把 Task#17 定性成"显示小病"、以及"那 20 秒的 0 由 `:1213` 造成"——**两者都不成立**，现结论见第 9 条。

**开工顺序（2026-10-01 03:0x 主控拍定，写在这里是因为它会跨会话失效）**：收到"开 MC-3"那句时，
它是**本轮第一件事**，不许排在其他读数/文档/普查后面。开工前先回一行"剩余量够不够走完那九环"
（读 `dlmgr_load_state` → 写 clamp → 重编 → 造 A 桶夹具 → 两面各演 → 变异证红 → 全量复跑 → 两面输出归档 → 提交）；
不够就一句话报"开不了"然后**停**，不拿它当本轮产出——**不走完就不 `cp`**，因为 `cp` 属于开工动作。
若**连续两轮**都报"开不了"，这条链交接给一个新的一路会话跑——它需要的只有本节 + 红基线制品，两者都在仓里，不依赖本会话记忆。
四条硬条件到开工那轮仍有效，不重新解释：`avail > claimed` 那支保持不动；断言只长在 `TaskInfo.error_msg`（测具的外部比对不算）；
两桶各演一次会红；每步读数带 T3 与"本轮是否真重编"。

**MUT-A / MUT-B / 自我报告读不到：三条都是结构问题，不是没跑实验（2026-10-01 04:16 补）**

- **MUT-B（摘调用点②）**：`SELFTEST_RC=4`、`FAIL_rows=4`，红的正是**面B截短 + 面C删 .idmtmp**（另两条是会闪的前提读数），
  跨进程那条保持绿 ⇒ ② 有牙。
- **MUT-A（摘调用点①）**：`168 通过，2 失败`，**与不摘时一字不差**（T3 04:14:07 起，exe_mtime 04:13:59 / sha12=3daac313a985，
  本轮真重编 `Building C object`=1）⇒ **① 在当前夹具集合下没有牙**：跨进程那面里子进程也会走 `dlmgr_start`，被②覆盖。
  ①唯一不可替代的场景是"重启后不点继续、只看列表"时 `TaskInfo` 就要说真话，而那面夹具不存在。
  ⇒ 按既有口径把①标为**"未被变异证明的护栏，不占功"**，不许写成"两处都需要"。下一轮**二选一**：补那面夹具，或摘掉①。
- **`STATE_CLAMP_rows=0` 的真正原因**：`download_one_chunk` 每次 attempt 开头 `t->error_msg[0]='\0'`（≈`:1050`，
  注释原话是"清掉上一次失败留下的原因，否则重试时 GUI 还显示旧错误"）。⇒ clamp 在续传入口写的自我报告
  **活不到任务结束时被读的那一行**，所以这条断言在当前结构下不可能被外部读到。
  两条候选（选一条，别堆第三处）：**① 换个不会被清的字段**（`TaskInfo` 加 `clamp_chunks`/`clamp_refetch_bytes`，
  代价=公开结构 +2 字段、要过分层门）；**② 在完成那一刻（`:1211` rename 之前）重写一次结论**
  （代价=两处写同一字段，需断言"完成时读到的仍是那句"）。
  验收钉子不变：**绿侧那面也必须有一行盘上读数含 `[STATE-CLAMP]`**，现在零行。
- **两桶各演一次会红仍未做**：A 桶（`tmp 缺 + 成品完整` ⇒ 补拉 0 字节、且**不出现** clamp 句）需新夹具——
  它恰好就是让①长出牙的那面；B 桶（`tmp 缺 + 成品也不在` ⇒ 全量重拉）现成。
  顺序：**先把自我报告变成可读的（上面二选一）→ 补 A 桶 → 复跑 MUT-A' 证明摘任一调用点都会红。**
- 源码状态：MUT-A 已用 `git checkout -- src/core/download_core.c` 还原（两处调用点逐字回到 2b18cbf 的形状），
  并**重编**一次使 build/ 与 HEAD 一致（exe_sha12=60d59f44dc04）。




**拍定（04:20，技术决定主控此前已全权交出，不再等他选）**：自我报告走候选① —— `TaskInfo` 加两个**不被 attempt 清掉**的
字段（`clamp_chunks`、`clamp_refetch_bytes`），clamp 时写、只有新建任务才归零。候选②（完成那一刻再写一次 `error_msg`）
否掉的理由不是省事：**在一个已经被治好的文件上留一条错误文案，用户读到的是"完成 + 有错"，语义是错的**；
自我报告要表达的是一次**已经发生的自愈**，天然该是计数而不是错误串。代价如实写明：公开结构 +2 字段
（core→gui/cli 的 L0 契约面），`error_msg` 语义不动、IPC 现有字段不动。
判据三条一起钉进 `tools/engine_selftest.c`，**不新增第三个调用点**：面 B/C 结束时 `clamp_refetch_bytes>0` 且内容=1；
面 A（盘没被动过）两字段恒 0（**不误报**）；A 桶（`tmp 缺 + 成品完整`）也恒 0 —— 这面夹具同时就是让调用点①
长牙的那面，一并回答"①留还是摘"。

**主控 04:2x 拍定：选①＋留①＋先做 A 桶。** 附加要求里有一句在实现上取不到值，我按盘上语义改掉、不照抄：
"A 桶：`clamp_chunks>0` 且补拉 0 字节"——A 桶的定义是 `tmp 缺 + 成品完整` ⇒ 盘把 state 的声称**全部支持** ⇒
走 `avail >= asserted` 那一支一个字不动 ⇒ `clamp_chunks` 必为 **0**；而且该计数只在 `refetch>0` 时自增，
所以"`chunks>0` 且补拉 0"这个组合在代码里**不存在**，照抄只会得到一面**永红**的夹具。
改成可满足的三桶（A 桶仍是给调用点① 配牙的那面）：
- **A 桶**（`tmp 缺 + 成品完整`，state 声称 done=1）⇒ 两字段 `==0`、状态仍 COMPLETED。它的证人价值是**不误报**：
  "盘比 state 多/相等"那条衍生风险由这一面钉住（正是主控点名的那个风险）。
- **A' 桶**（`tmp 缺 + 成品只有前半`，state 声称整份 done）⇒ 两字段 `>0` 且等于缺口。**这才是"两字段都非零"的正例**，
  代价是补拉量必然大于 0 —— 语义上就该如此。
- **B 桶**（`tmp 缺 + 成品也不在`）⇒ `clamp_refetch_bytes == file_size`（全量重拉，夹具现成）。
① 在 A'/B 长出来之前仍按**不占功**写，不进"两面都有证人"。下一轮顺序：A 桶 → A' 桶 → 完成路径 → MUT-A' 复跑。


### 2026-10-01 05:0x（第 12 条）— A 桶夹具施工图（只写图，不写代码：本轮剩下的量不够走完那条链）

要补的是 `tools/engine_selftest.c` 里一面新夹具。三件要紧的：

1. **读数时刻**必须在 `dlmgr_load_state()` 之后、任何 `dlmgr_start()` 之前 —— 那是调用点① 唯一的证人时刻。
   之前 `[32]`/`[33]` 三面都调了 start，所以②必然先执行，①因此无牙（MUT-A 与不摘逐字相同）。
2. **两面**：
   - **A 桶（不误报）**：手写 state，3 片全 `"done":1`、`"downloaded"` 写整片长；**成品 `filename` 存在且长度 == `"size"`**，
     `.idmtmp` **不存在** ⇒ 断言 `clamp_chunks==0 && clamp_refetch_bytes==0 && status==COMPLETED`。
     它钉住主控点名的衍生风险："盘比 state 多/相等"绝不能判成数据丢失、更不能整份重下。
   - **A″ 桶（① 的牙）**：同一份 state，成品**截半**（或成品与 tmp 都不在）⇒ 同一时刻必须读到
     `clamp_refetch_bytes>0` 且等于缺口、`status != COMPLETED`。
3. **判别式与出口**：把①摘掉（`if (0)`）⇒ A″ 必须红、而 `[32]`/`[33]` 照旧全绿（②兜住了它们）。
   若 A″ 摘① 仍不红 ⇒ **摘掉①**，不留第二处无牙的护栏；不许为了让它存在而补判据。

键名与签名都是这轮从盘上读的，不是记忆：状态文件吃的键是 `"id":`/`"size":`/`"downloaded":`/`"status":`/`"chunks":[`
（`download_core.c:2049/2063/2067/2071/2080` 处的 `strstr(p, "\"key\":")`），片内是 `"start"/"end"/"downloaded"/"done"`；
测具用 `static void check(const char *what, int ok, const char *detail)`（`engine_selftest.c:35`）与既有的
`outdir/path/tmpp/d` 缓冲。夹具**必须自己删干净产物**（`clean_outputs()` 的名单要加新文件名，这一条我已经踩过一次：
名单漏了新文件 ⇒ 采到的 `.idmtmp` 不是引擎那条路径 ⇒ 整轮结论作废）。

### 2026-10-01 05:3x（第 13 条）— P2（进度恒 0）开工人包：一条已定位的线索与它要我先证的事

主控 05:3x 停止追问、要求直接推进，于是把 P2 的起点固定在这里。本轮**没有改 `src/`**——
不是来问，是这一步做完需要一条完整的链（读 `calc_chunks`/暂停路径/测具前提三面 → 改 → 重编 → 跑 86 秒 → 归档 → 提交），
我剩下的量不足以走完，半途留在产品代码里比不留更糟。

**线索（本轮实测读到的，不是猜）**：进度只有两处写手——`download_core.c:951-952`
（`c->downloaded += result.bytes_written; t->downloaded += ...`，每块边界都更新）。
而 `calc_chunks()`（`:801`）**无条件**把 `t->chunks[0].downloaded = 0`（`:807`）与每片
`t->chunks[i].downloaded = 0`（`:827`）清零，它在监督线程里被调用（`:1194`）。
⇒ 若 `:1194` 在**续传**路径上也会跑到，每次续传都会把已攒的分片进度抹成 0，而盘上的字节还在——
这与两条会闪的前提读数（`downloaded=0 chunk_count=3 chunks_done=0`、`状态文件里的最大写偏移=8388608`）
形状一致：字节到位、记账归零。

**开工第一步必须是证伪这条线索，而不是照它改**：`:1194` 到底在哪些路径上执行（新任务/续传/重启恢复），
`calc_chunks` 里那句清零有没有"已有进度就保留"的前置；测具那条 `静默=1` 走的是哪条暂停路径。
这三点没读清之前，"P2 是 calc_chunks 抹的"只是一个像样但未证的假设——按既有口径：**未证的根因不配当修改理由**。

判据形状（修完必须成立的）：面 A/B/C 的暂停点读数 `downloaded > 0 且 < file_size`、
状态文件里 `chunks` 带进度（这两条现在是红的，且**不承重**——它们是证人不是 P0），
#17 那条"进度恒 0 ⇒ 用户看到 0 B、下次启动整份重下"随之消失。不许为了让它们绿而只改测具的读取时刻。


**第 13 条的线索已被我自己证伪（06:0x，同一轮内完成）**：`calc_chunks(t)`(`:1194`) 在
`if (need_probe) {`(`:1163`) 的**块内**，而 `:1160` 写死 `int need_probe = (t->chunk_count == 0) ? 1 : 0;`。
⇒ 只要任务带着非空 `chunks[]`（失败读数里正是 `chunk_count=3`），探测整段被跳过，`calc_chunks` 根本不执行，
它那些 `downloaded = 0`（`:807`/`:827`）与 `else` 分支的 `chunk_count=1`（`:1196-1198`）都碰不到这次续传。
**所以"P2 是 calc_chunks 抹的"不成立，这条从待修清单里划掉，别当结论传下去。**

新的起点（尚未证，别再写成根因）：唯一写手 `:951-952` 在块边界累加，理论上必然非零；而读数同时带着
`静默=1`、`chunks_done=0`、`downloaded=0`。三个方向按便宜程度排：
① 读数的**来源对象**对不对——我在本轮已经踩过一次"解析器读到状态文件里别的任务的 chunks"，
   先确认测具读的是它自己那个 id；② `静默=1` 是哪条暂停路径（`dlmgr_pause` 走 `detach_slot`/提前收工时，
   会不会连记账一起没落盘）；③ `dlmgr_save_state` 写出的分片进度与内存值是否同源。
①最便宜且历史上有前科，先做①。


**P2 第①步的结论：测具那条 `状态文件里的最大写偏移` 是误导性读数（`:2596`）**
它写的是 `if (s + dd > maxoff) maxoff = s + dd;`。当所有分片的 `downloaded` 都是 0 时，这个式子退化成
`maxoff = max(start)` —— 12MiB 切 3 片就是 `8,388,608`，**恰好等于"第三片的起点"**，读起来却像
"盘上已经写到 8 MiB"。于是那条前提红行的正文同时印着 `downloaded=0 chunks 带进度=0` 和
`最大写偏移=8388608`，**后者会把前者的严重度洗白**（"看着像有进度，只是没落盘"）。
⇒ 这条必须先修再谈 P2：判据要分开两个量——`写入偏移证据 = max(start + downloaded)` **仅在有 `downloaded>0` 时才成立**，
否则应印 `maxoff=无(dd 全 0，此值来自片起点)`。形状同 [[官网那轮]] 的"断言'数据已到达'必须带 `rows=`"：
**没有条件支撑的派生量，印出来就是假证人。**
同一步里另一件已确认的好消息：状态文件解析**确实**先按 `"file":"<本任务名>"` 定位到本任务对象
（`:2583-2586`，注释就是我上次踩坑后写的），所以"读到别的任务"这一条**排除**，不必再查。

**P2 剩下的唯一候选**：内存侧 `t->downloaded` 的累加点（`:951-952`，在 g_lock 内、每块边界）是存在的，
所以怀疑对象收窄到 `result.bytes_written` 本身——它在分块 Range 路径上是否真的被填。
下一轮从 `NetDownloadResult.bytes_written` 的**写入方**读起（`network.h` / curl 写回调），
而不是再猜累加逻辑。


**P2 第③步：候选从"计数没累加"改写成"累加写到了别的槽位"——这条若成立就不是外观缺陷**
盘上两处证据（本轮读到，不是记忆）：
- `:599` 的注释自己写着「任务数组始终按 task_id 升序排列（add 追加递增 id；**remove 用 memmove 保序**）」，
  `:700` 的 `detach_slot()` 确实对 `g_tasks` 做 memmove 压缩。
- 工作线程拿到的是**指针**：`pool_worker(LPVOID arg)` 收 `(DownloadTask*)arg`，
  进度累加走 `t->downloaded += result.bytes_written`（`:951-952`，`g_lock` 内，每块边界都执行）。

⇒ 假设：**一旦有任何一次 `dlmgr_remove`/`detach_slot` 把前头的槽位挤掉，仍持有旧指针的在跑线程就会把
字节数、状态、完成回调写进"移动后落在另一个任务身上"的那一格**。`dlmgr_get_task_info()` 是按 id 找格子的，
于是它读到的那个任务恒 `downloaded=0`——与 Task#17 现场一致（20 秒里字节完整落盘、`计数器首次>0=i=-1`、
`status区间=[0,1]`）。
**这也顺带解释了为什么它是恒 0 而不是偶尔 0**：`[32]` 之前的用例反复建/删任务，槽位早就移位过了。

**必须先证伪再动手**，三个便宜判据（按顺序做，第一个不成立就换方向）：
① 在 `:951` 处临时同印 `t->task_id` 与 `cid`——若印出来的 id 不是发起下载的那个任务，指针错位直接坐实；
② `dlmgr_add` 是否可能返回**复用槽位**且 worker 在复用之后才写；
③ 有没有任何路径在任务 RUNNING 期间调用 `detach_slot`（`:700` 的注释说调用者须已持锁——
   那说明它**确实**可能在别的任务运行时被调到）。
若坐实：这是 **P0 级**（记账/状态/完成回调可能落到别人的任务上），优先级排在 P2 之前，且修法要改成
**按 id 取槽位**（每次更新时重新查表）或**去掉数组压缩**（槽位空洞 + 按 id 寻址），二选一都要交代价对比。
本轮 `src/` 未动；纯追加（numstat 见下）；`lint_control_bytes cb=0`。


**P2 假设 ①「累加写到移位后的槽位」被代码否证了一半，剩下的那一半更精确（`:662-700` 实读）**
`dlmgr_remove()` 里有专门一段防的正是这件事——注释原话：「此刻 memmove 把整个结构体挪一格，线程手里的指针
就会指向**另一个任务**：它会把自已的进度、分片数据、乃至『下载完成』状态写到那个无辜任务身上…**跨任务污染，
且无法事后察觉**」。做法是：`if (t->worker_alive > 0)` ⇒ 不摘槽，只登记 `t->removed=1` + `TASK_CANCELLED`，
等最后一个线程退出时由它就地摘除。
⇒ 所以"删除**正在跑的那个**任务"这条路已经被堵住，**我上一轮那条措辞（任何 remove 都会让在跑线程指错）过头了**，
按口径先撤回一半。

**没被堵住的那一半（新的、也是最后一个候选）**：守卫查的是**被删任务自己**的 `worker_alive`；
而 memmove 移的是**整张数组**。⇒ 删掉一个**空闲**任务 Y（`worker_alive==0` ⇒ 走 `else` 分支真摘槽），
会把排在 Y 之后的**正在跑的**任务 X 的格子整体前移一格，而 X 的线程仍持着 X 旧槽位的指针。
判据（可直接证伪，不需要下载）：
① 建 X、起 X（让它真的在下），再建一个**已完成/空闲**的 Y 排在 X 前面；
② `dlmgr_remove(Y)`；③ 轮询 `dlmgr_get_task_info(X)`——若 X 的 `downloaded` 从此刻起恒 0 而 X 的文件还在长，
   或 X 的槽位内容与另一个 id 串了，假设成立；若 X 照常涨，则**这条也否证**，P2 就得回到
   "为什么 `result.bytes_written` 累加不到 X 的格子"以外去找（比如读的是不是同一个 `g_tasks` 实例/
   两个数组之间的映射）。
测具位置：`tools/engine_selftest.c` 里新开一节即可，`p0_rerun.sh` 不用改（它跑的就是全量）。


**[35] 探针跑过两轮，结论是"未决"，而且我自己找到了它未决的原因（07:10 交）**
判据是"删掉排在前面、`worker_alive==0` 的空闲任务（`dlmgr_remove` 因此走 `detach_slot` 的 memmove 分支）
之后，正在跑的任务的 `downloaded` 必须继续前进"。两轮读数：
- 第一轮 `downloaded 2,097,152 -> 3,145,728` ⇒ 前进了（假设不支持）；同轮我原先的"文件必须增长"判据
  把这一轮误判成"未决"——**那是我的代理判据坏了**：多路 Range 并行写先 seek 到高偏移，`.idmtmp` 首采
  就等于全长 `41,943,040`，它对"有没有在前进"零信息。已把长度从判据里删掉，只印出来做对照。
- 第二轮（判据修正后）`downloaded 4,194,304 -> 4,194,304` ⇒ **冻住**，形状与假设一致。

**但这轮红我不留在仓里冒充结论**，因为它和"下载已经完成所以不再前进"这两种成因在我现有的读数量下**分不开**
——详情行里只有 `downloaded` 与 `.idmtmp` 两个数，没有 X 的 `status`/`chunk_count`/`chunks_done`。
⇒ 已 `git checkout -- tools/engine_selftest.c` 把探针退回（`REBUILD_RC`/`exe_sha12` 见上），
仓库回到 `173/2` 的形状，**不带着一条解释不清的红**。

**下一轮要加的量（三个，缺一不足以判）**：详情里同印 X 的 `status`、`chunk_count`、`chunks_done`；
判据改成条件式——`status` 仍是 RUNNING/PAUSED（未到终态）而 `downloaded` 不变 ⇒ 成立；
`status==3` ⇒ 本轮未决、要求重跑或把 URL 换成 `/huge.bin` 之外的更大目标。
两个数都要印，因为这条假设的预测是"**冻结**"，而完成也会造成冻结。


**[35] 补上三个量之后跑出来了：`status` 停在 1，"完成"这一解释被排除；但还缺阴性对照，所以不算定案（07:24 交）**
本轮把 `status`/`chunk_count`/`chunks_done` 一起印进详情行（判据也改成条件式：前进⇒不支持假设；
未到终态而冻结⇒假设的预测成立；到终态⇒未决、不许当通过）。实测 `173 通过，3 失败`，
T3 07:22:25→07:24:04，exe_sha12=ba9b7fba3ac6，objs=1 真重编，起前后监听 0/1、`server_still_alive=no`。
新增那条红的逐字读数：
```
[35 读数] remove(idle=7342) 前后：downloaded 4194304 -> 最大 4194304（前进=0）；status 1 -> 1；
chunk_count=10 chunks_done=0；.idmtmp 41943040 -> 41943040（长度不作证人）
```
**它证明了什么**：6 秒观察窗里 `status` 始终是 1（RUNNING）⇒ 冻结**不是**下载完成造成的，
上一轮那条"两种成因分不开"的顾虑被这三个量解决了。
**它还没证明什么**：没有**阴性对照**——"不删空闲任务时，同一个任务在同一时刻本就可能停住"（40MiB 夹具、
2 条线程、服务端有延迟）。缺这一条，"是删除导致冻结"与"下载自己此刻没动"仍分不开。
⇒ 探针**已 `git checkout -- tools/engine_selftest.c` 退回**（REBUILD 后 exe_sha12 见上，仓库回到 173/2），
不带着一条缺对照的红进下一轮的门。

**下一轮要做的那一次运行（就差这一步）**：同一份探针跑两面，`remove(idle)` 与**不 remove**（用一个只在
测试里存在的环境开关跳过删除），其他一切相同；判据是**差分**——
删除面冻结、对照面前进 ⇒ 槽位前移坐实；两面都冻结 ⇒ 冻结与删除无关，假设再次否证，P2 回到
"为什么 running 中 downloaded 不涨"这一族去找（先看 `chunks_done=0` 而 `downloaded=4MiB=一整片`
这个自相矛盾：片长 40MiB/10=4MiB，恰等于一片，可 `chunks_done` 却没跟上）。
两面都必须把 `[35 读数]` 印进日志（不是口头"跑过了"）。


**P2 定性要改一处：它不是"恒为 0"，而是"进度只在 1MiB 块边界发布一次"**（07:4x，改代码前先纠前提）
两处自相矛盾的证据都在我们自己的门里：
- 若真是"恒为 0"，那两条前提检查就**不可能有任何一轮是绿的**——可它们在 `170/0`、`173/2` 这些轮里就是绿的。
  ⇒ **"恒为 0"被我们自己的门禁否证过一次**，我之前按 Task#17 的措辞把它写成"恒"，定性偏重了，现在收窄。
- Task#17 现场与 `[35]` 读数放在一起看：`GET首次非零=i=5`、`tmp首次>0=i=1`、20 秒里计数器 `i=-1`；
  而 `[35]` 那一轮 `downloaded` 明明到过 `4,194,304`。⇒ 计数器**会**动，只是动的节拍是**一整块（1MiB）**。
  夹具侧有 40ms/次延迟、带宽 20KiB/s，`throttle_limit` 就在那条路径上 ⇒ **一个 1MiB 块可以几十秒才完成一次累加**，
  在这期间 `t->downloaded` 保持旧值，用户读到的就是"0 或不动"，而字节确实一行行在落。
⇒ 所以 P2 的真实形状是**发布粒度**（progress 只在块边界 +=），不是"从不累加"。
  修法形状：在 curl 写回调里就把到达的字节计入（或给 `NetDownloadResult` 之外再加一个"本次到达"的即时累加通道），
  让 `t->downloaded` 与写盘同步前进；`chunks_done` 仍按整片判定不动。
  ⚠ 这条修法要动 `network_curl.c` 的写回调 + `download_core.c` 的累加点，属于**第二个改 `src/` 的授权范围**，
  且判据要新写：`downloaded` 必须在**首个 1MiB 块完成之前**就 >0（拿 100ms 采样、断言首次 >0 的采样号 < 块完成号）。
**我不在这一轮动它**：我剩下的余量不足以安全写完网络层改动 + 两次编译 + 两轮全量（各 86s）+ 归档 + 提交，
半途留下的写回调改动比不改更危险（它直接碰数据落盘路径）。交接点：本条 + §7 第 13 条的两面差分。


**07:5x 差分跑完：假设坐实，P2 升级为 P0 级（跨任务污染）。探针 `[35]` 已写进 `tools/engine_selftest.c`（测量形态，不进退码）**
同一个二进制、同一个夹具、只切一个环境变量 `IDM_P35_NOOP`，T3 07:51:27 建好；
两面各跑一次全量，源日志 `selftest-1001-075131.log`（实验面）与 `selftest-1001-075317.log`（对照面）：

| 面 | 扰动 | `[35 读数]` 起点 → 扰动后最大 | 前进 | status | 全量 |
|---|---|---|---|---|---|
| `noop=0` | `dlmgr_remove(前面的空闲任务)` | 2,097,152 → **2,097,152** | **0** | 1→1（仍在跑） | `174 通过，2 失败` |
| `noop=1` | 同样 pause+start，但**不删** | 2,097,152 → **4,194,304** | 1 | 1→2（我主动暂停） | `176 通过，0 失败` |

同起点、同时间预算、同样发生过一次 `dlmgr_start` 扰动 ⇒ **冻住只能由那次 `dlmgr_remove` 造成**。
第二重独立证人：实验面红的那两条正是 #17/P2 的前提检查（`downloaded=0 …`、`chunks 带进度=0`），
对照面它们全绿 ⇒ **"这两条为什么一直闪"有了机制解释**：前面有没有删过任务，决定槽位是否已前移。

**根因**：`detach_slot()` 的 memmove 移的是**整张 `g_tasks` 数组**，而 `pool_worker`/监督线程持有的是
**建线程时抓到的指针**；`dlmgr_remove` 的守卫只查**被删任务自己**的 `worker_alive`。
⇒ 删一个空闲任务会把后面**正在跑**的任务整体前移一格，线程继续往旧格子写。
`dlmgr_remove` 的注释自己写着后果：「…乃至『下载完成』状态写到那个无辜任务身上，而那个任务的文件会被
重命名成当前这个任务的名字。**跨任务污染，且无法事后察觉**」——它防住了被删的那个，没防住后面的。
⇒ 定级：**P0 级（数据/状态可落到无辜任务身上）**，P2 的"进度恒 0"是它的**症状**，不再是独立一条。

**修法二选一（都要新写判据，不在本轮动）**：① 工作线程每次更新按 `task_id` 重查槽位（改动小、每次多一次查表）；
② 放弃数组压缩，改空洞 + 按 id 寻址（改动面大，但把"指针可失效"这一类彻底消掉）。
判据形状：A 面在扰动后 `前进=1`（与 B 面一致），且那两条前提检查在**有前序删除**的轮里也必须绿。
本轮 `src/` 未动；探针是**测量**（`check` 只要求两面都有非零起点），因为"是否冻住"是计时量、写成断言会闪。


**08:14 延后压缩变体落地并双向证明：门禁首次 `177 通过，0 失败`（`M=0` 是真的达到了，不是放宽）**
改动三处：`DownloadTask.pending_detach`（新字段）；`detach_slot()` 在动 memmove 前问
`any_worker_locked(task_id)`——**除自己以外**还有线程活着就不压缩，只登记待摘，同时把删除这件事
**当场生效**（`removed=1` + `TASK_CANCELLED` + 从等待表撤走），否则 GUI 点"删除"会因为别人在跑而毫无反应；
`start_task_threads()` 末尾（本任务线程全部退出、且不再使用 `t` 之后）调 `sweep_pending_detach_locked()`
从后往前清扫。数组稠密这个不变式保留 ⇒ 那 29 处 `g_task_count` 引用与 `find_task` 的二分**一处都不用动**。

三面读数（同一个 `engine_selftest.exe`，除变异那次）：
- 修好、两面各跑：`noop=0 起点=4,194,304 扰动后最大=27,262,976 前进=1 chunks_done=6`（`176/0`，T3 08:07:05→08:08:08，exe `48bb9851d206`）；
  `noop=1 起点=5,242,880 扰动后最大=7,340,032 前进=1`（`176/0`，T3 08:08:18→08:09:36，同一个 exe）。
  **两条 #17/P2 的前提读数在两面都绿了** ⇒ 它们"闪"了十几轮的成因就是这条错位，不再归给发布粒度。
- 变异（把 `any_worker_locked` 首位塞 `return 0` ⇒ 等于回到修前行为）：**`174 通过，3 失败`**，
  红的正是 `[35] 扰动之后记账必须继续前进`，读数 `noop=0 起点=4,194,304 扰动后最大=4,194,304 前进=0`（exe `63034333f601`）。
- 撤掉变异、重编、复跑：**`177 通过，0 失败`**（T3 08:13:03→08:14:03，exe `8af9e43168de`；+1 是新加的那条断言）。
  三道文本门 `lint_layering=0 live_roster=0 lint_control_bytes=0`。

新加的那条断言就是 `[35]` 的牙：两面都断言"扰动后必须前进"（不按 env 分支），因为对照面同样不该倒退，
回归时无需知道跑的是哪一面。残余：`pending_detach` 的槽在清扫前仍会被遍历到（最多延后到最后一个任务收工），
这是我把"指针安全"换"数组立即收缩"的取舍，明写在函数注释里。


### 2026-10-01 08:33（第 14 条）— 17 个 UI 探针模式第一次在**本机**真跑过：`PASS=62 FAIL=0 零断言模式=0`
写档器 `tools/ui_probe_run.sh` 入仓。它自己踩了一次假绿，形状值得留：

**第一遍全 0**。脚本原本用 `env "$m=1" "$BIN"` 传模式开关 ⇒ 15 个断言型模式**每个 rc=0、日志 0 字节**，
`grep -ac` 数出来 PASS=0 FAIL=0，退出码一路 0。要是只看退出码，这就是"62 条断言全过"的另一个名字。
⇒ 修成 `export "$m=1"; "$BIN" > "$log" 2>&1; unset "$m"`，并把"断言型模式里一条断言都没有"单独算失败
（`零断言模式=` 那一行就是它的旗子）。

**四面实测传参形式**（同一个 `build/ui_snapshot.exe`，`exe_sha12=e9ebbba101e2`，只切 `IDM_ZOOM_PROBE`）：

| 传法 | rc | 日志字节 | `[PASS]` |
|---|---|---|---|
| `VAR=1 exe`（真前缀赋值，用 `eval` 拼出来才测到） | 0 | 546 | 4 |
| `export VAR=1; exe`（本脚本这条） | 0 | 546 | 4 |
| `env VAR=1 exe` | 0 | **0** | **0** ← 静默没跑 |
| `"$M=1" "$BIN"`（我加引号写错的那次） | **127** | 60 | 0 |

第四行是**我测法的破**，不是机制的破：bash 的"这个词是不是赋值"发生在展开**之前**，加了引号就当成命令名去找 ⇒ 127。
下一轮别据此写"前缀赋值在这台机器上不行"。脚本走 `export` 而不是前缀赋值，是因为模式名在变量里、
只有 `eval` 才能拼出前缀赋值形式，而 `eval` 的输入是从二进制 `strings` 扫出来的名字——
虽然 `grep -oE 'IDM_[A-Z_]+_PROBE'` 是白名单，也不值得为省一行开这个口。

现行读数（T3 08:33:22→08:33:44，22s，`RUNNER_RC=0`）：`modes=17`，15 个断言型模式
`PASS=62 FAIL=0 零断言模式=0 问题模式: 无`；另 2 个（`IDM_ICON_PROBE`/`IDM_ARROW_PROBE`）是**人眼对照表**，
刻意排除在闸门名单外、不进这个总数。**本轮没有重编**：源码一字未动，直接复用 08:25 的 exe（`objs` 不适用）。
总数 62 我用第二套实现复算过一遍：不读汇总行，而是从盘上 15 份 `<模式>.log` 逐个 `grep -ac '\[PASS\]'` 相加
⇒ 58（前 14 个模式）+ 4（ZOOM）= **62**，与脚本自己的累加器一致；17 份日志最小 349 字节，没有一份是空的。

**这条覆盖不到什么**（写清楚，别被读成"UI 没问题了"）：
① CI 侧那道 `N≥100 且 M=0` 的判据本轮没碰、也没在 runner 上执行过 ⇒ 与之前一样是**永久验证缺口**，
本机这 62 条不能顶替它；② 探针跑的是 `QT_QPA_PLATFORM=offscreen` 的 `ui_snapshot.exe`，
量的是"控件状态与落盘"，**不等于**真机窗口里的观感（美观另走截图那条线）；
③ `IDM_IPC_PROBE` 依赖"该端口上只有一个我的实例"，本轮起前后监听 0/1，但脚本没把它做成硬断言。

产物清理：`~/idm_shots` 下截图与 `_data\tasks.json`、`_data\history.db` 当场删；
`_data\aria2|ffmpeg|yt-dlp` 是用户自己的工具缓存，**保留未动**（脚本第 47 行那条打印就是给这件事留证人）。

**这条 62/0 后来被实测驳回了一次（同日 12:05），改写成"带分母的读数"**：
`tools/ui_probe_run.sh` 复跑第二轮（T3 12:05:32→12:06:00，同一个 `exe_sha12=e9ebbba101e2`、源码一字未动）
报 `PASS=61 FAIL=1`，红的逐字原文：
```
[FAIL] 一条空转连接占着 GUI 线程时，后面那条正常请求仍 <500ms 拿到回话（实测 3263ms）
```
第三轮（T3 12:07:35，仍同一个 exe）`PASS=62 FAIL=0`，同一条读数 **218ms**。
单独把这一个模式重跑 5 次：`219 / 221 / 216 / 216 / 217 ms`，**5/5 绿**。
⇒ 分母是 **1 红 / 7 次**（两轮 sweep + 五次单跑），不是"稳定红"，也不是"没有红"。
同机当时在跑的进程普查：`svchost 93 / bash 16 / msedge 14 / Qoder CN 13 / node 8 / postgres 6 / cmd 5 / mysqld 4 / java 4`
⇒ 有一个说得通的机制（离屏 GUI 线程被抢 CPU），**但我没有把它证成结论**：要证得在负载受控时同批复跑，
本轮没做。所以这条留在报告行，**判据一个字不放宽**（500ms 是产品自己的断言，不是我的门；
把它抬到 3500ms 就等于把"空转连接会不会拖住 GUI 线程"这个问题从世界上抹掉）。
写在这里的目的：下一轮如果又见到这条红，先看当时机器上有多少 `bash/Qoder/node`，再决定是不是产品问题。


### 2026-10-01 12:0x（第 15 条）— 开源范围与许可证事实入仓：`NOTICE.md` + `tools/notice_verify.py`
主控令：范围与许可证**怎么定是用户的决定**，本轮不新增/不修改任何 `LICENSE`、不推远端、
不碰 `D:\visual studio\lianxi\swoop\idm` 那棵树。入仓的是**事实 + 复算命令**，形状按 §1.2 那套：
每个数字都写清"哪个文件、哪条命令、命中数"，并由 `python3 tools/notice_verify.py` 从盘上重算，
不一致 `exit 1`（只读、不联网；要联网核的两行在 NOTICE §2 里标了"⚠️ 要联网"，不进断言）。

**许可证真相（这轮推翻了"仓库没有许可证"这句过于笼统的话）**：

| 结论 | 一手证据 |
|---|---|
| `master`（IDM Next）**没有**许可证文件 | `git ls-files` 里匹配 `(^|/)(LICENSE\|LICENCE\|COPYING\|UNLICENSE)(\.\w+)?$` 命中 **0** |
| GitHub 徽章显示 **MIT**，它取自**默认分支 `main`**，不是 `master` | `git symbolic-ref refs/remotes/origin/HEAD` ⇒ `refs/remotes/origin/main`；`GET /repos/159357yangjun/swoop/license` ⇒ `path="LICENSE"`、`sha=594869ea…`、`size=1063`、`spdx_id="MIT"` |
| 那份 MIT 具体是哪个文件 | 默认分支根目录 `LICENSE`，blob `594869ea5e6b28156af2402367269f2c28f2fb62`；本地 `git rev-parse origin/main:LICENSE` **逐字相等** ⇒ 本地 ref 与公开内容当时一致 |
| 所有者已经把 MIT 换成 **GPL-3.0-only**，但**没推** | 只读普查那棵树：HEAD=`80a1db7 2026-09-25 license: MIT → GPL-3.0-only（含源文件 SPDX 头）`；`git branch -r --contains 80a1db7` **空**；`rev-list --left-right --count origin/main...HEAD` ⇒ **21 2** |

⇒ 三条叠起来的实际风险（写进 NOTICE §2 的原话）：公开读到的仍是 MIT，
所以第三方可以按 MIT 拿走**包括浏览器扩展在内的全部内容**，而所有者心里的许可（GPL-3.0-only）还没生效。
`browser-extension/`（19 个文件 / **656497 字节**入库）自 `874427e`（2026-09-19）就在公开历史里 ⇒
**这句声明撤回不了已经公开的东西**，能做只有：定 `LICENSE`、改写公开历史（force-push，不由我做）、或把 L7 推上去。

**我自己两处数字失实，当场更正**（不是措辞问题，是数字）：
① 上一轮我说扩展目录"19 文件 **16 MB**"。实测：盘上 25 个文件 / **16473007 字节**，被跟踪 19 个 / **656497 字节**——
16 MB 是**磁盘数**（含 6 个未入库的 windeployqt 产物，`Qt6Core.dll` 11.3MB 等），公开暴露的是入库数。
两个数从此分开写、`--verify` 各自核，禁止再用"约 16MB"混过去。
② 上一轮我说过"全仓**无 LICENSE**"。准确形状是上表 L1–L4：`master` 没有，默认分支 `main` 有，
徽章因此显示 MIT。**笼统那句会误导下一个做决定的人**（他会以为没有可继承的许可条款）。

顺带登记两处盘上不一致（都写进 NOTICE，由 `--verify` 核）：
`.gitignore:31-32` 声明忽略扩展目录的 `*.exe|*.dll`，但同一目录下 **3 个 `.dll` 是被跟踪的**（规则对已跟踪文件无效）；
`.gitignore:49` 把 `third_party/libcurl/docs/` 整体排除，而实测该目录 59 个文件里**一个许可证全文都没有**——
上游 10 个组件（curl/LibreSSL/nghttp2/nghttp3/ngtcp2/libssh2/zlib-ng/zstd/libpsl/brotli）
的许可全文**一份都没进仓**，版本却全部有 `文件:行` 锚点可查（NOTICE §3 那张表最后一列就是这件事的可见性）。

**残余（不得读成"开源合规做好了"）**：`LICENSE` 仍不存在于 `master`（等用户定）；
上游许可全文未收录（要联网或从上游 zip 取）；徽章与公开性两行需要联网，`--verify` 到不了；
`origin/main` 有可能过期——本机 `git ls-remote` 到 `github.com:443` **21 秒超时**，
所以 L3 的"逐字相等"是**本地 ref 与 API 读数**相等，不是与 fetch 后的 ref 相等。

**这道门落地当场抓到的两处，以及它自己"先红"的形状**
`tools/notice_verify.py` 第一版跑出来 5 条 `FAIL`，原文（逐字已存 `docs/notice-verify-red-2026-10-01-121055.txt`）：
```
[FAIL] tools/ 文件数｜盘上=24 文中=26
[FAIL] tools/ 字节数｜盘上=479919 文中=496421
[FAIL] 仓根散装文件数｜盘上=9 文中=10
[FAIL] 合计文件数｜git ls-files=324 文中=327
[FAIL] 上游 docs 文件数｜盘上=57 文中=59
```
前 4 条是**预期的红**：那三个文件还在盘上、没进 `git ls-files`——"下一个只拿到仓的人"看到的确实比文中少，
这正是这道门该说的话；提交落库后同一命令转绿（`--verify` 在最终 HEAD 上重跑，见本轮报告）。
**第 5 条是它抓住我的一条真错**：我在 NOTICE 里写"上游 docs 里 59 个文件没有一个许可证全文"，
那个 59 是 `ls | wc -l` 数的**条目**——目录里还有 `examples/`、`libcurl/` 两个子目录。
改成递归后的真数是 **188 个文件**（顶层 57 文件 + 2 子目录），且**整个 `third_party/libcurl/` 递归内
匹配 `LICENSE|COPYING|NOTICE|UNLICENSE` 的文件 = 0**（这条比原来那句更强，也更接近我要的主张）。
⇒ 顺带把量具改成核"递归 + 顶层拆分"三个数而不是一个数，并把文中那个易误读的单一计数拆开了。
`python3` 在本机不存在（`rc=127`，响亮），所以 NOTICE 头部那句运行命令改成 `python`——
这类"命令在这台机器上根本没有"的坑，只要让它**响亮**就不是问题，让它**静默 0** 才是问题。

**本轮收尾三笔提交**（本地，不 push）：① 探针跑批器；② `NOTICE.md` + `tools/notice_verify.py` + 上面那份红档
（声明与它的量具必须同一笔，否则声明里指向的文件在仓库里不存在＝装饰）；③ 本文档。
②落库后 `--verify` 又自己报了一条新红：`[FAIL] tools/ 字节数｜盘上=496980 文中=496421`——
差 559 字节，是量具自己在那两次修补（`tok_re` 扩到能吃 `.gitignore:NN`、L6 那条从"永远通过"改成真有牙）之后
变胖了，而我把 §1 表里的字节数留在了修补前算的那次读数上。
⇒ 这就是"改了被台账登记的文件的同一笔里必须重签"的形状，而且**这次是门在提交之后仍然拦住了我**；
修法走第 ③ 笔（改文中数字），不 amend 第 ② 笔，红档 `docs/notice-verify-red-2026-10-01-121055.txt` 原样不动
（它是那次运行的逐字记录，不能被后来变绿的结果覆盖）。
除了这两次真实漂移，还单独演了一次**篡改**（把 NOTICE §3 里 nghttp2 的证据原文 `1.70.0` 改成 `1.70.1`，
只改这一处）：`--verify` 回 `RC=1`，两条红各自点名自己和那行——
```
[FAIL] third_party/libcurl/include/nghttp2/nghttp2ver.h:32｜第 32 行实际是「#define NGHTTP2_VERSION "1.70.0"」
[FAIL] nghttp2｜版本 1.70.0 在本行 1 条证据原文里都找不到
```
改回来即 `FAIL=0`。⇒ 锚点那一节不是靠聚合总数说话，指得到具体文件的具体行。
`git status --porcelain` 收尾会**剩且仅剩** `?? idm-next-glass-ui.html` 一行——
它是用户对盘上文件的现行指令（保留、不入库、不删，见 §7 第 4 条与"别碰"清单），
"porcelain 必须为空"这条巡视令与那条现行指令**不可能同时成立**，我不擅自删文件、也不擅自把它入库，
原样报给他判断。该文件本轮实测：16684 字节、mtime 2026-08-14 06:58，
标题「IDM Next · 柔雾玻璃仪表盘」，纯静态样式稿（Tailwind CDN + Google Fonts），
与代码零联系（`grep -c "glass_effect\|MainWindow\|\.cpp"` = 0）⇒ **它既不是探针产物也不是页面交付物，
是设计稿，应被记录而不应被删**，而它已经在本文档 §7 第 4 条被记录了。


### 2026-10-01 12:39（第 16 条）— 负载差分把"机器负载"这一支否掉了一半：`C 证据不够`，但排除项是真的
令：把 `IDM_ACTIVATE_PROBE` 那条 1 红/7 次归到"机器负载"还是"产品真会堵 GUI 线程"。
跑批器 `tools/load_diff_run.sh` + 分布器 `tools/load_diff_stat.py` + 发生器 `tools/load_worker.py`，
原始输出全量归档 `docs/load-diff-2026-10-01-124110.txt`。同一个二进制 `exe_sha12=e9ebbba101e2`（未重编，
脚本先比 sha、不等就 `exit 3` 而不是拿旧二进制冒充新读数），每轮都带 `IDM_ROUND_ATTR=1`。

三面各 5 轮，报分布不报单次（`单轮最长` 就是那条 <500ms 断言的实测；`B 段往返` 是产品可见的轻对照）：

| 面 | 标定 pre（固定功量忙等） | 负载比 | 该轮墙钟 | 单轮最长 min/median/max | B 段往返 min/median/max | C 段 ack | PASS/FAIL |
|---|---|---|---|---|---|---|---|
| idle | 486–555ms | 不适用 | 4–5s | **212 / 215 / 217** | 6 / 7 / 14 | 50/50×5 | 35/0 |
| load（16 忙等×16 核） | 935–1370ms | **1.78–2.61** | 8–10s | **226 / 246 / 293** | **20 / 47 / 96** | 50/50×5 | 35/0 |
| idle_after（等长 183s 空载） | 491–579ms | 0.94–1.10 | 5s | **215 / 216 / 218** | 13 / 14 / 14 | 50/50×5 | 35/0 |

**四条读数说的事，一条比一条硬**：
1. **负载真的上了机器**：标定 525→1009ms（×1.92）、墙钟 5→8.5s（×1.7）。没有这条证人，"负载面"三个字是空的
   ——而且**我第一版就是空的**：worker 用 `python -c "多行" >/dev/null 2>&1 &` 起，traceback 被我自己的重定向吃掉，
   16 只里当场死掉几只看不出来，那一面 calib 492–573ms 与空闲无异 ⇒ **整面作废、重写发生器**
   （现在 stderr 落盘、`worker_stderr_非空数>0` 就响亮报错，且每轮算负载比、`<1.5` 的轮不计入负载面）。
2. **轻对照漂得比被测项更狠**：B 段往返 7→47ms（×6.7），而 `单轮最长` 只 215→246ms（×1.14）。
   按"轻断言也漂 ⇒ 是机器"的口径，这一轮的**变动方向**确实归机器。
3. **但负载造不出那一次红的形状**：15 轮里 `单轮最长` 最高 **293ms**，红的值是 **3263ms**（差 11 倍）；
   而且负载面每一轮 `IPC占比=97–100%`、`peakN=1`、`取尽自身峰值≈单轮最长`——
   红的那一轮是 `IPC占比=7%`、`取尽自身峰值=217ms`、`单轮最长=3263ms`，**93% 的时间不属于 IPC**。
   ⇒ 同一支 CPU 负载把"IPC 排空那一轮"按比例放大，却**没有**把不属于 IPC 的东西变长。这不是"负载不够"能圆过去的：
   `handleClient` 的读循环是 10×100ms 有界的（单条连接最多约 1s），**3263 比这个上界还大**，
   所以那 3.26s 根本不在 IPC 路径里，而在同一轮 `processEvents` 内的别的东西上。
4. **本该定案的那次归因没跑成**：消融是 `worstLoopMs ≥ 500` 才执行的（"消融只在复现的那一轮才有意义"），
   本轮 15 次都没复现 ⇒ 没有一轮拿到 `有定时器→停定时器→再有定时器` 的 on/off/on。
   而 12:05 那唯一一次复现，sweep 没设 `IDM_ROUND_ATTR` ⇒ **日志里连一行 `[attr]` 都没有**。
   已改：`tools/ui_probe_run.sh` 现在对 `IDM_ACTIVATE_PROBE` 固定带这个开关（不复现时零成本，只多印一行"未复现"）。

**定性：C —— 证据不够，但排除项是真的**。
被排除的是"**纯 CPU 抢占**能解释那 3263ms"这一支（×1.92 的 CPU 负载只把有界的 IPC 轮推到 293ms）；
没被排除的是：内存/磁盘/Defender 扫描这类非 CPU 抢占的外部占用，以及产品里一条**尚未复现**的 3 秒级 GUI 线程占用。
累计分母：**1 红 / 24 轮**（08:33、12:05、12:07 三轮 sweep + 单跑 5 次 + 预检 1 次 + 本轮 15 次）。
判据没动：500ms 照原样红/绿，本轮新增的负载比 1.5、"至少 3 轮满足"都是**量具是否成立**的规则，不进产品的退码。

**能不能归因到用户可感知的卡顿**：如果那 3.26s 发生在真实窗口里，就是"点下去到窗口有反应之间卡住 3 秒"，一定可感；
但我这轮所有观测都是 `offscreen`，且事件在 24 轮里只出现过 1 次、复现时又没带消融
⇒ 只能说"**若复现即可感**"，不能说"用户会遇到"。反过来，正常分布里那 **208–293ms** 的 GUI 线程独占
（一条空转连接让 `handleClient` 的有界读循环占住主线程约两轮 100ms）是**每轮都发生**的、
且是直接占着主线程的时间——它与"重试收到两次 activate / 服务端慢回话"那个原始问题是同一处，
只是量级比那次红小一个数量级。

**下一轮要做的两件事**（都不涉及改判据）：① 等它复现——现在复现那轮会自动带 on/off/on 三窗口，
一次就能把"时间花在定时器回调上"这条判掉；
② 换负载类型再打一次（磁盘/内存压力，或把 `LOAD_WORKERS` 提到 32 并让每轮同时采样同机 CPU），
因为本轮只证伪了 CPU 这一支。

**消融开关已经接进 sweep，并当场验了一次（12:54，`f0fbaad` 之后）**：
`bash tools/ui_probe_run.sh` ⇒ `PASS=62 FAIL=0 零断言模式=0 问题模式: 无`（同一个 `e9ebbba101e2`，未重编），
`IDM_ACTIVATE_PROBE` 的日志里第一次出现了归因行：
```
[attr] 定时器 5 个：
[attr] 本轮未复现（D 段单轮最长=213ms < 500，IPC 自身峰值=213ms）⇒ 跳过消融；消融只在复现的那一轮才有意义
```
⇒ 以后**只要复现一次**，那一轮就自动带出 `有定时器→停定时器→再有定时器` 的三窗口对照，
不再需要谁事后想起去补开关；不复现时它只多印两行、不进退码（本轮 62 条与改动前逐字相同，是回归证人）。
顺带量到一个数：MainWindow 子树里现在 **5 个 QTimer**；而消融那段注释回忆第一版误判时写的是"4 个定时器全被判成元凶"
——⇒ 定时器数目在这两条记录之间变过（+1 是 08:14 那笔 `pending_detach` 引入的清扫/监督侧改动带进来的还是别的，本轮没查），
所以注释里的 4 与盘上的 5 不能互相顶替，谁引用谁都要重新数一遍。


### 2026-10-01 13:2x（第 17 条）— 拆名这活我**停在边界上**：三件事全要重编才能验；同时撤回我上一轮一句不准的话
主控令：把 `IDM_ACTIVATE_PROBE` 那条断言的名字、判据、实测量对齐，并做一次真红演示。
**本轮没有动 `tools/ui_snapshot.cpp`**，因为①②③三件全部要求重编——判据原文就编在二进制里：

| 检查 | 命令 | 读数 |
|---|---|---|
| 判据文本在不在二进制里 | `grep -ac '空转连接占着' build/ui_snapshot.exe` | **1** |
| 正例对照（同一方法必须看得见别的东西） | `grep -ac 'IDM_ACTIVATE_PROBE' build/ui_snapshot.exe` | **1** |
| 反例对照（同一方法不该瞎答应存在的东西） | `grep -ac 'idle-behind-request' build/ui_snapshot.exe` | **0** |

第一版我用 `strings -n 8 … \| grep -ac '空转连接占着'` 拿到 **0**——那是 `strings` 看不见 UTF-8 中文，
**是我的方法瞎，不是串不存在**；换 `grep -a` 才有上面三行。只在源码里改名而不重编，跑的还是
`e9ebbba101e2` 那个旧二进制，那样交出去的"拆名后读数"是假的（本仓已经吃过一次旧 exe 冒充新读数）。
⇒ 按令中"必须先停下报给我"处理，不自行动手。

**撤回与更正**（主控这条令的依据有一部分是我上一轮的话，那句话不准）：我写过
"同一次红轮里，真正用户可见的那条请求是 `B 段放弃后往返=15ms`"，读起来像"红轮里那条被压住的请求只花 15ms"。
**这个对照不成立**：`B 段`量的是**前一条已写出数据后放弃**的连接之下、第二条的往返，而且它在 `C 段`压测与
`D 段`空转连接**之前**就跑完了；`D 段`窗口内**从头到尾没有发出过任何请求**
（`tools/ui_snapshot.cpp:1360–1400` 里只有 idle 连接的 `connectToServer` + `msleep(2500)` + `abort` 与
`processEvents` 采样，没有一次 `askActivate`）。⇒ 3263ms 那一次，
"**被空转连接压在后面的那条请求的真实完成时间**"**从来没被量过**——既不是 15ms，也不是任何数。
所以标签的问题不是"名字大于实测量"这么轻：`:1505` 那句"<500ms 拿到回话"**承诺了一个本段里不存在的事件**，
而判据 `ok7`（`:1503`）兑现它用的数是 `idleBehindMs`（`:1399 = worstLoopMs`，事件循环单轮最长）。

**而且那个"再问一次、量它的往返"的测法已经试过并废掉过一次**（`:1349` 注释原话：第一版这么测出 14ms，
因为阻塞整个发生在某一次 `processEvents()` **内部**，GUI 线程上的 150ms 等待循环正好把这一秒吞在自己的计时之外
⇒ 量到的是"事后"）。⇒ 拆出来的第二个读数**不能**再由 GUI 线程自己量。

**下一轮可直接照做的施工图**（三处，全在 `tools/ui_snapshot.cpp`，不碰 `src/`）：
1. **改名**：`idleBehindMs` → `worstEventLoopTurnMs`，判据与打印的字面同步改成
   "最长一次事件循环轮次（=GUI 线程被独占多久）"。
2. **补第二个具名读数** `idleBehindReplyMs`：在 `D 段`窗口内发一次真实 `activate`，
   **复用 `askActivate`**（`:1248` 起，它本来就跑在**独立 `QThread` + 本线程 `QEventLoop`** 里，
   正是绕开 `:1349` 那个坑所需要的形状），量它的 ack 往返，与 `worstEventLoopTurnMs` 并列打印。
3. **判据只留一个，且阈值一个都不改**：500ms 继续挂在 `worstEventLoopTurnMs` 上。机制理由三条——
   ① 它是这条链上**唯一抓到过 3263ms** 的证人，把判据挪走等于放走唯一证人；
   ② `idleBehindReplyMs` **今天没有分布**（一次都没量过），给它配 500 就是**拍阈值**；
      正确顺序是先攒 ≥N 轮、知道正常值与抖动振幅，再谈它进不进判据（规则数可写死、实测数禁拍）；
   ③ 因为阈值数字不动，本轮不存在"为了让它绿而改阈值"的空间。
   ⇒ `idleBehindReplyMs` 第一次落地只进**报告行**、不进退码，并在同一行印"本轮首次测量、无基线"。
4. **真红演示的形状，含一个还原陷阱**：扰动必须打在**进判据的那个量**上——在测具自己的 `D 段`窗口里
   注入一次可数的忙等（例如采样前临时 `msleep(800)`，只改这一处、工作树其余部分干净）。
   预期 `ok7` 红，**且** `[act] D 段归因` 的 `IPC占比` 同步掉下来（注入的时间不属于 IPC）——两个证人同向才算演成。
   **还原的相等证人不能用 exe 哈希**：链接时间戳会让 exe 哈希在源码未变时也变（09-30 已实测过一次），
   所以还原证人 = `git diff` 为空 **且** `git hash-object tools/ui_snapshot.cpp` 等于改动前记录的哈希；
   exe 只报新哈希并写明它必然与旧哈希不同。

**本轮交付的合法子集**：上面的施工图 + 三面 `grep -a` 对照 + "`D 段`无请求"的行号证据 + 对我自己那句话的撤回。
**没有交付代码改动**，因为它在这一轮无法被验证（未跑过的编辑不算交付）。


### 2026-10-01 14:0x（第 18 条）— 第 17 条施工图落地：两个读数分开了；过程中我自己把一个证人打瞎过
授权范围很窄：只编 `tools/ui_snapshot.cpp` 这一个目标。命令原文
`ninja -C build ui_snapshot`（`[1/4] MOC → [2/3] Building CXX …/ui_snapshot.cpp.obj → [3/3] Linking ui_snapshot.exe`，
**没有编任何产品目标、没有下载任何东西**；工具链本机已有：ninja 1.13.1 / cmake 4.1.0 / g++ 15.2.0，全在 `D:/mingw64`）。
改前身份：`/d/visual studio/lianxi/MFC/idm-next/build/ui_snapshot.exe`，08:25:23，2281493 字节，`e9ebbba101e2`；
改后：14:00:51，`944123ae728b`。两面输出与身份表全量存 `docs/probe-split-before-after-2026-10-01.txt`。

**①② 做完的形状**：`idleBehindMs` 拆成两个具名量——
`worstEventLoopTurnMs`（进判据，500ms 原样挂在它身上，一个阈值都没改）与
`idleBehindReplyMs`（只进报告行，同行印"本轮首次测量、无基线⇒不进判据、不配阈值"）。
旧句 `一条空转连接占着 GUI 线程时，后面那条正常请求仍 <500ms 拿到回话（实测 213ms）`
⇒ 新句 `空转连接占着 GUI 线程时，最长一次事件循环轮次仍 <500ms（量的是 GUI 线程被独占多久，不是请求延迟；实测 215ms）`
+ 新行 `压在空转连接后面的那条真请求：往返=218ms ack=1（同窗口最长轮次=219ms 取尽连接数 peakN=1…）`。
**新读数的第一批数**：单跑三轮 `217 / 217 / 218ms`（各自同窗口最长轮次 `218 / 219 / 220`），`ack=1×3`。
⇒ 那条排在空转连接之后的请求**确实等了约 218ms 才拿到回话**，比它自己那一轮的时长还长一点——
"串行排队"这一解释第一次有了直接证人，而历史上那个 14ms（`:1349` 的错测法）从此不再被引用。
**我不给它进判据**：n=3、同进程连跑、不是独立样本，看不出抖动振幅；阈值要从读数里取，不许拍。

**我自己引入的一个 confound，是量出来的不是猜的**：第一版我把这条真请求**塞进 D 段同一个窗口**，
于是 `取尽自身峰值` 从 213ms 掉到 **0ms**、`IPC占比` 从 100% 掉到 **0%**——
而 0% 正是我拿来判"别的东西占大头"的那个形状（3263ms 那次就是 7%）。
根因在产品代码里（一手证据）：`src/app/ipc_server.cpp:101-104` 写的是
`s_drainStat.lastMs = drain.elapsed()` 即**最近一次**取尽，我这条快请求（约 0ms）的后一次取尽
把空转连接那次（213ms）覆盖了 ⇒ `peakDrainMs = qMax(...)` 采不到被覆盖的那次。
⇒ 我把自己的归因证人打瞎了。**修法**：第二个读数**自己开一份事件**（另起 idle 连接 + 另发请求），
D 段窗口回到原样只留改名。这也正是 `:1349` 那条老教训的同一形状——
"消融必须重开一份被测事件"，不能拿别的窗口的事件给自己的计时作保。
修后证人回来：`取尽自身峰值=215ms 单轮最长=215ms IPC占比=100%` ✓。

**③ 真红演示（原文存 `docs/probe-split-mutation-red-2026-10-01-140023.txt`）**：
只改一处——D 段采样循环里 `one.start()` 之后插一行 `QThread::msleep(800)`，工作树其余部分不动。
结果**两个证人同向**：
```
[act] D 段归因：排空静默=成 peakN=1 取尽自身峰值=216ms 单轮最长=1026ms IPC占比=21%
[FAIL] 空转连接占着 GUI 线程时，最长一次事件循环轮次仍 <500ms（…实测 1026ms）
[report] 压在空转连接后面的那条真请求：往返=221ms ack=1（同窗口最长轮次=222ms peakN=1…）
PASS=6 FAIL=1，模式退出码=1
```
`1026 ≈ 800(注入) + 216(真取尽)`，`IPC占比 100%→21%`（注入的 800ms 不属于 IPC）✓，
而 `[report]` 那一行**没被带上**（它自己那个窗口没被扰动）⇒ 两个量确实是被分开的两件事，不是同一个数换了个说法。

**还原证人（按第 17 条定的形状，且这次是当场实测）**：删掉那一行后
`git hash-object tools/ui_snapshot.cpp = da28821a8b258176d2526adbf988aea09c3986e5` **与扰动前记录的完全相等**、
`grep -c 'msleep(800)'` = 0、`git diff` 只剩我要提交的那 92/5 行。
同一份源哈希编了两次，exe 分别是 `71463b68309b` 与 `944123ae728b` ⇒ **"exe 哈希相等"这条证人被当场否证**，
谁再拿它当还原凭据就是错的。

**④ 我上一轮列的第 5 条，诚实的答案是"仍未证成"**：原样 `ninja -C build ui_snapshot` 报的是
`ninja: no work to do`——那是 **mtime 判定**，不是内容判定，证不了旧 `e9ebbba101e2` 就是 `176f4dd5` 这份源码编出来的。
本轮能给的只有**行为一致性**：新旧两个 exe 的 17 个模式逐模式 `[PASS]/[FAIL]` 计数**全部相同**
（ACTIVATE 7/0、FILEICON 8/0、… 两轮 sweep 都是 62/0），唯一差异就是我改名的那一行与新加的那一行。
"相同源码 ⇒ 相同 exe 哈希"既然已被否证，"相同行为 ⇒ 相同源码"同样不能反过来当证明——
所以这一条我标**未验**，不写成已闭。

**改动范围**：`tools/ui_snapshot.cpp` +92/−5（删掉的就是旧声明 1 行、旧赋值 1 行、旧判据 1 行、旧打印 2 行）；
`src/` 一字未动；`NOTICE.md` §1 的 docs/tools 字节数随新增文件重签；`build/` 里只有 `*.obj`（`.gitignore:3` 已忽略）。

**这张字节表自带一个回旋，本轮撞到了，记下来免得下一个人再撞**：§1 把每个目录的**字节数**登记在 `docs/` 里的一句话，
而"把 `--verify` 的红原文存进 `docs/`"这件事本身会改变 `docs/` 的字节数与文件数 ⇒ 我为了归档红档而新造一个文件，
刚签好的数字立刻又红。本轮的处理：红原文**不再新开文件**，随本轮 commit message 归档（`git show` 可复算），
真正承重的那两份红（扰动演示、上一轮 5 条门红）仍在 `docs/` 里。
落库前那三条预期的红（逐字，已进本轮提交信息）：
```
[FAIL] docs/ 文件数｜盘上=15 文中=17
[FAIL] docs/ 字节数｜盘上=285567 文中=290446
[FAIL] 合计文件数｜git ls-files=333 文中=335
```
形状与 12:10 那次的四条红同一类：**差的是"已入库"与"只在盘上"**，不是数字算错。
提交后 `--verify` 复跑 `FAIL=0`。⇒ 若下一轮还想给字节表加东西，先想清楚它会不会自举；
真要避免，办法是把表里的字节数改成"由 `--verify` 现算现印、文档里只留规则"，但那样公开的暴露量就不再是
可 grep 的静态数，权衡留给你定，本轮不擅自动手。


### 2026-10-01 15:2x（第 19 条）— 出厂二进制过期这件事进门退码：`PRODUCT_EXE_STALE`
① 具名读数（一行，可直接 grep）：

```
PRODUCT_EXE_STALE: build/idm-next.exe mtime=2026-09-30 20:04:54 -0400 sha12=45bbbab03006
  而 HEAD=678826a 的输入集合里最新一次 src 提交=7777bce(2026-10-01 08:14:34, P0 根因修复)
  ⇒ 差 -43780s ⇒ 这枚出厂二进制**不含**该修复
```

复算命令（一行 `stat` + 一行祖先判定 + 门本身）：

```bash
stat -c '%y %s' build/idm-next.exe
git merge-base --is-ancestor 7777bce HEAD && echo "IS_ANCESTOR=yes"      # 本轮实测 yes
git log -1 --format='%h %ct %ad %s' --date=iso HEAD -- src resources third_party/libcurl/include CMakeLists.txt
python tools/freshness_gate.py ; echo "rc=$?"                            # 本轮实测 rc=1
```

② 会自己说的门：`tools/freshness_gate.py`。四个产物各按**自己的输入集合**判，不合格分三种成因不混：
`STALE` / `MISSING`（产物不在，响亮）/ `DIRTY_INPUT`（有未提交的输入，任何二进制都不可能含它）。
退码：**0 全新鲜；1 有不合格；2 门自己没跑到判断**（rev 取不到或内部异常）——
崩溃绝不能被读成"报了红"，所以 1 与 2 分开，且实测过：`python tools/freshness_gate.py --rev 不存在的rev` ⇒ `rc=2`。

接线的两处（都不靠人读文档）：
· `ci.yml` 在 `Verify expected executables` 之后加了一步跑它——放 Build 之后是必须的，Build 之前产物合理地不存在会造假警报；
  **没碰** UI 探针那道 `N≥100 且 M=0`。但 CI 从未执行过 ⇒ 它对今天没有约束力。
· `tools/p0_rerun.sh` 收尾复述它的读数与 `freshness_rc`，但**本脚本退码仍只等于引擎退码**
  ——把两个数混成一个，以后就没人能从这里读出引擎跑没跑绿。

两面夹具（原文各存一份）：

| 面 | 命令 | 结果 |
|---|---|---|
| 当前盘上 | `python tools/freshness_gate.py` | `idm-next.exe STALE(-43780s)`、`idm-next-host.exe STALE(-33221s)`、`engine_selftest.exe FRESH`、`ui_snapshot.exe FRESH` ⇒ **不合格=2，rc=1** → `docs/freshness-gate-2026-10-01-1524-face1-HEAD.txt` |
| 受控反例（取一枚早于所有产物的 rev 当基准，只读不动工作树） | `python tools/freshness_gate.py --rev 6a7cf84` | 四条全 `FRESH`、**不合格=0，rc=0** → `docs/freshness-gate-2026-10-01-1524-face2-rev6a7cf84.txt` |

第二面是必要的：没有它，"这道门会报"与"这道门永远红"分不开。

**建门过程中被实测否证掉的一个证人（这条比门本身更值钱）**：我一开始想让 `ninja -C build -n <目标>`
当第二证人，结果它把 `engine_selftest.exe` 判成要重编——而那枚二进制**确实含** P0 修复
（177/0 就是它跑出来的，`[35]` 延后压缩那两条断言是行为证人）。原因看 `-d explain`：
`src/core/download_core.c` 的 mtime 是 **08:21:31**，比 exe 的 08:12:58 晚 8 分钟，
可 `git status --porcelain -- <同一批输入>` 是**空的** ⇒ 内容与 HEAD 逐字相同，只是**被碰过**。
⇒ ninja 的 mtime 规则分不清"变了"和"被碰过"，拿它定罪会把好二进制喊错。
所以定罪权只留给：内容是否等于 HEAD（`git status`）+ 提交时间差（带容差 `GRACE_SECONDS=3600`，**规则数**），
`ninja` 降为**提示**并在行内写明为什么不一致。容差的代价也写明：构建后 1 小时内提交的真实改动会被放过，
这时靠 `DIRTY_INPUT` 与 `MISSING` 两条兜住，且这道门本来就不该被当成"内容相同"的证明——
**要证明"产品 exe == 当前源码"只有一条路：重编产品目标**，本轮边界禁止。

③ 时间证人为什么不用 HEAD 的提交时间：那样每一笔只改文档的提交都会让门永远红
（本轮 HEAD 之后就有三笔文档提交），而它报的根本不是"二进制过期"。所以比较量是
**该产物输入集合里最新一次提交的 committer 时间**，且每个产物一套输入
（`idm-next-host` 只看 `src/host + CMakeLists.txt`，不被 `src/core` 的改动冤枉）。
