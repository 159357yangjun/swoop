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
| `src/gui/legacy/glass_effect.h` | 未编译 | 2026-10-02 清掉了原先那条 include（曾写作 `main_window.h:10`）⇒ **现在没有任何编译 TU 走到它**，声明也不进编译；`Glass::` 在去注释后的全 `src/` 调用点 = 0。别把它读成"玻璃拟态还在用"。类别从「仅声明型死头」改成「未编译」是**代码变了**、不是我改了说法：由 `live_roster` 的 ROSTER-LIVENESS + ROSTER-ZOMBIE-STALE 两面盯着（清 include 却忘改表 ⇒ 红） |
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

<!-- PROBE-ROSTER-BEGIN 由 tools/probe_roster.py 复算；改探针必须同批改这张表，否则门红 -->
| 模式 | 类型 | 在 CI 闸门名单 | 断言条数（本轮实测） |
|---|---|---|---|
| IDM_ACTIVATE_PROBE | 断言型 | 是 | 7 |
| IDM_ARROW_PROBE | 人眼对照 | 否 | 0 |
| IDM_CONCURRENCY_PROBE | 断言型 | 是 | 1 |
| IDM_FILEICON_PROBE | 断言型 | 是 | 9 |
| IDM_FLUSH_PROBE | 断言型 | 是 | 2 |
| IDM_FORWARD_PROBE | 断言型 | 是 | 5 |
| IDM_ICON_PROBE | 人眼对照 | 否 | 0 |
| IDM_IPC_PROBE | 断言型 | 是 | 4 |
| IDM_NOTICE_PROBE | 断言型 | 是 | 5 |
| IDM_OFFTHREAD_PROBE | 断言型 | 是 | 4 |
| IDM_PROXY_PROBE | 断言型 | 是 | 5 |
| IDM_SCHEDULE_PROBE | 断言型 | 是 | 3 |
| IDM_SETTINGS_PROBE | 断言型 | 是 | 4 |
| IDM_SITEAUTH_PROBE | 断言型 | 是 | 5 |
| IDM_SIZECOL_PROBE | 断言型 | 是 | 5 |
| IDM_SIZEWIRING_PROBE | 断言型 | 是 | 4 |
| IDM_TEXT_PROBE | 断言型 | 是 | 1 |
| IDM_TOOLPROBE_PROBE | 断言型 | 是 | 4 |
| IDM_SIZELIVE_PROBE | 断言型 | 是 | 3 |
| IDM_KEYNAV_PROBE | 断言型 | 是 | 3 |
| IDM_ZOOM_PROBE | 断言型 | 是 | 4 |
| IDM_PATHGUARD_PROBE | 待修型（刻意不进闸门名单） | 否 | 3 |
| IDM_HASHVERIFY_PROBE | 断言型 | 是 | 7 |
| IDM_CLIPBOARD_PROBE | 断言型 | 是 | 5 |
| IDM_DRAGDROP_PROBE | 断言型 | 是 | 5 |
<!-- PROBE-ROSTER-END -->

「待修型」这一类的存在理由：**红门不许进 CI 闸门名单**（否则 CI 永远红、真回归被淹没），
但它又必须出现在表里，否则 `probe_roster` 的 ROSTER-MISSING 会点名它——两边都要出声。
判据三条：① 类型只能是 断言型 / 人眼对照 / 待修型；② 断言型 ⇔ 必须在 CI 名单，
待修型与人眼对照 ⇒ 必须不在 CI 名单；③ 待修型条目**必须同批在 §7 登记"什么时候才转成断言型"**，
没有收口条件的待修型等于永久豁免。当前只有一条合法住户：`IDM_PATHGUARD_PROBE`
（G1/G2 因 A1 dir 面零清洗而必红，见 §7-63；A1 修法落地那轮把它改标断言型并进 CI）。
上表的三个集合恒等式（模式名 / 类型 / 是否在 CI 名单）由 `python tools/probe_roster.py` 现算比对，
不一致即 rc=1。**它当时是红的**：`IDM_SIZEWIRING_PROBE` 已进二进制与 CI，但文档一处登记都没有——
正是"只存在于当前代码恰好遵守、没人断言"的那类约定。

**这道门自己先红过（四个变异，逐条留档）**：`python tools/probe_roster.py`
| 变异 | 期望 | 实测 |
|---|---|---|
| A 删掉 `IDM_SIZEWIRING_PROBE` 那一行 | 红 | `[RED] ROSTER-MISSING IDM_SIZEWIRING_PROBE`，违规=1，rc=1 |
| B 把 `IDM_ICON_PROBE`（人眼对照）改标成「断言型/在CI」 | 红 | 三条：`ROSTER-CI`＋`ROSTER-CI-FLAG`＋`ROSTER-KIND`，rc=1 |
| C 删掉一个旧模式行（ZOOM） | 红 | `[RED] ROSTER-MISSING IDM_ZOOM_PROBE`，rc=1 |
| D 整张表被 `<!-- -->` 注释掉 | **不许变绿** | rc=1（锚点之间解析不出数据行 ⇒ 前置不成立即红） |
复原后 `违规=0 rc=0`，且 `git diff --numstat docs/ARCHITECTURE.md` 与备份 `sha1sum` 双向对上
（两边同为 `8d1fe0b6ff0a…`），证明变异已逐字撤回。入口已接进 CI（`probe_roster gate`，排在 live-roster 之后）。
**它覆盖不到什么**：断言条数那一列不参与比对（每轮都在动，只要求是非负整数并印出来给人核）；
"某个模式该不该存在"仍是人定的，门只保证三处不打架。

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


### 2026-10-01 15:49→21:26（第 20 条）— 两个产品目标编出来了：门从 `rc=1` 变 `rc=0`，本轮 19 轮 0 红
主控授权重编（理由：他承认自己上一轮"重编要抢几分钟 CPU"是没量就下判断，而我的 `ninja -n` 读数说
`idm-next` 只要 7 步、`idm-next-host` 4 步 ⇒ 是增量）。全部细节在
`docs/probe-rerun-after-product-build-2026-10-01.txt`，这里只留承重：

- **命令**：`ninja -C build idm-next` / `ninja -C build idm-next-host`，33 秒，`rc=0×2`，
  没要求下载任何东西，没碰 packaging / installer / 发布清单 / 签名 / CI / 版本号 / CMake 配置。
- **新身份**：`idm-next.exe` `45bbbab03006`(09-30 20:04:54) → **`fe0f82754e16`**(15:49:23)；
  `idm-next-host.exe` `d1cb57613930`(09-29 03:46:29) → **`a908f656e862`**(15:49:32)。
  `engine_selftest.exe` 与 `ui_snapshot.exe` 本轮**未重编**（前者不在授权里且无需，后者见下）。
- **门的两面**（原文各存一份，不许只留最新那份）：
  `docs/freshness-gate-2026-10-01-1548-before-product-build.txt` ⇒ 不合格=2 **rc=1**；
  `docs/freshness-gate-2026-10-01-1549-after-product-build.txt` ⇒ 四条全 FRESH 不合格=0 **rc=0**。
  ⇒ 从今天起"产品 exe == 当前源码"这句话第一次有证人（内容等于 HEAD + mtime 晚于输入集合最新提交）。
- **③ 那条令里有一处我按边界顶回去了**：令要求探针"必须是本轮产出的那枚 exe，不许沿用 `944123ae728b`"，
  但 ① 只授权两个产品目标 ⇒ 我没编探针。可用的证人是 `ninja -C build -n ui_snapshot` ⇒ `no work to do`，
  而第 19 条刚实测过 ninja 的 mtime 规则只会**多报**过期（被碰过就喊重编）⇒ "不干活"在"新鲜"这个方向可信；
  加上 `ui_snapshot` 静态链 `idm_app_lib`（`CMakeLists.txt:233`，含 `src/core/download_core.c`）
  ⇒ 这枚探针本来就带着 P0 修复。我试过用符号名 grep 验（`pending_detach` 三枚全 =0）⇒ **方法瞎，弃用不作证据**。
- **④ 分布（n=19，逐轮从日志取）**：`worstEventLoopTurnMs` 211/216/220（振幅 9ms）、
  `idleBehindReplyMs` 212/217/220（振幅 8ms）、新读数自己窗内最长轮次 216/219/223（振幅 7ms）。
  **本轮 19 轮 0 红**；与上一轮 n=4 的分布**相同**，照实报相同。
  两个"0 红 vs 1 红"的对照**不能归因给修复**——那是两枚不同 exe（旧的 `e9ebbba101e2` 24 轮 1 红，
  拆名后的 19 轮 0 红），本轮只能说"没复现"。要看 P0 对 GUI 路径的真实效果，需要拿**修复前**的 `src/core`
  编一枚探针做受控消融（一次重编 + 一次 checkout），本轮既不授权也不该顺手做。
- **一条我自己踩到的计时坑（记下来防重犯）**：这 8 轮日志 mtime 从 `15:53:24` 连到 `15:54:02`，
  然后跳到 `21:25:45`——不是探针慢，是那条命令超过 10 分钟被挪到后台后**被挂起**了。
  ⇒ 报"多久"必须当场取 `date` 并用日志 mtime 排序核对；单轮真实耗时 ≈ 9–11 秒。
  另：这 8 轮没带 `QT_LOGGING_TO_CONSOLE=1`（sweep 带），日志比 sweep 少 INFO 行（19 vs 132），
  断言面齐全（`PASS=7 FAIL=0`）；有两轮 18 行，差的也是一行 INFO。


### 2026-10-01 21:36（第 21 条）— 新编的产品 exe **真的跑了一次**：起得来、在监听、单实例交接成功
第 20 条末尾我留了一句"没运行过产品 exe，要你点头才做"——那是把能自己做的验证交回给人。
本轮自己去做了，可复算的入口是仓库里的 **`tools/idm_next_smoke.ps1`**（不是我的临时脚本），
逐字输出与解码后的 stderr 存在 **`docs/product-smoke-2026-10-01.md`**。

被测：`build/idm-next.exe`，`exe_sha12=fe0f82754e16`，mtime 15:49:23，size 2124565（就是第 20 条那枚**新**二进制）。

四条承重结论，每条指到具体一行：
1. **能起**：`INSTANCE1 pid=7648 alive_after_5s=True`，Qt 日志一路走到 `[INFO] "主窗口初始化完成"`。
2. **在监听**：`[INFO] "IPC server 已启动，监听 idm-next-ipc"`。
3. **单实例交接成功**：第二个实例自己退了（`exited_within_15000ms=True`），而第一个实例的 stderr 里出现
   `[INFO] "IPC 请求: activate → 成功（排队 0ms，处理 16ms，送达=1，回话时客户端=在）"`
   ——这正是 `ci.yml` 单实例那段注释里写的"两句都有 = 交接成功"的分诊形状，**本机第一次拿到**。
4. **数据隔离有效**：`[INFO] "HistoryStore 已打开: C:\Users\yyyy\AppData\Local\Temp\idm-smoke-1001-213619\history.db"`
   ⇒ 依 `src/utils/app_paths.cpp:14-19`，设了 `IDM_DATA_DIR` 就不碰注册表与 %LOCALAPPDATA%。
   隔离目录由脚本**先断言不存在**再创建，收尾删自己造的 `history.db` 与 aria2 拷贝。

顺带否掉一个我自己差点误报的点：隔离目录里冒出 5,649,408 字节的 `aria2\aria2c.exe`，看着像"运行时下载了东西"。
比哈希即可证伪：它与用户缓存 `C:\Users\yyyy\idm_shots\_data\aria2\aria2c.exe` 的 sha256 前 16 位
**完全相同（`be2099c214f63a3c`）**，且用户那份 mtime 仍是 2023-11-15（我没动他的缓存）
⇒ 是从本地缓存**拷进**数据目录，不是网络来的。

三条产品侧观察（只记不修）：
· `QSystemTrayIcon::setVisible: No Icon set` ⇒ 托盘项是在没有图标的情况下 setVisible 的，
  Windows 上表现为一个空白/不可见条目。出现位置在 `已加载主题: light` 之后、`主窗口初始化完成` 之前。
· `QT_LOGGING_TO_CONSOLE is deprecated, use QT_ASSUME_STDERR_HAS_CONSOLE and/or QT_FORCE_STDERR_LOGGING`
  ⇒ `ci.yml` 和这份脚本都靠它把 Qt 日志引到 stderr；今天仍生效（否则 `instance1_stderr_bytes=0`），
  后继开关记在这里，等它真失效时不用重新考古。
· `exitcode=(unreadable)`：PS 5.1 从 `Start-Process -PassThru` 读不到已退进程的 ExitCode。
  脚本把空值显式印成 `(unreadable)` 并加一句 `blank or unreadable is NOT the same as 0`；
  交接是否成立由第 3 条那行 activate 作保，不由一个数字作保。

这份脚本自己身上踩的两个坑（都修在脚本里）：
① **PowerShell 5.1 把无 BOM 的 UTF-8 当 ANSI 读**——第一版脚本含中文提示，解析在第 30 行炸；
   第二版我把中文留在字符串里，导致 `INSTANCE2` 那行从我的 grep 结果里**静默消失**。
   ⇒ 现在文件头写明 ASCII-only，且逐字符核过：非 ASCII 计数 **0**。
② **一次采样不能当结论**——第一次跑印了 `CLEANUP_WARN pid=20860 still alive`，
   复核 `tasklist //FI "PID eq 20860"` 与 `IMAGENAME eq idm-next.exe` 都是"没有匹配的任务"
   ⇒ 是 1 秒采样太紧。现在轮询到 10 秒印最终状态：`CLEANUP pid=7648 terminated_within_1s=True`、
   `CLEANUP pid=29276 terminated_within_0s=True`。杀进程的名单只有本脚本 `Start-Process` 记下的两个 PID，
   别的会话的进程一个都不碰。

门的状态没被这件事污染：`.ps1` **在** `lint_control_bytes` 的 `TEXT_EXT` 里（实测 True），
所以这份新脚本进门被扫；`.sh` 仍然不在（盲区维持原状，已在 ENGINE-NOTES 挂账）。


### 2026-10-01 22:0x（第 22 条）— 应用图标补上、`.sh` 盲区关掉，并且**真的用它下了一条**
上一条末尾我把"要不要跑产品 exe"交回给人，被驳回。这一条是做完的样子。

**① 托盘/任务栏图标：根因不是一个调用，是两个都缺**
冒烟里那句 `QSystemTrayIcon::setVisible: No Icon set` 追下去：
`src/app/tray_controller.cpp:25-26` 写的是 `QIcon::fromTheme("download", QApplication::windowIcon())`
——Windows 没有图标主题 ⇒ 第一个候选空；而 `git grep setWindowIcon -- src` **零命中** ⇒ 兜底也是空。
⇒ 新增唯一真源 `resources/icons/app.svg`（128 viewBox、圆角方底 `#2563eb`、白色下箭头 + 底线；
配色与 `QToolButton#primaryToolBtn` 同值，形状语言沿用 `light.qss` 顶部那句"中性底 + 单一蓝强调 + 小圆角 + 无渐变"），
`resources.qrc` 登记，`src/main.cpp` 在 `QApplication` 构造后调 `setWindowIcon(QIcon(":/icons/app.svg"))`
⇒ 任务栏与托盘兜底同时有了。截图里主窗口和「新建下载任务」对话框的标题栏都是这枚蓝底白箭头。

**② 这条修复由探针自己发现，不靠人眼**
`IDM_FILEICON_PROBE` 加第 9 条断言：`:/icons/app.svg` 必须 `!isNull && !pixmap.isNull && 16px 墨点>0`，并加进该模式退码。
实测 `[fileicon] 应用图标 :/icons/app.svg isNull=0 naturalSize=64x64 16px 墨点=228` ⇒ PASS；sweep 从 62 涨到 **63 条全过**。
**会红演示**（只改资源名一处 `app.svg`→`app.TYPO.svg`）：
```
[fileicon] 应用图标 :/icons/app.svg isNull=1 naturalSize=-1x-1 16px 墨点=0
[FAIL] 应用级窗口图标非空且 16px 画得出墨（任务栏与托盘的唯一真源）      模式退码=1
```
还原后 `git hash-object tools/ui_snapshot.cpp` 与改前记录的 `8d7e558cafdae2dd2b377782080c66e6ad11ed86` **逐字相等**，重编后回绿。
顺带被编译器教了一条 Qt 6 事实：`QIcon::available()` 在这个版本**已不存在**
（`error: 'const class QIcon' has no member named 'available'`）⇒ 换成 `actualSize(QSize(64,64))`。

**③ `.sh` 盲区关掉**：`TEXT_EXT` 补 `.sh`（带理由注释）。分母 277 → **296**；
`--selftest` 两面 + "真实分母非空(296)" 全过；全仓 **0 处裸控制字节**。
新被扫到的 5 个：`tools/ext_shot/run.sh`、`load_diff_run.sh`、`mutation_fixture_fileicon.sh`、`p0_rerun.sh`、`ui_probe_run.sh`。

**④ 真的用它下了一条（不是"起得来"而已）**：computer-use 点「新建任务」→ 填
`http://idmuser:idmpass@127.0.0.1:18080/small.bin` → 保存目录改到临时目录 → 「确定」
→ 表格出现 `small.bin / 已完成 / 2.66 MB/s / 127.0.0.1`，侧栏 分类 1·其他 1·已完成 1，状态栏 `任务: 1 下载中: 0`。
应用自己的日志（第一手，不是我推的）：
```
[INFO] "添加任务 #1 (队列:无): http://idmuser:idmpass@127.0.0.1:18080/small.bin"
[INFO] "任务 1 完成"
```
盘上核账：`size=1048576`、与服务器载荷 `bytes(i%251)` **逐字节全等**、
`sha256` 前 16 位 下载件 `631b84027d6b9e52` == 独立重算的期望件 `631b84027d6b9e52`（两套实现，不共用同一次判断）；
服务端 `/_stats`：`auth_ok=10 ranges=9 max_cur=8` ⇒ **8 线程并发 Range 真发生了**，URL 里的 Basic 凭据被 GUI 这条路带上。
⇒ P0 修的那类"大小对、内容错、状态还显示已完成"，在真实用户路径上第一次端到端核过。

**⑤ 三条只有真操作才撞得见的事实**
1. **这台机器的键鼠输入不独占**：我启动应用后、还没操作前，它 stderr 里已经有
   `视图缩小 12pt → 11pt → 视图放大 → 视图缩放重置` 四行（不是我发的——四路会话共用一个焦点窗口）；
   我 `type_text` 填保存目录时 **`idm-gui-215114` 被吞成 `idm--ui--15114`**。
   ⇒ 规矩：**UIA 写入必须回读**（`set_value` 的 `verification:"matched" + verifiedValue` 就是干这个的）；
   "我输入了 X"不等于"框里是 X"。
2. **`大小` 列在已完成任务上显示 `-`**（截图与 UIA 树一致，而文件实际 1,048,576 字节）
   ⇒ 这是 P2（进度字段对已完整落盘的下载不报数）**第一次在真实 UI 上被看见**，不再是从引擎读数推的。
3. **UIA 的组节点会吃掉点击**：`QDialogButtonBox` 是元素 11、`确定` 是 12；点 11 返回 `succeeded` 但对话框不关。
   ⇒ "调用成功"不等于"点中了要点的控件"，必须带 `post_action_state` 看后置状态。

**⑥ 我自己的一处数字失实（当场订正）**：归档初稿把下载件的 sha 写成 `be2099c214f63a3c`——
那是上一节 **aria2c.exe 拷贝**的哈希，被我错抄成下载件的。真值 `631b84027d6b9e52`，订正连同来历写在
`docs/product-smoke-2026-10-01.md` 里。

**收尾**：我起的服务器走它自己的 `/_shutdown`（`{"ok":1,"bye":1}`，端口监听数回 0）、
我起的 app 实例 `taskkill`（`GUI_PID=23968`，全程只有这一个实例，现 `tasklist` 查无）——
**单实例门闩必须还给用户**，否则他自己打开 IDM Next 会交接到我这个用临时数据目录的实例上。
取证件留在 `%TEMP%\idm-gui-215114\`。本轮重编：`idm-next.exe` `91de8e79e7ac`、`ui_snapshot.exe` `379411249120`；
`engine_selftest.exe` 未动（`8af9e43168de`，它的输入集合不含本轮改的这些文件，门也据此判它 FRESH）。


### 2026-10-01 22:4x（第 23 条）— 「大小」列的 "-" 查出**两个**缺陷：修了一个，另一个只登记不修
第 22 条里我记了"已完成任务的大小列显示 `-`"。真去修的时候发现它是**两个独立缺陷叠在一起**，
而且**我修的那个不是用户看到的那个**——这件事必须原样写下来。

**D1（已修 + 已被探针钉住）：模型没有任何接口能回填文件大小**
- `TaskListModel` 只有 `updateProgress(id, downloaded, speedBps)` 与 `setProgressPct`，**没有一处能改 `row.fileSize`**；
- 更糟的是 `updateProgress` 的 `dataChanged` 只覆盖**第 2..5 列**（`task_list_model.cpp:219`）⇒ 第 1 列连重绘机会都没有。
- 修：新增 `TaskListModel::setFileSize(id, bytes)`（值未变直接 return ⇒ 4Hz 不造成重绘风暴；
  发了就覆盖**第 1..3 列**：大小 / 状态里的百分比 / 剩余时间），
  `main_window.cpp` 的 `refreshFromEngine()` 里每 tick 在 `info.file_size > 0` 时回填一次。
- 钉它的是新探针模式 **`IDM_SIZECOL_PROBE`**（5 条断言，实测全绿）：
```
[PASS] 建行时大小未知(-1)，第 1 列显示占位符（实测「-」）
[PASS] setFileSize 之后第 1 列变成「1.0 MB」（实测「1.0 MB」）
[PASS] 回填确实发了 dataChanged 且覆盖第 1 列（判定时刻次数=1 列 1..3）
[PASS] 同值再回填不发重绘（4Hz 循环里不造成重绘风暴）
[PASS] 状态列百分比跟着回填出现（前「下载中」后「下载中 (50%)」）
```
  **牙演示**：把 `setFileSize` 改成空操作（`if (true) return;`）⇒ 后 4 条 `[FAIL]`、模式退码 1；
  还原后 `git hash-object src/model/task_list_model.cpp = af66f8a15a39108e35f0944f98b664d669137d0b` 与改前逐字相等。
  第 5 条特意把 `progressPct` 设成 -1：模型优先用后端上报的百分比，不设就会把这条测成**假绿**。
  新探针同时加进了 `ci.yml` 的 `PROBES` 名单（本机 `ui_probe_run.sh` 是从二进制里 `strings` 枚举的，自动就带上它）。

**D2（查到、登记、本轮不修）：引擎在完成时根本不知道大小，于是把 -1 一路写进历史**
真机复验（新二进制 `idm-next.exe 4a1503599f0a`，跑 `small.bin` 1MiB）：
- 界面上「大小」列**仍然是 `-`**（截图核对），文件确实 1,048,576 字节；
- 应用自己的 `history.db`：`('small.bin', -1, 'completed')` ⇒ **不是 GUI 丢的，是引擎给的 `file_size` 就是 -1**，
  而且这个 -1 被持久化进了历史记录。
> **订正（同日 23:2x，做第 24 条时写的；上面那句"是引擎给的 -1"是我在没有证人的情况下替产品作的保）**
> ① 那次跑的是 **D1 之前**的二进制——`TaskListModel::setFileSize` 是 22:52 的 `b04a18c` 才存在的函数，
>    所以当时「大小列 = `-`」只能证明 D1，证不了引擎。
> ② `/small.bin` 的 HEAD **是回 Content-Length 的**，引擎在那条路上给的正是 1,048,576，
>    本轮由 `[36②]` 的无条件读数行量到（`docs/d2d3-readings-2026-10-01-232215.txt`）。
> 引擎侧真有 -1 的那条路是**服务器不声明长度**（分块传输 / 动态端点 / HEAD 被限流后放弃），
> 与产品那次观测不是同一个缺陷；它由第 24 条按 TDD 修掉。
⇒ 所以 D1 的修在真实链路上**暂时是惰性的**（它只在引擎报正数时起作用）。我不拿"D1 探针全绿"冒充"用户的问题修好了"。
> **订正（同上）**：这条推论跟着上面那句一起错——D1 不是惰性的，它正是产品那个 `-` 的修复；
> 引擎侧的 -1 是**另一条**只在"服务器不声明长度"时才走到的路（第 24 条）。
> 保留原句不删，是因为"不拿探针全绿冒充用户问题修好"这个纪律本身仍然要留着，只是这次它指错了缺陷。
D2 的候选形状（写清楚，等下一轮按 TDD 做）：完成路径（`download_core.c:1333` 置 `TASK_COMPLETED` 处）
在 `t->file_size <= 0` 时用**已经存在的** `utf8_file_size(final_path)` 回填，再写历史；
但这条改的是**数据落盘路径的记账**，必须先有引擎自测的失败断言（例如"完成任务的 `TaskInfo.file_size`
必须等于盘上文件长度，即使探测阶段没拿到 Content-Length"），看着它红，再改，再复跑 177 那条门。
本轮剩余余量不足以安全走完"红→改→重编→全量复跑→归档"，**所以停在这里，不半途改 `src/core`**。

**顺带两条实测教训（都是操作层面的）**
1. 用 `Start-Process` 起 GUI 时**必须把 stdout 和 stderr 都重定向到文件**：只重定向 stderr 的那版启动脚本，
   子进程继承了控制台工具的 stdout 管道 ⇒ 调用方一直等不到 EOF，命令被挪到后台（本轮撞到两次）。
2. 用 URL 参数启动会走**默认保存目录**（`C:/Users/yyyy/Downloads`），`IDM_DATA_DIR` 只隔离数据/配置不隔离落盘位置
   ⇒ 我造的两个测试文件（`big.bin` 12,582,912B、`small.bin` 1,048,576B，mtime 22:33/22:44 都核对过是自己的）
   已删除；实例已 `taskkill`，服务器 `/_shutdown` 后端口监听数回 0。

### 2026-10-01 23:07→23:2x（第 24 条）— 按 TDD 修掉 D2，**自己的判别式顺手炸出第三个缺陷 D3**
上一轮把 D2 登记成"下一轮按 TDD 做"。这一轮做了，而且做出来的是**两个**修复：
D2 是计划里的那条，D3 是给 D2 配的判别式自己炸出来的——不在计划里，也没提前问过。

**先红（逐字归档，三份都在仓里）**
| 轮 | 引擎 exe（sha256-12） | 结果 | 红的那一条 |
|---|---|---|---|
| 加 `[36①②]`、未改 `src/core` | `fcbf63b20904` | `178 通过，1 失败` | `[36①]`：`file_size=-1 downloaded=200000 盘上=200000` |
| D2 回填之后 | `74268bf0ea47` | `179 通过，0 失败` | — |
| 再加 `[36③]`（重启面） | `8237381a31cc` | `179 通过，1 失败` | `[36③]`：`status=2 clamp_chunks=1 clamp_refetch=200000` |
| D3 修好之后 | `07e1e7bc3f83` | `180 通过，0 失败` | — |
| 补无条件读数行 | `839555d2c8ad` | `180 通过，0 失败`（独立复跑第二次） | — |

原文：`docs/d2-selftest-red-2026-10-01-230845.txt`、`docs/d3-restart-clamp-red-2026-10-01-231434.txt`、
`docs/d2d3-selftest-green-2026-10-01-231951.txt`、`docs/d2d3-readings-2026-10-01-232215.txt`、
`docs/d2d3-ui-probe-18modes-2026-10-01-231845.txt`。

**D2 的修法**：完成处 `download_core.c`（原 `:1333` 置 `TASK_COMPLETED` 那几行）在 `utf8_move` 成功之后、
写状态之前，`if (t->file_size <= 0) t->file_size = utf8_file_size(final_path);`。
为什么必须在这个位置：`final_path` 在改名成功之前不存在，盘上长度只有这里读得到。
为什么条件是 `<= 0` 而不是无条件覆盖：「服务器说多少」与「盘上有多少」是两个事实，
同值时两种写法观测不可区分，所以 `[36②]` 对这个变异**不占功**（写进注释，不许以后有人把它记成牙）。

**D3（新缺陷，比 D2 更疼）**：`verify_progress_against_disk()` 里
`span = (c->end >= c->start) ? (c->end - c->start + 1) : 0` —— 长度未知的单流片写的是 `{start:0, end:-1}`，
于是 `span=0`，紧跟的 `if (avail > span) avail = span;` 把盘上可用字节夹成 0，
等于宣称"这片一个字节都不该在盘上"。**只要 `c->downloaded > 0` 就恒判"被盘否证"**。
用户看到的：完整下好的任务，重启之后「已完成」变「暂停」，然后**整份文件重下一遍**。
这个表达式里没有 `file_size` ⇒ **它是先于 D2 存在的**，不是我的回填造成的；本轮只是第一次有人量到它。
修法：给未知片单独一条——`unknown` 时不夹 `avail`，只按 `c->downloaded` 与 `backing` 比
（盘上少了才重取，够了不动，所以 [32] 面B/C 那两条真牙仍然非零：`clamp_refetch=9433088B / 9437184B`，`3 片`）。

**两条测具自身的错，都当场认了**
1. `lc36 >= 1`：我按"load 返回恢复条数"写的，实读 `download_core.c:2225` 成功返回 **0**（条数走
   `dlmgr_get_task_infos`）。这是永红夹具的形状。**这一条红不靠它成立**——同一次输出里
   `status=2 / clamp_chunks=1 / clamp_refetch=200000` 三个产品读数独立指向 D3，所以红仍可归因产品，
   归档文件里把这一点写死了。
2. 绿侧没有数字：`check()` 只在**失败**时把 detail 带到盘上，跑绿就只剩一行 `[PASS]`，
   于是"引擎到底给了 1048576 还是 -1"在绿侧零证据。同一坑我在 `p0_rerun.sh` 的 `error_msg` 那行踩过一次，
   这次是**门自己的断言**栽在同一件事上 ⇒ 给 `[36]` 三条都补了无条件 `printf("    [36 读数] %s\n", d36)`。
   补完之后第 23 条那句"是引擎给的 -1"才有了反证（`[36②] 读数: file_size=1048576`），订正也由此落笔。

**产物身份（本轮把四个目标都重编了，所以"产品 exe == 当前源码"这句话仍然有证人）**
`sha1sum` 前 12 位（与 `p0_rerun.sh` 打印的 sha256-12 是两种函数，别混用）：
`idm-next.exe 13dd6f27e562→7aeb3de9da4e`、`idm-next-host.exe 296da6ffa557→d7e1fe10b658`、
`ui_snapshot.exe b5b8b9fec732→fe6a2eed8af1`（探针跑器对同一文件印的是 sha256-12=`2e7c06366e2d`）。
编译过程中没有任何下载要求（`ninja` 只动了 `download_core.c` / `engine_selftest.c` 两个 obj 和链接）。
新 `ui_snapshot.exe` 复跑 18 模式：`PASS=68 FAIL=0 零断言模式=0`，含 `IDM_ACTIVATE_PROBE 7/0`、
`IDM_SIZECOL_PROBE 5/0`（D1 那条探针在含 D2/D3 的 `src/core` 之上仍然绿）。

**仍然没验证的东西（不许读成"产品没问题"）**
- 产品 GUI 在 D2 之后**没有再拍过一次**：本轮"大小列有数"这句话的证据是引擎侧 `[36①]` + 模型侧
  `IDM_SIZECOL_PROBE`，两段拼起来不等于真机跑过那条链路。
- CI 从未执行过（87 笔未推），上面所有门都只有本机读数。
- D3 只在"长度未知的单流片"这一形状上验过；`end=-1` 还有没有别的来源（比如 state 文件被手工改坏）
  没有穷举。
- `[36②]` 对"无条件覆盖"这个变异不占功（上面写死），所以"声明值不会被盘上数字换掉"这句话
  **没有被任何断言证明过**，只是代码里写着 `<= 0`。
- `idleBehindReplyMs` 仍不进判据（n=19，幅度 8ms）；3263ms 那个事件仍未复现，`[attr]` 开/关/开三面从没同时拿到过。

### 2026-10-01 23:27→23:3x（第 25 条）— 产品 GUI 拍回来了：**D2 修完，用户看到的「大小」列仍然是 `-`**
第 24 条末尾我把"产品 GUI 没再拍过"列进未验证清单。这一条就是去拍的，结果是**它仍然是坏的**。

**读数（一手，逐字进仓：`docs/d4-product-gui-2026-10-01-233500.txt`）**
被测 `build/idm-next.exe`（sha256-12=`1aa399a39b41`，mtime 23:18:28，含 D2+D3），URL 用自测服务端的
`/cut.bin`（HEAD 刻意不回 Content-Length ⇒ 正是 D2 那条路）：
- 盘上成品 `dl\cut.bin = 200000` 字节，服务端 `auth_ok=3 cut_range=1` ⇒ 下载真的走完了；
- 引擎侧同一个夹具在自测里给的是 `file_size=200000`（`[36①]` 读数行）⇒ **D2 本身是生效的**；
- UIA 树里那一行：`19 cut.bin / 20 "-" / 21 已完成 / 23 192.2 KB/s`（标头 10..18 依次是
  文件名·大小·状态·…⇒ 节点 20 就是「大小」列）——**列上还是 `-`**；
- 应用自己的 `history.db`：`(1, 'cut.bin', -1, 'completed')`。
⇒ 「引擎知道长度」与「界面显示长度」之间断了一环，第 24 条说"D2 修完"没有覆盖到用户看见的那一层。

**断在哪（实读三处，不是推测）**
1. `MainWindow::refreshFromEngine()` **没有挂在任何定时器上**：全仓只有 3 个调用点——启动时
   `loadState` 之后（`main_window.cpp:414`）、`onStartAll`（`:1485`）、`onPauseAll`（`:1492`）。
   250ms 的 `m_statusTimer` 只连到 `updateStatusBar()`（`:380-382`），而 `updateStatusBar` 的函数体
   里不碰引擎快照（`:1539-1564`）。
2. 实时进度回调 `MainWindow::onTaskProgress(taskId, downloaded, total, speedBps)`（`:1286`）**收到了
   `total` 却把它丢掉**：函数体只调 `updateProgress(taskId, downloaded, speedBps)`（`:1298`），
   没有任何一条路径把 `total` 交给模型。
3. ⇒ 第 23 条里我写的"这里每 tick 回填一次"（`refreshFromEngine` 内 `setFileSize` 上方的注释）
   **是错的**：那行代码只有在载入/全部开始/全部暂停时才跑。D1 加的 setter 是真的，
   但它等的"每 tick"根本不存在。这条注释要跟着改，否则下一个读它的人会继续以为有证人。

**D4 的候选修法（本轮登记不修，按 TDD 走）**
- ① `onTaskProgress` 里把已经收到的 `total` 用掉：`if (total > 0) m_taskModel->setFileSize(taskId, total);`
  ——零新数据源，用的就是引擎已经在推的那个数。
- ② 但①对"长度未知"的任务仍然无效（下载途中 `total` 恒为 -1），所以完成时还要**读一次终态快照**：
  `onTaskCompleted` 在 `recordFinished(url, name, size, success)` 之前 `dlmgr_get_task_info(taskId, &ti)`，
  用 `ti.file_size` 回填模型再取 `size`——否则历史库继续收 -1（今天就是这样）。
- 夹具形状：`onTaskProgress`/`onTaskCompleted` 是私有槽，仓里已有先例用
  `QMetaObject::invokeMethod(obj, "槽名", Qt::DirectConnection)` 走真实代码路径（`ui_snapshot.cpp:392`），
  不必为测试开后门；先红断言 = "给一个 total>0 的进度回调，第 1 列必须变成字节数"。

**一条未归因观测（明确不算产品缺陷）**
第二发 `/small.bin` 的任务名变成 `small.bin分任务执行（推荐）` 并失败。已查：字符串 `分任务执行`
在本仓源码 `grep -rn` 0 命中、在 `build/idm-next.exe` 里 0 命中 ⇒ **不是产品自己的文案**。
我的 `set_value` 每次都回报 `verification:"matched"`，但同一份回执里 `runtimeIdValidated:false`
⇒ 它是按**索引**而非运行时 id 解析的，而我后两次调用都带着**陈旧 snapshotRevision**。
首要嫌疑是操作侧索引错配，不是产品；下一步该做的干净复现是**用键盘输入代替 SetValue** 再跑一次。
在这之前这条不写进缺陷表。

**收摊核对**：`Stop-Process` 掉我自己那个实例（`idm_next_procs=0`）、`/_shutdown` 后
`18080` 上监听数回 0、临时运行目录（含 `dl\cut.bin` 与那个 0 字节 `.idmtmp`）已删、
`~/Downloads` 里没有被造出文件（保存目录全程指在临时目录）。

**仍然没验证的东西**
- D4 没修：产品界面上「大小」列与历史库的 size 在实时链路上仍是 -1（这条**推翻**了第 24 条
  提交信息里"D2 修完用户可见的 -"这个说法——那个说法超出了证据）。
- 「重启后大小列会不会出现数字」没测：这个实例被 `Stop-Process` 强杀，`tasks.json` 没落盘
  （`data/` 里只有 `aria2/` 与 `history.db`），所以载入面（`refreshFromEngine` 的那 1 个真调用点）
  本轮没有证人。
- 第二发任务的失败原因没查（只到"名字不对 ⇒ 可能 404/写盘失败"这一层，未取证）。
- CI 从未执行（88 笔未推）。

### 2026-10-02 00:0x（第 26 条）— D4 修完：接线补上，并且**量出这条探针哪一面没有牙**
第 25 条登记的 D4 落地了。先红原文 `docs/d4-sizewiring-red-2026-10-02-000046.txt`，
变异差分 `docs/d4-mutation-differential-2026-10-02.txt`。

**新探针 `IDM_SIZEWIRING_PROBE`（4 条断言）**——与 `IDM_SIZECOL_PROBE` 的分工要说清：
后者只测 `TaskListModel` 自己（模型有接口），前者测**有没有人调它**。这次坏就坏在"有接口没人调"，
所以必须有一面走真 `MainWindow`：私有槽按仓里先例用 `QMetaObject::invokeMethod` 按名调用，不开后门。

**改了两处接线（`src/gui/main_window.cpp`）**
1. `onTaskProgress` 用掉引擎已经在推的 `total`：`if (total > 0) m_taskModel->setFileSize(taskId, total);`。
   `> 0` 不是装饰：长度未知的任务 `total` 恒 -1，而 `formatBytes(<=0)` 印 `0 B` ⇒ 无守卫会把 `-` 换成 `0 B`，
   那是比 `-` 更响的谎。
2. `onTaskCompleted` 在 `updateStatus`/`recordFinished` 之前**现读一次** `dlmgr_get_task_info`：
   历史库那个 size 就是从这儿取的，不现读就继续收 -1。

**夹具本身踩了三个坑，都当场量出来了（这些是测具的错，不是产品的红）**
- `dlmgr_list()` 不查 `g_initialized`、`dlmgr_get_task_infos()` 查 ⇒ 没 init 时 `total=min(1,0)=0`，
  一行都不建。读数 `引擎条数=1 模型行数=0` 把它指了出来；已在 `refreshFromEngine` 上方写明这个不对称。
- 探针的数据目录按日期分层、**跨轮复用** ⇒ MainWindow 载入上一轮留下的队列，连跑三次得到
  `模型行数=1/7/7`。改成每轮新建空目录后三轮全一致。
- Qt6 默认 `DirectConnection`，槽在本线程内联执行 ⇒ 拿"最后一次调用的快照"当判据会读到上一次的计数；
  而且 `onTaskProgress` 自己还要发一发合法的 2..5，判据只能数**覆盖第 1 列**的那一发。

**三面变异（同一枚夹具，只切 `main_window.cpp`）**
| 切掉什么 | 结果 | 说明 |
|---|---|---|
| 进度面 `setFileSize(total)` | 3 条 `[FAIL]`，退码 1 | 牙在这里 |
| 完成面"现读引擎"那一发 | **退码 0，不红** | ⚠️ 这一面**没有被证明** |
| 只切进度面、留完成面 | 2 条进度面 `[FAIL]` | 两面不能互相冒充 |

第二行为什么不红：夹具在建行时已经走过一次 `refreshFromEngine`，进度面把 200000 写进模型之后，
完成面的 `setFileSize` 撞上自己的"同值不发重绘"守卫直接 return ⇒ 它在 offscreen 里没有独立可观测效果。
要让它有牙必须造"全程 `total=-1`、完成那一刻才有长度"的真链路（真服务器 + 真下载）。
**所以这一发的正确性目前只有代码为凭**，写进未验证清单，不许记成"D4 已被四条断言钉住"。

**复跑**：探针套件现在 **19 模式 / PASS=72 FAIL=0 零断言=0**（68→72 就是这 4 条）；
引擎门 `180 通过，0 失败`；`lint_layering`/`live_roster`/`lint_control_bytes`/`notice_verify` 四道 rc=0；
四个目标全部重编（`idm-next`、`idm-next-host`、`engine_selftest`、`ui_snapshot`）。

**仍然没验证的东西**
- 产品 GUI 在 D4 之后**没有再拍过一次**：本轮 UIA 通道坏了（`GetWindowTextW` 对同一 hwnd 返回空串，
  MCP 调用超时），只做到"进程起得来、窗口标题在、`Responding=True`"。⇒「大小列现在真的有数」这句话
  的证据仍然是**探针级**，不是像素级。
- 上一轮那条未归因的文件名掺入仍未做干净复现（键盘输入路线被同一个 UIA 故障挡住）。
- 完成面那一发的牙（见上表第二行）。
- CI 从未执行（90 笔未推）。

### 2026-10-02 00:47→00:5x（第 27 条）— D4 的像素级核对**没做成**，失败原因逐条落到盘上
上一节说"D4 修完，但产品 GUI 没再拍过"。这一节就是去拍的——**没拍到**。写清楚为什么，
免得下一轮又用同一套坏量具去试一遍。

**先纠正我自己两个错判**
1. 我一度判定"屏幕抓取是左右镜像的"（任务栏文字反、`English Teach` 反）。那是**我的行序 bug**：
   `BITMAPINFOHEADER.biHeight` 给负数时 GetDIBits 已经返回 top-down，我又按 bottom-up 倒了一遍。
   改成正确行序后画面是正的。**结论：抓取本身没问题，是我的读数错。**
2. 我据此又判定"SendInput 被系统丢弃"。也不成立——`AttachThreadInput` 之后 Tab/Enter **确实生效**
   （焦点环移动、对话框关闭、目录选择器弹出都能从截图里看到）。真正的原因见下。

**这台机器上驱动这个 GUI 的三条通道现在的状态**
| 通道 | 现象 | 定性 |
|---|---|---|
| computer-use MCP | `get_window_state` 超时；UIA 枚举子窗口返回空 | 不可用（本轮复测仍不通） |
| `PrintWindow` / `WM_PRINT` | 对本进程的 Qt 窗口返回空白（有色像素 0.0%） | 不可用（Qt 走自己的合成后端） |
| Win32 输入 + BitBlt 抓屏 | **可用**：能开对话框、能粘贴、能提交、能读回像素 | 可用，但被下一条挡住 |

**挡住的到底是什么**：主窗口停在 **952×278**（正常应是 1193×756），`SW_SHOWMAXIMIZED`、
`SW_RESTORE`、`SetForegroundWindow` 全部改不动它，`IsIconic/IsZoomed` 都是 False。
表格与侧栏因此不在可见区里 →「大小」列根本没进过任何一帧像素。
另外 `Ctrl+N` 不触发（菜单项明明绑了 `QKeySequence::New`），走 Alt+F→N 才开得起来；
以及一个真缺陷候选：**Tab 到「保存目录」那一行会直接弹出目录选择器**（焦点环似乎把只读按钮
和输入框算成了同一档），这会让纯键盘提交多一层模态框。

**做过的尝试（都留了产物或读数，不是"我试过了"）**：Alt+F/N 开窗 ✓、剪贴板 Ctrl+V 填 URL ✓
（截图可证：`http://idmuser:id…` 已落在「下载链接」格）、Tab 换焦 ✓、Enter 提交（弹出了校验提示
⇒ 说明提交路径真的在跑）✓、清字段重填 ✓。缺的就是最后那一步：**看见那一列**。

**所以现在的证据状态要说准**：D4 的正确性有**探针级**证人（`IDM_SIZEWIRING_PROBE` 4 条 + 变异差分），
**没有像素级证人**。这条挂账，不许在读表时被当成"已验"。

**收摊核对**：`dl/` 里没有下载产物（提交从未成功）、临时运行目录已删、
`18080` 监听数回 0、`idm_next_procs=0`、探针残留目录 0 个、工作树只剩约定保留的 `idm-next-glass-ui.html`。

### 2026-10-02 04:2x→04:5x（第 28 条）— 像素核对**第二次失败**，原因换成了一条能证的；顺带把文件名掺入那条查到"不是产品"
**做了什么**：新加 `tools/probe_roster.py`（探针名单门，见 §1.3 末尾的表与四个变异），入口接进 CI 第 3 步。
它红过四种形状（缺行 / 类型标错 / 旧模式行被删 / 整表被注释掉），复原后 rc=0、备份与复原 sha1sum 同为 `8d1fe0b6ff0a…`。

**像素核对为什么又没做成（这次有读数，不是"手气不好"）**
第 27 条把障碍写成"主窗口卡在 952×278"。**那个诊断错了**：本轮全新实例的窗口是 **1193×756（正常）**，
`SW_MAXIMIZE` 也改得动。真正的障碍是这台机器的**前台焦点我抢不到**：
| 采样 | 结果 |
|---|---|
| 连采 6 次 `GetForegroundWindow()` | 全部 = `Qoder CN`（pid 13016），间隔 0.35s |
| `AttachThreadInput` + `SetForegroundWindow(我的窗口)` 之后 0.4s / 1.5s | 前台仍是 `Qoder CN` |
| 期间一次误注入落点 | `fg=0x220664` —— **不是本进程的窗口** |

⇒ 键盘输入会打到别的程序上。**我不在别人（含我自己另一个会话）的窗口里打字**：那既是破坏性动作，
拿到的"产品读数"也不成立。所以这条路的正确定性是：**量具到不了**，不是"产品有问题"也不是"我再试一次就好"。
可复算的替代通道已经用掉了两条并都判为不可用：UIA/computer-use MCP（超时、枚举空）、
`PrintWindow`/`WM_PRINT`（对本进程 Qt 窗口有色像素 0.0%）。第三条 BitBlt 抓屏本身是好的
（同区域连抓三次 colored≈48% 稳定），坏在它抓之前必须先把目标窗口带到最前。
**下一轮别再重复试这三条**；要拿像素级证人只能换路径：① 用户自己在那个 GUI 窗口里操作一次；
② 或给探针加一个"真服务器 + 真下载"的 offscreen 面（不依赖桌面焦点，能同时补上完成面那颗缺失的牙）。

**上一轮那条未归因的文件名掺入：查完了，不是产品**
`分任务执行（推荐）` 这串字符在 UTF-8 字节层面扫了工作树 8690 个文件（避开 `strings` 对中文瞎的坑）：
命中 2 个，**两个都是我自己写的文档**（`docs/ARCHITECTURE.md`、`docs/d4-product-gui-…txt`）；
`build/idm-next.exe` 里 UTF-8 与 GBK 两种编码各 0 命中。
⇒ 结论与前一轮一致但更硬：**字符串不在交付物里**，它来自我那次 SetValue 之外的桌面文本源；
按规矩仍写"**未归因到具体注入方**"，只是现在可以确定它不可能由产品产生。原产物目录已删，无法再取证到进程级。

**收摊**：我的实例 `procs=0`、`18080` 监听数回 0、临时运行目录已删（`dl/` 全程为空 ⇒ 没有任何下载真的发生）、
探针残留目录 0、工作树只剩约定保留的 `idm-next-glass-ui.html`。

**仍然没验证的东西**：D4 的像素级证人（两轮均未拿到，原因如上）；完成面那一发的牙；
`Ctrl+N` 不触发与「Tab 到保存目录弹选择器」两条都只是登记，没有断言；CI 从未执行（93 笔未推）。

### 2026-10-02 09:0x→09:3x（第 29 条）— 完成面那颗牙**补上了**：新探针 IDM_SIZELIVE_PROBE 走真服务器 + 真下载
第 26 条末表第二行记的是"摘掉完成面那一发 ⇒ 退码 0，不红 ⇒ 该发未被证明"。这一条把它变成有牙的。

**为什么 offscreen 那面给不了牙**：它先调进度面把 200000 写进模型，完成面的 `setFileSize`
撞上自己的"同值不发重绘"守卫直接 return ⇒ 摘与不摘观测不可区分。**证人时刻只有一个**：
一条**全程 total 恒 -1** 的真下载——那时模型里的长度只能来自完成时的现读。这就是本面的形状：
自测服务端 `/cut.bin`（HEAD 不回 Content-Length、GET 只发 8000 字节再断流 ⇒ 必须 Range 续传补齐），
QProcess 起服务、Qt 自己试绑端口判空闲、`QTcpSocket` 打 `/_stats` 判就绪，然后建真 `MainWindow`、
`dlmgr_add`+`dlmgr_start` 跑完，再断言第 1 列。

**三条断言（本轮实测）**
```
[PASS] 真链路：任务确实下完（status=3；成品=200000 字节 引擎 file_size=200000）
[PASS] 长度未知的下载完成后，第 1 列显示真实大小（实测「195.3 KB」）
[PASS] 该回填发了覆盖第 1 列的 dataChanged（发数=1 列 1..3）——它的唯一来源是完成时现读
```
**变异（这次是真有牙）**：把完成面的 `if (dlmgr_get_task_info(...) && finTi.file_size > 0)` 改成 `false && …`
⇒ `[FAIL] 第 1 列显示真实大小（实测「-」）` + `[FAIL] …dataChanged（发数=0）`，退码 1；
还原后 rc=0，且 `src/gui/main_window.cpp` 与备份 sha1sum 同为 `eeda389850ce…`、`git diff --numstat` 只剩本轮新增。

**写这面时踩到的四个坑（都是测具的错，逐个当场量出来）**
1. **`bash -lc` 在这台机器上不是 bash**：命令不存在时它回一段 UTF-16/GBK 混排中文报错、退码 1 ⇒
   读数成乱码而不是零。端口占用改用 `QTcpServer::listen` 自己试绑，服务用 `QProcess` 直接起 python。
2. **就绪判据只读一次会恒假**：这个服务把响应分成 145 字节（状态行+头）与 291 字节（body）两个包
   （实测 `chunk_sizes=[145,291]`）⇒ `contains("auth_ok")` 永远拿不到。改成读到连接关闭。
3. **窗口必须先构造再加任务**：`MainWindow` 构造里 `loadState(...)`，而 `dlmgr_load_state` 开头
   `reset_all_tasks()` ⇒ 先 add 再建窗口，任务被自己的载入抹掉（成品字节=-1、模型行数=0）。
4. **"最后一发 dataChanged"不能当判据**：下载途中 `updateProgress` 合法地发 2..5，第一版就是这么红的。
   正确做法是把模型显式打回未知（`setFileSize(tid,-1)`）后再单独调那个私有槽，数这一次调用内的发数。

**顺带一个可复算的事实（推翻我上一节的猜测）**：中途那一格在第 1 帧进度回调后是 **「0 B」而不是 "-"**——
引擎在探测之前 `file_size` 是结构体初始值 0，`formatBytes(0)` 就印 "0 B"。这条既解释了为什么
`onTaskProgress` 里 `total > 0` 那道守卫不能省，也说明"中途显示 `-`"这个说法本来就不成立。

**像素级核对仍然没有证人，而且现在知道为什么不用再做**：offscreen 平台的 Qt 窗口**根本没有 HWND**
（子进程 pid 连 60×0.25s 枚举全空，换 windows 平台跑同一枚 exe 也是 0 个可见窗口）。
所以"拍到那一列"这件事**不可能**由任何探针给出，只能靠人在真窗口里看一次。已登记为需求而非缺陷。

**闸门环境里解释器不是 `python`（这条差点变成假绿）**：`ui_probe_run.sh` 环境下三个候选名的实测结果是
`试过的解释器=python=fail0,python3=fail0,py=ok` —— 前两个 `error()=Unknown(0)` 是"没试过"的初值，
不能当证据，所以读数必须印**哪个名字成功**。CI 侧 `Setup toolchain` 只往 GITHUB_PATH 前面加 mingw64/cmake，
那里没有 python ⇒ runner 上的 `python` 是 windows-latest 镜像自带的。**不许回退成本机绝对路径**
（那等于把 CI 判据改成我这台机器的形状）；三个都起不来就三条全红 + 读数行说明试过谁。

**通过闸门这道门的变异（不是单独手跑）**：摘掉完成面那一发 ⇒ 套件报
`PASS=73 FAIL=2 问题模式: IDM_SIZELIVE_PROBE(F=2,rc=1)`；还原后 `PASS=75 FAIL=0 零断言=0 问题模式: 无`，
且 `src/gui/main_window.cpp` sha1sum 与备份同为 `eeda389850ce…`。

**收摊**：每次运行留下的 `%TEMP%\idm-sizelive-probe-<ms>`（各含一个被单例句柄占住的 history.db）
本轮清过两批共 14+ 个；**判据不含清理**（竞态敏感，见 §7 第 26 条末），但读数行印出路径。
名单门 `probe_roster` 同步收录新模式（代码声明=20 / CI=18 / 文档表=20，违规=0）。

### 2026-10-02 06:3x→07:2x（第 30 条）— 键盘面两条登记项：一条**真修了**，另一条是**我写错的期望**

台账里挂着两条"发现了、没有任何断言"的键盘面问题。本轮给它们配探针 `IDM_KEYNAV_PROBE`（K1/K2 两面），
结果一面成立、一面被自己的普查推翻。**推翻的那条比修好的那条更值得记**。

**K1（Ctrl+N）成立并已修**：`setupMenus()` 里那条 `fileMenu->addAction("新建任务(&N)...", QKeySequence::New, …)`
的 `shortcutContext` 停在默认 `WindowShortcut`，而主窗口 `focusPolicy=ClickFocus` ⇒ 焦点不在任何子控件上时
没有受理者；工具栏 tooltip 却明写「新建下载任务 (Ctrl+N)」，等于对外承诺了一个不生效的键。
修法：显式 `newAct->setShortcutContext(Qt::ApplicationShortcut)`（main_window.cpp:575）。
变异（摘掉这一行）⇒ `[FAIL] … context=1(0=Widget 1=Window 2=Application 3=WidgetWithChildren)`，还原 ⇒ `context=2 [PASS]`。**这条有牙。**

**量具限制（差点把测具的锅记到产品头上）**：第一版 K1 判据是"发 Ctrl+N 看有没有弹窗"。
canary 用同键同 context 的临时 `QShortcut` 试了四种发法——`postEvent` / `sendEvent` 到焦点件 /
`activateWindow` 后 send 到主窗 / `QCoreApplication::sendEvent`——**实测累计触发 0**：
offscreen 平台没有活动窗口，Qt 的快捷键处理拿不到受理者。所以那条红属未归因，改成只读状态
（动作存在 + context 值）才立得住。读数行仍印 canary 数，免得下次有人以为"发键能证"。

**K2 被我写的期望害了一次，随后撤回**：台账原话是「Tab 到『保存目录』会弹模态选择器」，
我据此断言"两次 Tab 必须落在目录输入框"。为了让它绿，我先往 `NewTaskDialog` 加了 7 条 `setTabOrder`。
然后做了整链普查（`nextInFocusChain`，从焦点件所属容器起步）：

```
url → dir → 浏览... → name → spin → lineEdit → combo → 刷新格式 → QWidget → QLabel → buttonBox → 确定
```

两件事同时暴露出来：① **摘掉全部 `setTabOrder` 后这个读数逐位不变** ⇒ 我加的那批是死代码
（Qt 默认的"按创建顺序"链本来就已经是这个形状）；② 「浏览...」**夹在两个输入框中间**，
Enter 永远落不到它身上 ⇒ 台账那句描述本身不成立。于是我把产品改动**整批回退**
（`git checkout -- src/gui/new_task_dialog.cpp`），并把 K2 判据改成断真实需要的不变式
（目录框在链上、下一站是「浏览...」）。`setDefault/AutoDefault` 那条也试过：摘掉后
`确定=default:1/auto:1` 不变（`QDialogButtonBox` 自己就设了）⇒ 无牙，降级成对照读数、不进退码。

**坦白清单**：K2 现在这条断言**没有被任何变异证明过**（我没能构造出"产品变一下它就红"的代码状态），
所以它按台账规矩算**读数**，不算证人；K1 才是本轮唯一有牙的一面。
套件现 **21 模式 / PASS=77 FAIL=0 零断言=0**，引擎门 `180/0`，NOTICE §1 四行重签
（src 931274 / tools 596027 / docs 393443 / .github 36279，合计仍 354=344+10）。

**踩到的两个测具坑（都是第三次犯同一类）**：
1. 我给 `context` 写的图例凭印象写成"2=Window 3=Application"，实测值是 1——**读数没错、解释错了**。
   现在图例从 qnamespace.h:1317-1322 现抄，并把数字与文字一起印。
2. `ninja -t commands` 那次我拿旧 exe 读了两轮"新"读数：一次是正则把 `new_task_dialog.cpp` 改坏、
   编译 FAILED 我却在看上一条 PASS。**`BUILD_RC!=0` 的那一轮不存在"跑过了"**——已在本条再次确认。

### 2026-10-02 07:3x→08:4x（第 31 条）— 引擎门自己红过一次：179/1，红的是夹具的残留目录

`[13] 保存目录不存在（多级）` 报 `目标多级目录预先确实不存在` 失败。三条 PASS 同时说明
下载本身是好的 ⇒ 红的不是产品，是**这项什么都没测**。

根因在拆除逻辑：`RemoveDirectoryA` **只能删空目录**。上一轮我把 `kill $SRV` 写在同一条命令里、
引擎自测还在跑就被打断，`sub\dirfix.bin` 留在盘上；于是"先删再断言不存在"静默失败。
修法是先清空目录内容再摘目录（engine_selftest.c:1195 起）。

⚠ **这一处修复不占功**：控制变量 A/B（同一份手工种的夹具、只切拆除算法）实测旧代码也返回"已删净"
⇒ 我没能让任何断言为它红过。它是夹具健壮性，不是产品缺陷的证人。别写进"修好了什么"。

**顺带两次自我纠错（都是服务器没起来造成的假象）**：
- 一次把服务进程在同一条命令末尾杀掉，导致后续 62~70 条 `[FAIL] status=4/-1`——**收尾不得和测量同一条命令**。
- 一次后台任务的 bash 结束时把子进程 python 一起带走，而引擎自测打印了
  `服务端就绪检查：★超时，后续结果不可信` 却仍继续跑完 ⇒ 那批 FAIL 全是环境的锅。
  **这条旗子必须看**：`== 结果` 之前若出现 ★超时，整轮作废、不许引用任何计数。
- 现值（本轮最后一条有效运行）：`服务端就绪检查：已响应 /_stats` + **`180 通过，0 失败`**，
  逐字档 `docs/engine-gate-green-*.txt`；红的那轮原文同批留档 `docs/engine-gate-red-13-residue-*.txt`。

### 2026-10-02 22:1x（第 32 条）— 活/死名单门补上「僵尸 include」这一族；顺带量到套件自身的偶发红

**做了什么**：`tools/live_roster.py` 以前只建模「死头是否可达」——可达就报 info、永不红。
于是 §1.2 归位表里唯一未清的那项（`main_window.h:10` 仍 include `legacy/glass_effect.h`）
只存在于**一句散文**里，"清掉了"和"没清"在退码上一模一样 ⇒ 假绿。现在判据从代码现算
「谁 include 了实现不编译的头」，与表里依据列点名的 includer **双向比对**，两侧都能红。
清掉那条 include 后，`glass_effect.h` 的类别由「仅声明型死头」改为「未编译」（代码变了，不是我改说法）。

**夹具三次才配出牙（每次都暴露不同的真问题）**：
| 变异 | 第一次结果 | 真原因 |
|---|---|---|
| M6 删依据列符号名 | **rc=0 假绿** | 判据拿**表里的字符串**当搜索词：删掉后 `for sym in …` 循环为空 ⇒ 静默跳过。改成从死头源码抽它自己声明的类/命名空间 |
| M6a 登记一条真事实（前置面） | rc=1 | 抽符号时用了全文正则 ⇒ 把头里**使用**的 `QString::` 当成它声明的符号。只认 `class/struct/enum/namespace` 定义点 |
| M10 表里塞一个不存在的 include 点 | **rc=0 假绿** | 对照集只在「仅声明型死头」分支收集，而那句话写在「未编译」行里 ⇒ 根本没进集合。收集不再依赖类别标签 |
| M11 代码新增僵尸 include | rc=1 ✅ | —— |
最终 `mutation_fixture_live_roster.py` **13/13 道按预期**（含还原逐字核对 + 还原后再跑门 rc=0）。

**夹具自己漏过一次脏盘**：M6 抛异常那轮 `restore()` 根本没跑，`task_detail_dialog.h` 被加的 include
和 ARCHITECTURE 里注入的那一行都留在盘上，后面 M7~M11 全从残骸起跑（三条 SKIPPED-ANCHOR + 一条假 BAD）。
修法是**把登记放进 `put()` 里**（写之前先存原件），这样"忘了 snap"结构上不可能发生。

**另一件必须记下的事：这套件的计时类断言本身会偶发红**。本轮连跑抓到两次、不同模式：
`IDM_ACTIVATE_PROBE` 实测最长轮次 2510ms（阈值 500）、`IDM_FORWARD_PROBE` 的「对方回了成功」红一次。
分开单跑 FORWARD 8/8 绿、随后聚合 3/3 全绿 ⇒ 复现率约 **2/9 ≈ 22%（分母就是这九轮，不外推）**。
**没有把它当产品回归**：两份原文进仓（`docs/suite-flake-*-1002-222144.txt`），根因未归因。
本机前台焦点抢不到、同机还有别的会话在跑 ⇒ 计时类读数天生不稳；这两条判据防的是"GUI 线程被独占"，
不是"这台机器当时不忙"。要坐实得给它们配同机负载对照面，目前没做。

### 2026-10-02 22:3x→23:0x（第 33 条）— 偶发红排查：一条归因到量具、一条仍未归因，但装上了能定性的读数

上一轮只报了分母（约 2/9）。本轮按用户要求查根因，结论分两半，**其中一半是我自己造的**。

**ACTIVATE 的单轮 2510ms ⇒ 归因到测具的采样方式**
`QCoreApplication::processEvents(flags, maxtime)` 的第二参数在 Qt 6.11 头文件里是 `int maxtime`
（墙钟上限），**不是事件条数**；我原来写 `processEvents(AllEvents, 20)` 时以为它是"取 20 个事件"。
这一轮被 Windows 队列里不属于本探针的事件占着时，调用会等满预算 ⇒ `one.elapsed()` 把**别人的阻塞**
记成"我的 GUI 线程被独占多久"。改成 `QDeadlineTimer(20)` 给每轮硬预算，并印出撞预算次数。
实测改后 8/8 绿、单轮 210~225ms（改前那次红是 2510ms）、`撞每轮预算次数=1`（那条空转连接本身）。
同一段第二个采样循环（真请求那一轮）同样改了。⚠ **这是量具修正，不是产品修复**：判据数值含义变了
（现在才真是"每轮 ≤20ms 预算下的最长一轮"），阈值 500 没动。

**FORWARD 的那次红 ⇒ 未归因，但读数已补齐**
一手证据（`docs/suite-flake-forward-red-1002-222144.txt`）：`结果=2 期望=1`、
stderr 同时含 `Log::warn` 与 `QTextStream` 两份「已连上运行中的实例，但请求没写进去（Unknown error）」，
服务端看到连接=1（符合期望）。也就是说 worker 走的是**写失败分支**，而 `error()` 停在 NoError
⇒ Qt 只能翻成 "Unknown error"，**状态码当时没落进任何读数**，事后判不了。
本轮复现不出来：DIAG-off 3/3 + 3/3 绿、聚合 5/5 绿 ⇒ 记**未归因**，不写"没问题"。
补上的定性通道：
· 产品侧（cli_forward.cpp:98）把 `state / error 枚举 / 送出字节数` 一起写进那句原因 —— 下次红一次就能定性；
· 探针侧加 `IDM_FWD_DIAG=1`：worker 跑完后用探针自己的客户端重放同一条用例，打连接/写入/flush/写完确认/state/error。
  ⚠ 重放**绕开 freopen 与 worker 线程**两个形状，所以它只在抓到红时有用，不能拿"重放正常"证明产品没事。

**我这轮自己造的第二把半坏的门（必须记）**：诊断重放第一版放在 worker **之前**、且多占一条连接，
而假服务 handler 按累计连接数 `seen` 决定回不回话、`held` 里的 socket 永不清空 ⇒
case1~4 **稳定全红（3/3、又 3/3）**。这不是被测代码的问题，是我加了观测就改变了被测对象。
修法：重放挪到 worker 之后 + 每条用例开头 `resetPeer()`（清 held、归零 conns）。
两面各测 3/3 绿后才算修好。**教训：给探针加诊断，必须先证明诊断本身不改判据读数。**

**还有一次差点误判**：我以为 `truncatedTurns` 那版没生效，是因为我把 sha 算法搞错了——
我用 `sha1sum`、sweep 用 `sha256sum`，两个号不一样长，我一度读成"二进制没变"。
现值：套件 **21 模式 / PASS=77 FAIL=0 零断言=0**（改后连跑 6/6 轮无红）、六道门 rc=0、夹具 13/13。

### 2026-10-02 23:2x→23:3x（第 34 条）— 主控点名的不是偶发红，是那句「六个 rc=0 吃的是旧二进制」

**为什么这条更严**：红会被看到，假绿直接进结论。上一轮我在 `ninja` FAILED 的那一轮里读到六个 rc=0，
并把它们当"跑过了"写进报告 ⇒ **那六次读数全部作废**，包括我据此说的"ACTIVATE 8/8 绿"。
本轮不引用上轮任何读数，全部重跑，且每条都带仓根 + commit sha + exe sha12。

**新增闸门 `tools/build_gate.py`（会拦退码，不是日志）**
- J1 `BUILD_RC != 0` ⇒ 退 3，**不进入断言阶段**；
- J2 每个被测 exe 的 mtime 必须 ≥ **它自己输入的 mtime** ⇒ 否则退 4。
  ⚠ J2 第一版写成 `mtime >= 本轮构建开始时刻`，实测立刻误红三个合格产物：ninja 是增量的，
  内容没变时 no-op、不 touch 产物 ⇒ **喊狼的门比漏检更贵**。改成"不早于其输入"后 3/3 OK。
- 输入集从构建系统拿：`ninja -t query <exe>` → 各 .obj 节点 → `ninja -t deps <obj>`。
  ⚠ 第一版对 **target 名**调 `-t deps`，实测吐「deps not found」⇒ 解析到 0 条输入 ⇒ 又把合格产物判死。
- 入口 `tools/ui_probe_run.sh` 已接：不合格 ⇒ `exit 12`，一个探针都不跑（实测 entry_rc=12）。

**反面夹具两面各留档（`--selftest` 现 2/2 按预期红）**
| 面 | 做法 | 期望 | 实测 | 原文 |
|---|---|---|---|---|
| A 编译失败仍想跑测试 | 把 `main()` 改坏再跑门 | rc=3 且输出里没有"允许进断言" | rc=3 未进断言=True | `docs/buildgate-fixtureA-build-fails-rc3.txt` |
| B exe 比源码旧 | 逐个目标把它真依赖的源文件 mtime 推未来 | rc=4 且**每个产物都红** | rc=4 不合格=3/3 | `docs/buildgate-fixtureB-stale-exe-rc4.txt` |
⚠ 面 B 第一版只推 `tools/ui_snapshot.cpp`，而 engine_selftest / idm-next 不依赖它 ⇒ 只拦下 1/3
（"部分产物过期"漏网）。现在逐目标选输入。所有改动在 finally 里逐字还原，跑完 `git diff` 为空。

**ACTIVATE 重跑（同一条命令里先过门、再连跑八次，全带身份）**
门：`BUILD_RC=0 gate_rc=0 本轮是否重编=是`，`exe_sha12=0af3cadceca0`，repo `@479069f`。
分母 **8/8 跑成**、**红 0**、单轮 205~229ms。**两件分开报**：
① 判据口径已变（第二参数从"我以为是事件数"改为 `QDeadlineTimer(20)`/轮），阈值 500ms 未动 ⇒
   **"通过"与"多快算通过"是新口径下的读数，与旧口径不可比**；我没有新旧对照数据，不声称等价。
② 每次读数固定印出 `撞每轮预算次数=1`（那条空转连接自身），这是量具读数、不进判据分子。
原文：`docs/activate-fresh-1002-232805.txt`。

**FORWARD 差分（fault=off / fault=on 各五轮，输出留档）**
扰动定义：`IDM_FWD_DIAG=1` ⇒ worker 之后多开一条连接并重放同一条用例。
实测 off 5/5 rc=0、on 5/1 rc=0，FAIL 均为 0，diag 行数 off=0 / on=5（证明开关真的生效）。
⚠ 第一版差分**无效**：我用 `env $E` 且 E 为空串，结果执行成 `env IDM_FWD_DIAG= ...` ——
变量被置成空值，而判据用 `qEnvironmentVariableIsSet`（空串也算已设置）⇒ off 面根本没关、两面同一面。
**对照面要用"变量不存在"构造，不能用"变量等于空"**。那份废档已删并在 `docs/attic/README.txt` 写明原因。
定性到此为止：**那次红只证明了「写失败分支被走到」**（stderr 两份"没写进去"+ 服务端看到连接=1），
原因未归因；产品侧现在会把 `state/error枚举/送出字节数` 写进原因，下次红一次能定性。

### 2026-10-03 00:0x（第 35 条）— ACTIVATE 新旧口径对照：**结论把我上一轮的归因推翻了一半**

对照设计：同一枚二进制（`exe_sha12=0980d043f275`，repo `@72caa3f`，BUILD_RC=0、gate_rc=0、本轮重编=是），
只切一个环境变量 `IDM_ROUND_CRITERION=count|deadline`；判据表达式与阈值 500ms 两边都没动。
每行末尾印「口径=…」当生效证人 —— **第一版没印这行**，两面读数几乎重叠（219~228 vs 203~227），
当时无法区分"两种写法等价"还是"开关根本没生效" ⇒ 那份档作废（`docs/activate-criterion-differential-1002-235631.txt` 已删）。

**数据（各 14 轮）**
| 口径 | n | min–max | 超阈次数 |
|---|---|---|---|
| count（旧：`processEvents(flags, 20)`） | 14 | 208–225ms | **0** |
| deadline（新：`QDeadlineTimer(20)`/轮） | 14 | 207–**1779**ms | **1** |

**三件事必须分开说**：
1. **抓到的那次红落在新口径面上**，不在旧口径面上 ⇒ 我上一轮说的"旧口径把别人的阻塞记成我的轮次、
   改成 deadline 就干净了"**没有得到数据支持**。常态下两口径分布重叠（约 208–225ms），差别看不出来。
2. 那一次的一手读数：`排空静默=成 peakN=1 取尽自身峰值=215ms 单轮最长=1779ms IPC占比=12%`，
   同轮另一读数 `真请求往返=220ms / 同窗口最长轮次=221ms` ⇒ 1779ms **不是**那条真请求造成的。
   定性到此：**有一轮 processEvents 花了 1779ms，其中 IPC 只占 12%**；剩下 88% 是谁——**未归因**。
3. 因此"量具修正修掉了偶发红"这句话我收回。现在的诚实说法：**换口径没有降低复现率的可测证据**
   （n=14 对 n=14，一次红 vs 零次红，不足以定方向）；阈值 500ms 仍未动，也仍**没有重新标定过**。

**顺带一条方法论**（和 §7 第 33 条同源）：对照实验的开关**必须在输出里自证生效**。
我这次差点拿一份"两面其实是同一面"的档去签字 —— 靠的是读数重叠这个可疑信号回头查，不是靠设计时发现。

### 2026-10-03 00:1x（第 36 条）— FORWARD 根因复现：一个假设被否证，机制造出来了，但原始那次仍未定因

两个候选扰动各切各的、同一入口跑（`tools/fwd_repro.sh <n|block> <轮数>`，gate_rc=0 才继续）：

| face | 扰动 | 结果 |
|---|---|---|
| n | 假服务 handler 摘掉显式 `flush()` | **6/6 未复现** ⇒ 假设被否证：Qt 在 socket 析构时自己会 flush，摘掉造不出「写完不落」 |
| block | worker 在 `write` 前按住 2500ms | **6/6 稳定复现** rc=1 FAIL=4、命中写失败分支=4 |

新加的状态码第一次就用上了，一手读数：`Unknown error；管道状态=3(Connected)，错误码=-1(NoError)，送出=69 字节`。
机制：**QLocalSocket 的字节确认要靠客户端自己的事件循环派发**；把被测函数丢进没有循环在转的 worker 线程，
2 秒内 `bytesWritten` 不落地 ⇒ 走写失败分支。**所以这类红是测具形状造成的，不是产品 IPC 缺陷。**

**与原始红档逐字段对账**（`docs/suite-flake-forward-red-1002-222144.txt`）：形状一致 ——
`结果=2 期望=1`、服务端看到连接=1、stderr 同时含 `[WARN]` 与 `QTextStream` 两份「没写进去」⇒ 走的就是同一条超时分支。
但两处不能划等号：① 原档 stderr=113 字节 / 复现 67 字节；② **原档事发时还没有状态码**，拿不到 state/error 去比。
⇒ 定性上限：**机制已被证明可稳定制造同一分支，但"原始那次是否同因"仍未证明**。不写成已归因。

**落一条规矩**：留档必须当场带 `exe_sha12 + commit sha`，否则事后无法与任何复现对账 —— 这份旧档就是反面例子。
两面输出各自留文件：`docs/forward-repro-n-1003-001505.txt`、`docs/forward-repro-block-1003-001645.txt`。
复现用的注入代码（假服务 flush、产品侧阻塞钩子）**已全部逐字还原**：`git diff --stat src/ tools/ui_snapshot.cpp` 为空。

### 2026-10-03 00:4x（第 37 条）— B0/B1/B2 三面实验：**否证了我上一轮写的机制**，并给出产品侧 call-site 证明

主控指出我上一条结论里"复现机制"与注入位置不一致。做三面小实验（同一枚 exe_sha12=144ae566163e、repo `@f898ac1`、gate_rc=0、只切 `IDM_FWD_BLOCK_STAGE`、阻塞时长同为 2500ms）：

| face | 注入点 | 结果 |
|---|---|---|
| B0 write 前 | 按住 2500ms | rc=1 FAIL=4，`waitForBytesWritten=0 elapsed=0ms` |
| B1 write 返回后 | 按住 2500ms | **同上，逐字一致** |
| B2 flush 之后 | 按住 2500ms | **同上，逐字一致** |

三面差异为零 ⇒ "阻塞位置"这个变量根本没作用到机制上；而且 `elapsed=0ms`（等待没走到超时就直接 false）。
**独立最小探针**（同 Qt 6.11.1、同机、对端 write+flush+deleteLater，两种线程形状各一次）给了反证：
```
[worker-none] ok=0 elapsed=2006ms state=3 err=-1
[gui-loop ]   ok=0 elapsed=2004ms state=3 err=-1
```
⇒ **"有事件循环的那一面同样失败"** ⇒ 我上一轮写的「worker 没有事件循环导致 bytesWritten 不落」**被否证，撤回**。
同时注意独立探针是 `elapsed≈2000ms`（等满），钩子里是 `0ms` ⇒ 两者不是同一个形状，**我的钩子读数本身也不可信**（它在 stderr 被 freopen 的窗口里插了 fprintf）。

**产品侧 call-site 枚举（这才是把结论往上抬的那一步）**：`forwardToGui/waitForGui` 的全部调用点都在 `src/main.cpp`，且**四个全在 Application 对象构造之前**：
| 行 | 调用 | 当时有没有 Q(Core)Application |
|---|---|---|
| 103 | `forwardToGui(cliArgs)` | 无（app 在 :125 才构造） |
| 111 | `waitForGui(cliArgs,…)` | 无 |
| 167 | `waitForGui(forwardArgs,…)` | 无（GUI 模式 app 在 :201） |
| 188 | `forwardToGui(forwardArgs,false)` | 无 |
全部在主线程、无线程切换。⇒ **正式产品路径本来就是"没有事件循环"的形状来调这个函数**，
探针那个 worker 线程形状并不是测具独创。**所以不能再说"属于测具形状而非产品问题"** ——
这句降级为：**未定性**。要判产品是否真会踩，得先看 `waitForBytesWritten` 在无 QCoreApplication 时返回 false 是 Qt 契约还是本机管道行为（这一步需要 Qt 文档/源码级证据，我没做）。

**FORWARD 当前准确状态保持不变**：同分支机制已稳定复现（但复现注入的形状可疑）；原始红档同因性未证明；机制解释已撤回。
钩子代码已全部撤销（`git diff src/` 为空），临时探针文件已删净。按主控指示，**停止再磨 FORWARD**，
下一步转向 ACTIVATE 的 500ms 重标定与剩余 88%，以及给 K2 做 mutation。

### 2026-10-03 01:0x（第 38 条）— ACTIVATE 量具标定：**500ms 管的是一个自相关常数**，另有一条身份更正

**先更正身份（比标定结果更要紧）**：`1ed129f` 这个 commit **从未被任何 ACTIVATE/FORWARD 读数实际测过**。
B0/B1/B2 三面跑的是**带 REPRO-HOOK 的二进制**（`exe_sha12=144ae566163e`，内含 fprintf(stderr) 注入），
撤钩子重编后无钩子产物是 `6c2fe70732db`（no-op 重编两次逐字相同 ⇒ hash 稳定）。
⇒ 这解释了 `elapsed=0ms` 的形状异常，也意味着那三次实验的绝对值不可引用；已在
`docs/forward-b0b1b2-1003-004132.txt` 末尾就地标注。教训：**读数所属的 exe hash 必须与提交时的源码态对上**，
"我改了 src 又撤回来"这一步会让中间所有读数失去归属。

**FORWARD 状态固定为三层，不再外推**（主控确认）：事实层＝历史红档确实走过 `waitForBytesWritten` 失败分支、
当前也能稳定制造同一业务分支；机制层＝"worker 无 event loop"已被独立探针否证，**机制未定**；
历史同因层＝**未证明**。产品侧 call-site 枚举（四个调用全在 Q(Core)Application 构造之前、主线程）
是今后再碰 FORWARD 时**第一个要检查的事实**，不许再从 worker/thread 猜起。

**ACTIVATE 标定（HEAD `1ed129f`、exe_sha12=`6c2fe70732db`、阈值 500ms 全程未动）**
| 面 | n | 红 | worst p50/p95/p99/max (ms) | deadline_hits p50/max |
|---|---|---|---|---|
| idle | 50 | 0 | 216/229/229/**229** | 1/1 |
| cpu（14 忙等进程） | 20 | 0 | 219/230/231/**231** | 1/2 |
| io（4×1MiB 循环读写） | 20 | 0 | 215/225/237/**237** | 1/1 |
| msgs（EnumWindows + PostMessage 广播） | 20 | 0 | 216/224/225/**225** | 1/1 |
| post（负载撤销后的对照） | 20 | 0 | 217/225/229/**229** | 1/1 |

三条结论：
1. **三种压力都没把单轮推离 idle 分布**（重叠到无法区分）⇒ 我先前怀疑的"同机别的会话/消息队列活动
   混进我的计时"在这三面下**没有再现**；那次 1779ms 与这三种负载无关，仍是孤立观测。
2. **读数几乎全由量具自己的预算决定**：`deadline_hits` 恒为 1、worst≈216ms —— 那是空转连接的
   `msleep(2500)` 被每轮 20ms 预算切出来的常数。⇒ 它现在测的不是"GUI 线程被独占多久"，
   而是"窗口里有多少个 20ms 预算片"。**判据语义已与阈值 500 的原始意图脱钩。**
3. 500ms 在观测分布里**远在尾部之外**（各面 max ≤237ms，触发需 ≥25 次命中）⇒ 它既不是"松了会漏"的
   阈值，也不是按分布定的，是旧口径遗留值。

**下一步不自行改判据，交回主控定**（选项已随档给出）：A 重标阈值（等于给坏量具调参）／
B 改成"单轮预算命中数"判据／C 先修量具（把空转窗口与采样窗口解耦，别让 msleep 切成常数）。
建议顺序 **C→B**；A 单独做不建议。数据档：`docs/activate-baseline-idle-1003-010247.txt`、
`docs/activate-baseline-load-1003-010941.txt`，工具 `tools/activate_calibrate.py`（只用标准库，无新依赖）。

### 2026-10-03 01:5x（第 39 条）— K2/K3 mutation testing：三段链现在有牙，且三种破坏的失败原因可区分

**为什么要加第三段**：K1 读静态属性（动作存在 + context）、K2 读 Tab 链形状 —— 两条都**不经过命令路由、
也不到行为末端**。所以"破坏 accelerator / 破坏路由 / handler 不开窗"三种 mutation **一个都杀不死**：
不是测试没牙，是牙不在那个位置。新增 K3 把 `key → command → handler → dialog` 拆成三条独立判据。

**结果（每面只切一个变量，各留原始输出于 `docs/k2-mutations-3face-f5f093406454.txt`）**
| mutation | 改了什么 | ①key→command | ②command→handler | ③handler→dialog | rc |
|---|---|---|---|---|---|
| M1 accelerator | `Ctrl+N` → `Ctrl+J` | **红**（act=(无)） | 红（连带） | 红（连带） | 1 |
| M2 command-route | 槽改接 `onBatchImport`（键与 context 都对） | 绿 | **红**，`wrongDlg=BatchImportDialog` | 红 | 1 |
| M3 handler | `onNewTask()` 首行 `return`（键与路由都对） | 绿 | **绿**（`actFired=1 wrongDlg=-`） | **红**（peak=0） | 1 |
| 干净基线 | src/ diff=0 | 绿 | 绿 | 绿 | 0 |

⇒ **三段的失败签名互不相同**，M2 与 M3 不再撞成同一句：M2 说"路由开了别的窗"，M3 说"什么都没开"。
恢复源码后复跑全绿（探针 exe_sha12=`f5f093406454`），套件 **21 模式 PASS=78 FAIL=0**（比上一版多一条断言）。

**这条链上踩到的三个测具坑（都是"挂死冒充红/绿"的形状）**
1. `trigger()` 直接调 ⇒ 内联进 `dlg.exec()`，外层 `QEventLoop` 还没起来 ⇒ 守卫的 quit 作用不到它，
   M2 表现成 **rc=124 挂死而不是 FAIL**。改成 `Qt::QueuedConnection` 投递 trigger，让 exec 跑在外层循环里。
2. 守卫只认 `NewTaskDialog` ⇒ 路由被改接后开的是 `BatchImportDialog`，永远关不掉 ⇒ 同样挂死。
   现在兜底会关掉**任何模态对话框**并把它类名记进 `wrongDlg`——这一改同时把 ② 与 ③ 分开了。
3. offscreen 下窗口收不到 `Show` 事件 ⇒ 用事件过滤器采样会得到 `peak=0` 的假阴性；改用定时器轮询。

**仍未覆盖的一段（如实标出）**：① 只是"Ctrl+N 被某个 enabled QAction 占着"这种静态读取，
**没有**证明"按下 Ctrl+N 会 fire 它" —— offscreen 合成按键进不了快捷键系统（canary 四种发法累计触发 0，
本轮再次复现）。要证那一段只能靠真窗口人工按一次，或换 windows 平台跑探针。
K2 那条 Tab 链断言依旧**未被变异证明**（摘掉 setTabOrder 读数逐位不变），保持"算读数不算证人"。

### 2026-10-03 08:5x（第 40 条）— 浏览器扩展链路审计：**我上轮四条指控里三条是错的，改掉的只有一件**

起因是问"插件能不能把网页视频交给桌面端下载"。第一轮我报了 F1–F4 + 工具总开关五条缺陷，
主控按 Chromium/Edge 官方契约纠正后，本机逐条复算的结果是**连他的纠正前提也一起塌**：

| 指控 | 一手证据 | 判定 |
|---|---|---|
| F1 host 漂移到旧仓 `MFC/stage_idm/…` | `ls` ⇒ **No such file**；HKCU Chrome+Edge 两键默认值都指本仓 manifest（`reg query` 实测） | 撤回：漂移在盘上不存在 |
| F1 理由"相对 path 按 chrome.exe 解析" | Edge 官方原文：*"On Windows devices, you can use relative paths to the directory that contains the native messaging host manifest file"*；exe 与 json 同级 | **我对，且门也不该管它合法不合法——只管解析得到解析不到** |
| F2 `allowed_origins` ID 不匹配 | `key`(RSA-2048 DER) → sha256 前 32 hex → a-p = `fmnehlhldcoknecihnlbbcmhpaaapmbp`，与 json 逐字相同；全仓 grep 那个 `dkdn…` ⇒ **0 命中** | 撤回：**那个号是我编的**，不是"CMake 注入的"（CMakeLists:208-218 只同步 exe+windeployqt） |
| F3 只有 Chrome key ⇒ Edge 坏 | `install_host.ps1` 原本就 `foreach $browsers` 写 Chrome **和** Edge 两份 | 撤回，比"降级"更彻底 |
| 「三工具都存在才启动」 | `main_window.cpp:332-353` 是三条**互相独立**的 `if (autoDownloadXxx() && componentMissing(...))`；`:1338-1365` 失败按后端给具名原因 | 撤回：按功能准备已是现状，没有总开关可拆 |

⇒ **本轮唯一有靶子的改动**：安装器把仓库里受跟踪的模板当安装目标就地改写。两条后果——
跑一次就弄脏开发树；而 PS 5.1 的 `Set-Content -Encoding UTF8` **会写 BOM**，浏览器按 JSON 读会当场报错。
改成 `[IO.File]::WriteAllText(…, UTF8Encoding($false))` 生成到 `%LOCALAPPDATA%\IDMNext\NativeMessagingHosts`，模板不动。

**新门 `tools/host_manifest_gate.py`（四判据 A/B/C/D）自己先红过**，原文：
```
[FAIL] A 安装器在改写受跟踪 manifest：install_host.ps1:34: $Json | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $JsonPath
```
牙由 `tools/mutation_fixture_host_manifest.py` 证明：**5/5**（A/B/C/D 各一条变异 + 一条阴性对照"写入落在生成目录变量上 ⇒ 必须绿"）。

**过程中又踩了两个自家量具坑，都值得记**：
1. **A 判据第一版扫到 0 处写入调用却报绿** —— 我要求"目标参数紧邻 cmdlet"，真实写法是
   `… | Set-Content -Encoding UTF8 $JsonPath`（目标被 `-Encoding` 隔在后面）。修法：整行找目标。
   教训形状：**"扫到 0 处"必须先当成量具失明的信号，不能当成通过**。
2. **夹具用 `read_text/write_text` 往返，把 LF 写成 CRLF** —— 盘上多 1 字节、`git status` 却报干净
   （smudge 看不见），等于夹具自己污染受跟踪文件；同一轮还因门用 `utf-8-sig` 解码**把 BOM 吞了**，
   M-D 红了但归因表读出空集。修法：夹具全程字节级备份 + 还原后逐字节对账；门按**浏览器口径**读
   （拒绝 BOM），并把「跳过没跑起来的判据」计入退码（`实际执行=4 跳过=0` 印在首行）。

**仍未验的（不在本轮范围）**：真实 E2E 一次都没跑过（加载 unpacked → 视频站 → 发送 → host → `idm-next-ipc` → GUI 任务列表）；
装载后 Chrome 实际分配的扩展 ID 未测；HKLM 若被安装包写过则搜索顺序谁赢未测；相对 path 真被解析成功无记录。

### 2026-10-03 09:3x（第 41 条）— 第一次真实 Native Messaging E2E：**主链跑通，并当场抓出一条让整条安装链失效的缺陷**

顺序按主控给的九步走，只拿普通 HTTP 直链（不混 yt-dlp/FFmpeg/HLS）。工具 `tools/nm_e2e.py`：
headless Edge 加载 unpacked 扩展 → CDP 打开扩展自己的 popup → `sendNativeMessage`
→ `idm-next-host.exe` → `\.\pipe\idm-next-ipc` → `ipc_server` 的 `add` → GUI 收下。

**运行态读数（逐字档 `docs/nm-e2e-gui-pass-1003-092712.txt`）**
```
[扩展] 扩展页面里 chrome.runtime.id=fmnehlhldcoknecihnlbbcmhpaaapmbp ｜与 key 派生一致=True
[第7跳-a] ping 回执={"resp":{"message":"pong","success":true,"version":"0.3.0"},"lastError":null}
[第7跳-b] add_download 回执={"resp":{"message":"已添加下载: http://…/512KB.zip","success":true}}
[证人] [INFO] "IPC 请求: add → 成功（排队 -1ms，处理 59ms，送达=1，回话时客户端=在）"
```
⇒ 上一轮挂在"推断"上的六件事一次性升级为观测：实际扩展 ID == `key` 现算值、浏览器接受生成物、
LOCALAPPDATA 方案可用、Chrome+Edge HKCU 注册有效、宿主被浏览器真拉起、`QLocalSocket → GUI` 通。

**E2E 第一次执行就撞出的缺陷（不是静态门能发现的）**：`install_host.ps1` 是 UTF-8 **无 BOM + 中文注释**
⇒ PowerShell 5.1 按 ANSI(GBK) 解码，中文字节被切成假引号、字符串跨行 ⇒ 整个脚本 ParserError，
`PS_RC=1`。**README 教用户执行的那一步根本跑不起来**。探针（PSParser::Tokenize）实测
无 BOM=2 个语法错、加 BOM=0 ⇒ 修法是给受跟踪的 `.ps1` 加 UTF-8 BOM（方向与 D 判据相反：
manifest 必须**没有** BOM，脚本必须**有**）。新门判据 E 先红原文：
```
[FAIL] E 安装器含 316 个非 ASCII 字符却无 BOM ⇒ PowerShell 5.1 按 GBK 解码会当场 ParserError（实测 TOKEN_ERRS=2）
```
`installer/package.ps1` 同形状，一并加 BOM。

**测具自己踩的三个坑（都写进 `nm_e2e.py` 注释了）**：
1. GUI 的 `Log` 走 `qDebug` ⇒ Windows 下按 **GBK** 落盘；用 utf-8 读会把中文解成乱码，
   判据永远匹配不上——第一轮就栽在这里，**证人明明在文件里却报"没等到"**。
2. `/json` 里的 `chrome-extension://…/background.rollup.js` 是 **Edge 自带服务**，不是本扩展的 SW；
   拿它当"扩展已加载"会得出"实际 ID ≠ key 派生 ID"的假结论。正解：由 `key` 现算 ID，
   `Target.createTarget(chrome-extension://<ID>/popup.html)` 进去问 `chrome.runtime.id`。
3. flatten 模式下 sessionId 要**逐帧比对**，否则别的 session 的回执会被当成答案（`got=None`）。
   另：WIN32 子系统的 GUI 必须给 `QT_ASSUME_STDERR_HAS_CONSOLE=1` 才把日志吐进管道。

**证人的形状要说清**：产品那行只印 `命令 → 结果（排队/处理/送达/客户端）`，**不印 URL**
（`ipc_server.cpp:350`）。所以"GUI 收到了这条 URL"是靠 `add → 成功` 这一行 + host 侧 success=true
两头夹出来的，不是日志里直接出现 URL。任务列表那一格属像素面，仍未验。

**第 8 步（host → CLI fallback）没走完，卡在测具不在产品**：无 GUI 时 host 正确走到
`cliAdd()`，但 `resolveCli()` 在开发树找不到 exe（候选只有同目录 / `NMH/build/` / 三个安装路径 / PATH）
⇒ 返回"IDM Next 未找到"。我手工复制出"安装形状"目录复测，四次都在同一处失败：
独立目录里的 `idm-next.exe` 报 `error while loading shared libraries: libcurl-x64.dll`，
即使 dll 就在旁边、连 Qt 与 platforms/styles/tls 一起拷也一样（而 `build/` 里同一个 exe 正常）。
⇒ **归因未完成，不写结论**；按"CI 红先读一手诊断、无证据不许沿同一假设盲改"停在这里。
便宜的下一步是读 `dumpbin /imports` 或直接在安装包里验，而不是继续手工拷贝试第五种组合。

**恢复点**：`%TEMP%/idm-probes/e2e-*/00-restore-point.json`；注册表两键已逐值写回测试前的仓库模板路径
（HKLM 本来不存在，未动）。生成目录 `%LOCALAPPDATA%\IDMNext\NativeMessagingHosts\` 保留（它是本轮验证过的产物）。

### 2026-10-03 09:5x（第 42 条）— 第 8 步依赖归因：**「dll 就在旁边却加载不了」是模拟安装不完整，不是产品缺陷**；CLI fallback 端到端已通

按主控定的五小步做，全程不改产品代码。读数档 `docs/deps-attribution-1003-0953.txt`。

**① exe 声明依赖**：`objdump -p build/idm-next.exe` 列出 16 枚，其中非系统的是
Qt6Charts/Core/Gui/Network/Sql/Widgets + libgcc_s_seh-1 + libwinpthread-1 + libstdc++-6 + **libcurl-x64**。
**② 同一枚二进制两边 import 表一致**（同一个文件，逐字节相同）。
**③ libcurl 往下**：它的 21 条 import **全是 `api-ms-win-crt-*` 与系统 DLL**（WLDAP32/bcrypt/CRYPT32/
Secur32/WS2_32/IPHLPAPI…），没有 OpenSSL/zlib/nghttp2 之类第三方传递依赖 ⇒ 那条假设排除。
**④ PATH 偷吃**：摘掉 Git/msys/anaconda 后跑三面对照——build 目录 rc=0；只拷 10 枚直接依赖的模拟目录
rc=**3221225781（STATUS_DLL_NOT_FOUND）**；补齐后 rc=0。且 `libcurl-x64.dll`、`Qt6Svg.dll` 在 PATH 上
**根本找不到** ⇒ build 的正例没有偷偷吃开发环境。

**根因**：第一次模拟只按"exe 的直接 import"拷了 10 枚 dll，漏了 build 里另外 5 枚
（`D3Dcompiler_47 / opengl32sw / Qt6OpenGL / Qt6OpenGLWidgets / Qt6Svg`）和六个插件子目录
（platforms/styles/iconengines/imageformats/tls/generic/networkinformation）。
⇒ **上一轮那句"连 Qt 与 platforms 一起拷也一样"不成立**：那三次我漏拷了 `Qt6Svg.dll`
（`cp -r build/platforms …` 那次构建目录里根本没有 svg），错误信息也从具名 `libcurl-x64.dll`
变成过 `?: cannot open shared object file`——形态不同就是没到同一根因的证据，当时我没核出来。

**中途又造了一把半坏的量具（已作废其读数）**：我自己写的 C 探针 `dllprobe.exe` 对
`C:\Windows\System32\KERNEL32.DLL` 都报 FAIL err=126 ⇒ 它连正例都判红，之前"10 枚全 BAD / build 15 枚全 BAD"
两批读数全部不算数。**教训形状：探针必须先跑阴性对照（拿一个必定能加载的系统 DLL），红了就说明是量具瞎。**
另一次污染：用 anaconda 的 python 做 `ctypes.WinDLL` 探测，宿主自己带 libcurl ⇒ 结果不可信。

**⑤ 真实 package 形状**：`installer/package.ps1:147-200 Add-RuntimeDependencyClosure` 从 exe/host 出发
递归取 PE import，`Test-SystemDll`（api-ms/ext-ms 或 System32/SysWOW64 存在）跳过，其余必须解析到实体否则
`throw 未解析的运行时依赖`，并且 windeployqt 带 `--compiler-runtime`。⇒ **打包本来就把闭包带齐**，
所以第 8 步失败只是我手工模拟不完整。

**第 8 步结论（观测级）**：完整安装形状下 host → `resolveCli()` 命中同目录 → `cliAdd()` → CLI 起任务，
回执 `{"message":"已添加下载","success":true}`，无 GUI 在场。**并用两个 URL 做了对照**：
jsDelivr 那个真落盘 `package.json` 646 字节；thinkbroadband 那个停在 `64KB.zip.idmtmp` 0 字节
（rc=0、无输出）⇒ **fallback 链可用已被正面证据钉住**；后者属网络侧形状，未归因、不算产品结论。

**明确不提前下的判断**：`resolveCli()` 在开发树找不到 app exe **不算产品缺陷**——它服务的是已安装形状
（host/app 同级），正式 package 形状下 fallback 已实测可用。只有 package 形状也断才值得动产品代码或打包规则。

**证人边界照旧**：本轮证到"CLI 起了任务并落了盘"，没证"GUI 任务列表像素里出现那一行"；后者留给 UI/像素证人，不再用日志硬推。

### 2026-10-03 10:1x（第 43 条）— GUI 真落盘 E2E：**四层证据全绿**，404 面不伪成功；thinkbroadband 继续冻结

口径由主控锁定，本条只做这一项：localhost 两路源 + 四层证据。**没有**顺手测 popup / webRequest /
DOM 扫描 / MP4 页面识别 / yt-dlp / ffmpeg / HLS / 公网 URL——任何一个混进来，红了都会重新扩大归因空间。

**测试源是自己生成的**（`tools/disk_e2e_server.py`）：payload 由位置派生的确定性字节当场算出，
启动即打印 `SERVER_READY port=… sha256=… size=…` ⇒ **源文件本身也是证人**，不依赖任何外部下载件。
刻意不复用 `engine_selftest_server.py`：它带 Basic 认证、40ms 固定延迟与三十来条分支路由，
那些多余行为会把"下载失败"的成因推到服务器上。这里只有 `/payload.bin`(200) 与 `/missing.bin`(404)，
外加必要的 Range（产品对已知长度资源会分段并发取）。

**正面 200 四层（512KiB 与 1MiB 各一轮，逐字档 `docs/disk-e2e-{512k,1mib}-pass-1003.txt`）**
```
[层① Native Host] success=True lastError=None msg='已添加下载: payload.bin'
[层② GUI IPC]     出现『IPC 请求: add → 成功』=True
                  [INFO] "IPC 请求: add → 成功（排队 -1ms，处理 54ms，送达=1，回话时客户端=在）"
[层③ 磁盘]        最终文件存在=True .idmtmp 残留=无
[层④ 内容]        size 524288 == 524288 : True ｜ sha256 前16 3dd34634f87b65cb == 同 : True
[正面 200] 四层=OK/OK/OK/OK ⇒ 已观测级打通
```
1MiB 面同样四层 OK（sha256 前缀 `a3f086db33d8d232`）。任务行对照：`#1 已完成 payload.bin http://127.0.0.1:8241/payload.bin`。
⇒ **Browser/host → GUI → HTTP engine → disk 已观测级打通**（这句话现在才有资格写）。
按口径明确一句：`add → 成功` 只代表 **GUI 收单成功**，所以 ①② 单独都不算下载成功——本轮是 ③④ 把它补上的。

**404 面（不伪成功）**
```
[404 轮询] 终态行=['#2\t失败\tmissing.bin\thttp://127.0.0.1:8241/missing.bin']
[404 层③] 目录内容=['missing.bin.idmtmp']  missing.bin.idmtmp size=0
[404 判定] 生成看似成功的最终文件=False｜任务被标『已完成』False
[404 判定] 日志里的失败证据行数=2   [INFO] "任务 2 失败" ×2
```
⇒ 四条要求全部满足：没有假装成功的最终文件、没有误标完成、`.idmtmp` 的大小与终态都记了、
有具名失败证据。**保留 `.idmtmp` 供重试本身不算缺陷**（本轮没把它写进判据）。
一个测具细节：终态必须**轮询到收敛**。第一次我用固定 `sleep(12)` 读到的是 `#2 下载中`——
引擎的重试是有界的（`retry_count` 递增到上限，`download_core.c:1095`），"下载中"只是中间态，
睡固定秒数会把中间态当终态读。改成每秒查、最多 90s、见到终态词才退。

**thinkbroadband 保持冻结**：解冻条件是"localhost 也出现 `.idmtmp=0`"，本轮 localhost 完整落盘
（③④ 双绿、无残留），所以公网那次停在 0 字节仍是站点/网络侧问题，不与本地链共享机制。

**新工具的两处形状，别改回去**（都栽过）：本机 anaconda 的 python **没有 `socket.AF_UNIX`** ⇒
Windows 命名管道只能走 Win32 `CreateFileW`+`WriteFile`/`ReadFile`；产品 IPC 协议是**裸 JSON**
（`ipc_server.cpp:108` 起），不是 native messaging 那种 4 字节长度前缀——按前缀读会拿到一坨不可解析字节。
另：任务终态是从 GUI 自己的 `list` 命令读的，不去猜它的 SQLite schema。

**仍未验**：任务列表那一格的像素证人（本轮证到状态字段是「已完成」，不是屏幕上看得见）；
网页识别 → popup → sendNativeMessage 整层（下一层目标）；MP4/m3u8 真实媒体链。

### 2026-10-03 10:4x（第 44 条）— 网页识别 → popup → 真点击：**A/B/阴性 三面全绿**，主路径打通到用户点那一下

基线 `b1145d1` 已证明后半段（GUI 收单 → engine → 落盘 → SHA256），本轮不重复验 SHA256，
只把前后两条已证链条接起来。工具 `tools/sniff_e2e.py`，逐字档 `docs/sniff-e2e-pass-1003.txt`。

**A 面 webRequest 嗅探** —— `/network.html` 里**没有 `<video>`**，只有 JS `fetch('/media/network.mp4')`
（Content-Type: video/mp4）：
```
[网页] 请求发生次数={"net":1,"doc":1}
[popup] 行数=2 含 network.mp4 的行数=1     network.mp4 127.0.0.1:8252 · 195 KB · MP4
[阴性] popup 把 document.txt 列成资源了吗=False
```
⇒ **网络嗅探通道已被运行态证明**，且同一页面请求的 `text/plain` `/document.txt` **没有**被吞进列表。

**B 面 DOM 扫描 + MutationObserver** —— `/dom.html` 初始无媒体元素，3s 后动态插入 `<video><source>`：
```
[网页] 插入前 DOM 状态={"videos":0,"inserted":0}
[popup] 插入前含 dom.mp4 的行数=0（必须 0）
[网页] 动态插入完成={"videos":1,...}
[popup] 插入后含 dom.mp4 的行数=1 ｜再等 4s 后行数=1（不无限增长）
```
⇒ 「源码里有 MutationObserver」升级为「动态媒体 DOM 运行时确实能被发现」。

**第三步：真点击，不是 console 直调 API**
```
[popup] 勾选目标行={"ok":true,"usedCheckbox":true,"text":"dom.mp4 …MP4"}
[popup] 发送按钮存在={"ok":true,"disabled":false,"label":"下载 (1)"}   ← 文案随勾选变化＝真的走了 popup 代码
[popup] 实际 click 发送按钮={"ok":true}
[GUI] 新增『IPC 请求: add』行数=1   [INFO] "IPC 请求: add → 成功（处理 62ms，送达=1）"
[GUI] 任务列表含 dom.mp4 的行=['#1\t已完成\tdom.mp4\thttp://127.0.0.1:8252/media/dom.mp4']
```
连跑两遍结果一致。**措辞仍按主控收紧**：`extension runtime/native host → GUI → HTTP engine → disk`；
本轮新证的是【网页识别 → popup → 点击】这一段，不写成"网页→Browser 全链"。

**这一轮最贵的测具教训（红了三次才找到，全部是量具形状、非产品缺陷）**：
popup 是在**自己加载时**用 `tabs.query({active:true,currentWindow:true})` 拉当前页资源的
（`popup.js:615` → `background.js:634`）。而 CDP 新建的 popup target **本身就是活动标签** ⇒
"先开 popup、后激活页面"永远读到空列表。正解顺序是 **`Target.activateTarget(页面)` → 再 reload popup**。
自证读数写在输出里（对照实验的开关必须自证生效）：
```
[扩展] popup 眼里的 active tab=["chrome-extension://…/popup.html"]        ← 错顺序
[扩展] 刷新 popup 后它眼里的 active tab=["http://127.0.0.1:8252/network.html"] ← 对顺序
```
期间我差点把两件事误判成产品缺陷并当场收回：① 「嗅探没抓到 mp4」——实测 `storage.session` 里
`tab_<页面id>` 一直有货；② 「Edge flag 影响嗅探」——带不带 `--no-first-run/--no-default-browser-check`
各跑一次，两次都写入 storage ⇒ 那次空是偶发，与 flag 无关。另有一次判据 bug：
`wait_eval` 返回 JSON **字符串**，我却 `(ins or {}).get(...)` 当字典用 ⇒ AttributeError。

**仍未验**：popup 的像素证人（本轮读的是 DOM innerText，不是屏幕截图）；右键菜单发送路径；
`downloadWithOptions`（另存目录/画质选择卡片）；HLS/m3u8 与 ffmpeg 自愈；真视频站。

### 2026-10-04 00:0x（第 45 条）— 单轮 capstone 闭合 **+ 一条真实产品缺陷被它逼出来**

**(a) capstone 绿**（档 `docs/sniff-capstone-pass-1004.txt`）——同一轮里不再跨档案拼接：
```
[capstone] 任务进入『已完成』=True 行=['#1\t已完成\tdom.mp4\thttp://127.0.0.1:8252/media/dom.mp4']
[capstone] 本轮落盘的 dom.mp4=1 个（本轮新建）；.idmtmp 残留=0
[capstone] size 200012 == 源 200012 : True
[capstone] sha256 前16 267989ec01812b90 == 源 267989ec01812b90 : True
[CAPSTONE] 单轮闭合：网页识别 → popup → 点击 → host → GUI → engine → disk 且 size/SHA256 与服务器源一致
```
⇒ 现在可以严格写：**网页识别 → popup → Native Messaging → GUI → HTTP engine → disk 已完整观测级打通**。
两处取证纪律值得记：① 源指纹是**当场向测试服务器取再算**，不是抄上一轮档案常量；
② "本轮新落的"必须在**点击之前**采 mtime 基线——晚一步采会把本轮自己写的文件当成"点击前就在"，
我第一版就是这么假红的（缺项 [1,3,4]）。另纠正一个测具假设：GUI 默认下载目录走
`QStandardPaths::DownloadLocation`→注册表（`settings.cpp:16`），**不随 APPDATA 隔离**，
所以证人目录是 `%USERPROFILE%\Downloads`，不在 work 之内；在 work 里找当然永远找不到。

**(b) 真缺陷：MV3 service worker 冷启动窗口内的请求会被 webRequest 漏掉。**
连跑 7 遍 A 面，3 遍 popup 里没有 `network.mp4`。加了判别式探针后才分得清成因：
```
第2遍 storage.session 直读={"keys":0,"urls":[]}                       ← SW 还没醒，什么都没写
      归因探针（SW 内主动再 fetch 一次）={"status":200,"urls":["…network.mp4"]}  ← 立刻抓到
第3遍 storage.session 直读={"keys":1,"urls":["…favicon.ico"]}          ← favicon 抓到了
      归因探针=…{"urls":["…favicon.ico","…network.mp4"]}                ← 补一次就有
```
⇒ **不是 popup 显示竞态**（storage 里本来就没有），也不是 URL 正则问题（同一个 URL 补请求就命中）；
是**页面加载初期扩展的 service worker 尚未注册好监听**，那一段时间里的请求永久缺席。
`background.js:151` 的 `onBeforeRequest.addListener` 写在 SW 顶层，MV3 下 SW 空闲即被杀、
下次事件才重新拉起——重新拉起的时机就是丢消息的窗口。
**修法方向（未做，属产品改动，等点头）**：popup/content 打开时主动向 background 要一次
"当前页资源的补扫"（用 `chrome.webRequest.search` 不存在，只能靠 content script 上报 +
`performance.getEntriesByType('resource')` 回填），或按官方建议给 SW 保活/改用 `declarativeNetRequest`。
**本轮没有为了让 capstone 绿而放宽 A 面判据**：`[总结]` 现在把两者拆开报，
A 面偶发失败单独印「已知缺陷·复现一次」，不进 capstone 的通过条件。

**(c) 清理**：E2E 会在用户真实 `Downloads` 里留下 `dom.mp4`（这是取证路径本身）。
每轮跑前删、跑后核，确认大小与服务器 media 逐字节相等才认定是我的产物；未动用户其它文件。

### 2026-10-04 00:4x（第 46 条）— MV3 定性收口：**三个候选全部被否，真因是"扩展没在册"**；Gate 达成（有效轮 A 面缺失=0）

按主控批准走"先定性后修"。插桩三点（SW 求值时刻 / 回调入口含 tabId+正则命中 / `storage.session.set` 前后读数），
跑 10 遍，**跑完逐字还原**（`git status --porcelain browser-extension/` 空、`DIAG 残留=0`、与 HEAD 26796 字节相等）。

**第一轮 10 遍（档 `docs/mv3-sniff-setup-invalid-1004.txt`）**
```
[汇总] 轮数=10 A面缺失次数=3
[计数] 回调命中(cb)=7 其中 tabId<0=0 写入(write)=7
第2遍 [通道未建立] …  storage.session 直读={}
```
⇒ **三候选逐一被否**：(a) `tabId<0` 出现 **0 次**；(c) 写了没落住 **0 次**（7 个 write 全 `afterHasMp4=true`、`lastErr=null`）；
(b) "回调没被调用"看着成立，实为**量具假象**——那 3 遍连 SW target 都找不到。

**加前置断言后重跑 10 遍**（`tools/mv3_diag.py` 现在每轮先验"扩展真在被浏览器注册"：
探测 `chrome-extension://<key 现算 ID>/…` 的可连 target 并读 `getManifest().version`）：
```
[汇总] 轮数=10 有效轮=7 A面缺失=0 SETUP-INVALID=3
[Gate] 只看有效轮：A面缺失必须为 0；SETUP-INVALID 属实验未成立，不得计入产品缺陷
```
⇒ **Gate 达成**。所以 §7 第 45 条里那句「MV3 SW 冷启动窗口内请求被 webRequest 永久漏掉」**根因写错了**：
真实机制是**这 3 遍里扩展自身没起来**（不在册 ⇒ SW 从未注册 ⇒ 监听器从未存在 ⇒ `storage.session` 返回 `{}`），
不是"SW 起来了但错过事件"。旁证：成功遍的 `swT`=131–317ms，页面请求确实发生在 SW 求值之后，
MV3 的事件分发保证是生效的。**同时撤回我上一轮的措辞与主控转述的"页面在监听器注册之前加载"**——两者都不成立。

**没有动产品代码**：修 `background.js` 治不了"扩展没被浏览器加载"，那是测具/环境前置条件。
content script 回填这条修复路线因此**降级为待验证假设**（它针对的病还没被证明存在）。

**失败成段出现（第 2/3/4 连续三遍）而非均匀散布** ⇒ 指向一次性环境状态（新 profile 首次启动时 Edge
扩展注册尚未就绪）。这一条只到**强推断**：要证它需要"同一 profile 连续复用 vs 每轮新 profile"各跑 10 遍对照，本轮没做。

**前置断言自己也被验过有牙**：把探测 ID 换成不存在的 32 位号 ⇒ 输出 `A面=SETUP-INVALID`
（既不是"命中"也不是"缺失"），变异体用完逐字节还原。否则这条断言只是装饰。

### 2026-10-04 12:1x（第 47 条）— SETUP-INVALID 达成 + HLS 前三步：**降级路径诚实，但「FFmpeg 自愈」在任务路径上根本没被触发**

**(a) MV3 前置断言收口**（`tools/mv3_diag.py`，档 `docs/mv3-sniff-setup-invalid-1004.txt`）
每轮先验"扩展真在被浏览器注册"，再跑 10 遍：
```
[汇总] 轮数=10 有效轮=7 A面缺失=0 SETUP-INVALID=3
[Gate] 只看有效轮：A面缺失必须为 0；SETUP-INVALID 属实验未成立，不得计入产品缺陷
```
⇒ **Gate 达成**。三候选全否：`tabId<0` 出现 0 次、写了没落住 0 次、"回调没调用"是量具假象。
真机制是**那 3 遍扩展自身没在册**。§7 第 45 条的"冷启动漏检"根因作废，content script 回填降级为待验证假设。
前置断言自己也被验过有牙：把探测 ID 换成不存在的号 ⇒ 输出 `SETUP-INVALID`（不是命中也不是缺失）。

**(b) HLS fixture 与识别（第 1–2 步，全本机）** —— `disk_e2e_server.py::build_hls()`
生成 `master.m3u8 → media.m3u8 → seg0/1/2.ts`（伪 TS，0x47 开头、确定性字节）。分片用 `chr(10)` 拼行尾，
因为字面 `\n` 在本会话里被 shell/转义层吃掉三次。
```
[网页] DOM 状态={"videos":1,"src":"http://127.0.0.1:8272/hls/master.m3u8"}
[popup] 行=master.m3u8 …· M3U8 || seg2.ts …· TS || seg1.ts …· TS || seg0.ts …· TS || favicon.ico …· ICO
[判据·第2步] popup 列出 master.m3u8 = True
[扩展] storage.session=["…master.m3u8[video]","…seg2.ts[video]","…seg1.ts[video]","…seg0.ts[video]","…favicon.ico[file]"]
```
⇒ **扩展识别 .m3u8 成立**（DOM 通道），且 `.ts` 也一并被抓、`favicon.ico` 归到 `[file]` 而非视频。

**(c) 第 3–4 步的真实答案：不是"自愈成功"，也不是"失败给具名错误"，而是"绕开自愈走降级"**
GUI 数据根用 `IDM_DATA_DIR` 隔离，前置断言确认 `ffmpeg.exe 存在=False ｜ PATH 上的 ffmpeg=无`，然后真点击发送：
```
[GUI] 任务终态行='#2000000\t已完成\tmaster.m3u8\thttp://127.0.0.1:8272/hls/master.m3u8'
[内容核对] 期望 22443 字节 ｜实际 22443 字节 ｜逐字节相等=True
[磁盘] ffmpeg 是否被准备出来=False（dataDir 内）
[HLS 日志链] HlsDownloader 选定变体（带宽 800000）→ media.m3u8
             HlsDownloader 解析到 3 个分片（加密=0，时长≈6s）
             HlsDownloader 拼接完成: C:/Users/yyyy/Downloads/master.m3u8.ts
```
对照源码 `hls_downloader.cpp:293-298` 的三分支——`isFfmpegAvailable()`→ffmpeg 出 `.mp4`；
`m_encrypted` 且无 ffmpeg→`fetchKey()`；否则→`startManualDownload()`+`concatSegments()` 出 `.ts`（`:507`）。
本轮走的是第三条，产物逐字节等于 seg0+seg1+seg2 ⇒ **降级路径诚实、没有伪成功**。

**但这就是缺口本身**：`MainWindow` 只在**构造时**做一次 ffmpeg 自愈检查
（`main_window.cpp:335-340`，`autoDownloadFfmpeg` 默认 true，`settings.cpp:49`），
而 `HlsDownloader::beginDownload()` **不看这个设置、也不调 `ensureAvailable()`**——它只问"现在有没有"，
没有就静默降级成 `.ts`。后果有两层：
① 用户装了扩展、首启时那次下载又没成（我这三轮里 `ffmpeg/` 目录**从未被创建**，而同位置的
   aria2c 自愈成功打了「已就绪（开箱即用）」），此后任何 HLS 任务都只会拿到 `.ts`，
   **没有任何一次会再尝试准备 ffmpeg**；
② 该次自愈传的是 `warnOnError=false`（`main_window.cpp:337`），失败分支只 `cb(false)`、
   既不弹框也不写日志 ⇒ 三轮 GUI 日志里 ffmpeg 相关行数 = **0**。
   对比同函数里的 aria2 分支成功时有 log、ffmpeg 分支失败时什么都不会留。
⇒ 这是**可观测性 + 自愈触发时机**两条真缺陷，不是我先前一度想写的"产品没做 HLS"。

**我在这一步读错过一次并当场改正**：第一版 VERDICT 见到"有产物 + 已完成"就印了
「自愈成功面：ffmpeg 被准备、产物落盘」。错在两处——产物其实是 `.ts` 拼接而非 ffmpeg 合并的 `.mp4`，
且 `dataDir` 下根本没有 `ffmpeg.exe`。改后的判据要求**三方同时成立**才许说自愈：
终态已完成 ∧ 产物逐字节 == 分片拼接 ∧ `dataDir/ffmpeg/ffmpeg.exe` 存在；缺第三项就只能写"降级"。

**(d) 隔离边界要说清**：`FfmpegBackend::FFMPEG_ZIP_URL` 是 gyan.dev 的公网 zip
（`ffmpeg_backend.cpp:21-22`）。所以"完全不碰公网"与"验证自愈成功路径"**不能同时成立**——
第 5 步（准备成功后验产物）必须在允许公网的前提下做，或改用本地 file:// 假源注入。
本轮**没有**为了让某一面绿而去联网下载 ffmpeg。

**仍未验**：加密 HLS（`fetchKey` 分支）、真 MPEG-TS 拼接后的可播放性、`.mp4` 转封装路径、
公网下载下的自愈成败、真实视频站。A 面 content script 回填按主控排序仍排在 HLS 之后。

### 2026-10-04 12:4x（第 48 条）— 步骤 5：AES-128 加密 HLS **fetchKey 分支承重**（含一条防自欺对照）

fixture 由 `disk_e2e_server.py::build_enc_hls()` 本机生成，三条设计决定都是为了让结论不可伪造：
· 明文分片长度取 **16 的整数倍**（960B），否则产品会走「跳过解密」分支（`hls_downloader.cpp:483`），测不到解密；
· **故意不写 `IV=`** ⇒ 强制走产品最容易写错的那条：序号大端作 IV（`segmentIv()`，`:402-413`）；
· 期望值用 `cryptography` 库现算 ⇒ 与产品里那份手写 AES 是**两套实现**，一致才算证据。

**读数（档 `docs/hls-enc-step5-pass-1004.txt`）**
```
[popup] 行=emaster.m3u8 …· M3U8 || eseg0.ts …· 960 B · TS      [判据·识别] = True
[GUI] 加密任务终态行='#2000001\t已完成\temaster.m3u8\thttp://127.0.0.1:8272/hls/emaster.m3u8'
[GUI] "HlsDownloader 拉取 AES-128 密钥: http://127.0.0.1:8272/hls/key.bin"
      "HlsDownloader 已获取 16 字节密钥，开始纯 C++ AES-128 解密下载"
[内容核对] 实际 size=2880 ｜逐字节等于期望明文=True
```
⇒ **无 ffmpeg 时产品仍能正确下载并解密 AES-128 HLS**：`fetchKey()`→长度校验 16B→CBC 解密→拼接，全程有具名日志。

**防自欺对照（这条比结论本身重要）**：若期望明文恰好等于密文，"解密成功"就是假的。实测——
```
期望明文 sha12 = 3bd318be82d3 ｜密文拼接 sha12 = 60b162780d70 ｜两者相同 = False
用错 IV（小端）再加密 ⇒ 结果 ≠ 期望明文
```
⇒ 产物既不是原样落盘的密文，也对 IV 规则敏感 ⇒ 这条断言真的有牙。

**(f) 待修项登记（主控已认可为产品缺口，本轮不修、不阻塞）**
`ensureAvailable(warnOnError=false)` 在失败时既不弹框也不写日志；且 `beginDownload()` 不看
`autoDownloadFfmpeg`、不调 `ensureAvailable()`，自愈只在 `main_window.cpp:335-340` 构造时查一次
⇒ 首启那次没成就永不重试，用户以为有自愈实则没有。**记录为独立待修项。**

**步骤 6（真实视频站）没有执行**：它必然走公网第三方站点，与本项目一直遵守的隔离策略冲突，
且我没有得到"访问外部站点"的授权。停在这里等指令，不擅自跑。

### 2026-10-04 13:2x（第 49 条）— 步骤 B：真 MPEG-TS 载荷 ⇒ **产品 concat 后仍是合法 TS**（结构层），播放性明确不声称

零公网。`tools/ts_fixture.py` 自己生成真 TS（PAT/PMT/H.262 流、PES 跨包、每片恰 60×188=11280 字节），
并自带一个**独立判读器** `audit_ts()`。为什么不能沿用伪 TS：伪 TS 只能证"字节搬运对"，
证不了"拼接点之后还是合法流"——TS 的 PES 可以跨包，所以只要任一分片长度不是 188 的整数倍，
接缝之后的 sync 全部错位；这是个真实失败模式，必须能被测出来。

**读数（档 `docs/hls-realts-stepB-pass-1004.txt`）**
```
[popup] 行=tmaster.m3u8 …· M3U8 || tseg2.ts …· 11 KB · TS || tseg1.ts … || tseg0.ts …
[GUI] 真TS任务终态行='#2000002\t已完成\ttmaster.m3u8\thttp://127.0.0.1:8272/hls/tmaster.m3u8'
[期望] size=33840 sha256 前16=328cc750e4c51626（fixture 侧独立算）
[字节核对] 产物 == 三段真TS拼接 = True
[结构判读] packets=180 residual=0 sync_ok=True PAT=True PMT=True vidPID=256 PCR跨度=4.0s errors=无
```
⇒ 拼接路径在真 TS 上正确：包边界对齐、PSI 可解析、视频 PID 提取到 256、PCR 跨度 4.0s
与 `#EXTINF 2.0×3` 声明一致。

**判读器先自证再当证人**（本会话反复踩的坑，这次前置做掉）：
`python tools/ts_fixture.py --selftest` ⇒ 正例通过、两个负例（改坏某包 sync / 截掉 3 字节）都失败。
再加一条针对本轮问题的反例：**中间分片少 5 字节** ⇒
```
对照：三片对齐拼接        packets=180 residual=0   sync_ok=True  PCR跨度=4.0s      errors=无
注入：中间片少 5 字节     packets=179 residual=183 sync_ok=False PCR跨度=64776.965s errors=57 包 sync/afc 异常
```
⇒ 判读器确实能区分"接缝对齐"与"接缝错位"，所以上面那条 VERDICT 不是空断言。

**明确不声称的部分**：本机没有 ffprobe/ffmpeg/解码库，且不装软件 ⇒ **"能不能真的播放"这一条不能由这套工具说**。
这里证的是更窄的结构性合法（包对齐 + PSI 可解析 + PCR 单调）。要补播放性只有两条路：
用户手动用播放器打开一次，或授权引入一个本地解码通道。

**过程中我自己写坏的三处（都已修，逐条记）**：
① TS header 位序错（AFC 写在 bit0-1 而非 bit4-5）⇒ 所有包 afc=0，正例全红；
② PMT 判读漏了 `pointer_field` 那一层偏移，且字段索引按 section 起点重算错过一次 ⇒ `vidPID` 恒 None
   ——典型的"看起来能跑但字段全空"的半坏自检；
③ 服务器自报行加了 `ts_sha256/ts_size` 之后，旧代码 `split("enc_size=")[1]` 没接 `.split()[0]`
   ⇒ 把后续字段整串吃进 int() 崩掉。已统一成 `field(name, cast)` 取值。

### 2026-10-04 13:5x（第 50 条）— A 面修复前的重新评估：**「content script 回填」治不了第 49 条定性的那个病 ⇒ 本轮不动产品代码**

主控要求"先确认这条病的真实性再决定动手"。评估结论是**不该按原方案修**，两条独立证据：

**① 被针对的场景已被证伪。** 第 46 条定性：那 3 遍不是"扩展在册但 SW 错过事件"，而是
**扩展自身没在册**（SW target 不存在、`storage.session` 返回 `{}`、连 `[DIAG sw-eval]` 都收不到）。
content script 是**由扩展注入**的 ⇒ 扩展没在册时 content script 同样不存在，回填无从运行。
即：这条修法对已观测到的失败模式**结构上无效**。

**② 回填能覆盖的那半个场景，价值也被高估了。** 单独测页面侧可见性（故意不加载扩展）：
```
无扩展时页面侧可见的资源 = {"entries":["…media/network.mp4","…document.txt","…favicon.ico"],"netHits":1}
```
⇒ `performance.getEntriesByType('resource')` **确实看得到** mp4 —— 若真有"SW 错过了而 content.js 在跑"的时刻，
回填是能补上的。但它补不了两件事：(a) 跨域资源在 resource timing 里只有 URL、拿不到 `Content-Type`
与 `Content-Length`，而这正是 `onHeadersReceived` 那条通道存在的理由（URL 无扩展名的流媒体）；
(b) 它救不了上面那个真实发生的失败模式。

**③ 所以"A 面偶发缺失"现在是一条未闭合的观察，不是一个已知缺陷。** 剩下的解释空间是
"新 profile 首次启动时 Edge 尚未完成扩展注册"（第 46 条记为强推断）。要把它升级成产品缺陷，
需要的是复现条件而不是补丁。下一步该做的是这条判别实验：
**同一 profile 连续复用 vs 每轮新建 profile，各跑 10 遍** —— 若只有新建面失败，则是自动化时序问题
（浏览器安装期竞态），产品不需要改；若两种都失败，才回头查扩展注册链路。

**没有动 `src/` 也没有动 `browser-extension/` 一行代码**（`git diff --stat src/` = 0、扩展源码与 HEAD 逐字节相等）。
按 piclist §21「任何高级技术必须通过进入主线审查」的第 5 问——"不做它会怎样"：不做回填，
已观测到的失败照旧；做了回填，已观测到的失败照旧。故不入主线。

### 2026-10-04 12:4x（第 51 条）— A 面判别实验：**结论推翻第 46/50 条，病确实在 webRequest 投递**；同时撤回我刚才那句"扩展没在册"

`tools/a_profile_discriminate.py`，零公网、不改产品代码。两面各 10 遍：
```
[汇总·reuse] 轮数=10 SETUP-INVALID=0 有效轮里A面缺失=1     （第 1 遍缺，2~10 全命中）
[汇总·fresh] 轮数=10 SETUP-INVALID=0 有效轮里A面缺失=10    （每遍新建 profile + 重起浏览器）
```
⇒ **profile/首启形状是决定变量**，差异完全干净：单实例复用几乎必中，每轮新起必不中。

**两条必须一起读的更正：**
① **`SETUP-INVALID` 两面的都是 0 ⇒ 第 46 条那句"那 3 遍扩展自身没在册"是错的。**
   当时的判据是"`/json` 里找不到 SW target"，而本轮实测同一形态下 `chrome.runtime.getManifest()` 读得到
   （`ver=0.3.0`）、`tabs.query` 也看得见页面 tab ⇒ **扩展在册**。所以那次 INVALID 是我探测方式不稳
   （依赖 `/json` 是否恰好列出 SW），不是浏览器没注册。§7 第 50 条据此建立的"回填结构上无效"论证
   **前提失效**，一并撤回。
② 但第 50 条的另一半仍然成立且更硬了：单独探针显示 fresh 形态下
   `netHits=1`（请求真发生）、tabId 正常 ≥0、`chrome.webRequest` API 存在，
   而 `storage.session` 的 keys = `[]` ⇒ **webRequest 回调在"新 profile 首次导航"时不被投递**。
   同一浏览器里 DOM 通道（content.js → `reportMedia`）是能写进 storage 的
   （B 面历轮一直命中），所以这不是"扩展没工作"，是**一条通道哑、另一条通**。

**于是 A 面缺陷被精确定位为**：`background.js:151` 的 `onBeforeRequest` 监听在扩展刚注册的第一个会话里
收不到事件（MV3 注册时序），而 URL 无扩展名的流媒体只能靠这条通道（`onHeadersReceived` 同理）。
⇒ content script 回填**这次是真的对症**：它走的正是那条能通的通道，能把首启窗口的漏检补回来。
第 50 条的否决要改判：**方案对，理由错**。

**边界不变**：仍未动 `src/` 与 `browser-extension/`（`git status` 两侧皆空）。
按 §37 停止条件，本阶段到"定性完成"为止——修复本身是产品改动，等指令再做，且修完必须用
本脚本复跑两面（fresh 面从 10/10 缺失变成 0 才算治好）。

**这轮我自己的错**：把"探测不到 SW target"当成"扩展没在册"这个产品事实写了整整两轮，
还基于它否掉了一个本来正确的修复方案。教训形状同 §7 第 46 条：**判据要用最直接的证人**
（能不能读到 `getManifest()`），不要用"某个 target 有没有出现在列表里"这种依赖枚举时机的间接证据。

**复算性**：两面各跑了两遍（档 `docs/a-discrim-{reuse,fresh}-10.txt` 是第二次），结果一致——
reuse 缺失 1/10、fresh 缺失 10/10。第一次跑的汇总数字与第二次逐字相同，所以这不是单次噪声。
（顺记一次自己的顺序错误：我在写完整 §7 第 51 条**之前**就 `rm -rf` 了实验目录，
导致要归档时发现原始读数已丢，只能重跑两面取回 ⇒ 落盘顺序应是"先归档、再清理"。）

### 2026-10-04 13:1x（第 52 条）— A 面修复落地：**content script resource-timing 回填**，fresh 面 6/6 → 0 缺失

先让验收门红（`tools/a_gate.py`，fresh 面）：
```
[A-GATE] 有效轮=6 A面缺失=6 SETUP-INVALID=0 退码=1      ← 修复前原文
```
改 `browser-extension/content.js`：新增 `scanResourceTiming()`，把"页面自己发起过、DOM 里没有对应元素"
的媒体请求从 `performance.getEntriesByType('resource')` 补报进同一条 `reportMedia` 通道；
并在首扫之后补两次延迟重扫（1.5s / 4s）——媒体 fetch 常在首扫之后发生且**不动 DOM**，
没有这两次就仍然漏。修复后：
```
[A-GATE] 有效轮=6 A面缺失=0 SETUP-INVALID=0 退码=0
```

**过程中我的第一版修复引入了一条真噪音（被自己的另一面当场抓到）**：HLS 那几轮的
`勾选 emaster={"ok":false,"why":"没有 emaster 行"}` —— 回填把 `emedia.m3u8`、`key.bin`、`seg*.ts`
这些**流的内部构件**列进了资源列表，而顶层 `emaster.m3u8` 是直接导航的文档、不进 resource timing，
于是列表里只剩噪音。⇒ 加过滤：排除 `.m3u8/.mpd/.key/.m3u`、`key.bin|/key/|?key=`、以及 `.ts/.m2ts`
（`.ts` 按 HLS 分片处理，整条流交给桌面端 HLS 通道，不让用户下一堆碎片）。修完三面复算全绿。

**没被这条修复覆盖的半个场景（明写，不含糊）**：URL 无扩展名的流媒体仍然补不到——
resource timing 没有响应头，拿不到 `Content-Type`/`Content-Length`，那是 `onHeadersReceived` 独有的能力。
所以回填治的是"首启窗口内可凭扩展名识别的资源"，不是全部嗅探。

**顺带一个净收益**：回填带 `transferSize` ⇒ 列表里的体积不再是 `-1`。
实测同一 URL：修复前 `network.mp4 …MP4`（无大小），修复后 `network.mp4 · 196 KB · MP4`。

回归全套（都跑过）：sniff A/B/阴性 + 单轮 capstone（size/SHA256 与服务器源一致）全绿、
HLS 未加密降级 + AES-128 `fetchKey` 承重 + 真 TS 结构合法 全绿、七道文本门 rc=0。
`src/` 一行未动（改动只在 `browser-extension/content.js` 与 `tools/`）。

### 2026-10-05 05:4x（第 53 条）— 真站 HLS E2E（240p 变体）：**三方同立拿到完整产物**，并钉死「产品没有画质选择入口」这条真实缺陷

用户选定的公网教学流 `test-streams.mux.dev/x36xhzz` 是自适应 master（8 档 240p~1080p）。
上一轮定性「停滞＝吞吐非挂起」后，本轮按授权用 **240p 变体**重跑真站端到端，拿完整产物 + SHA256。

下单走产品自己的命名管道（`command=add`），但 URL 用的是**子播放列表**而非 master——理由不是偷懒，
是一条被源码证实的产品缺陷（见下）。三方同立才算成功：
```
[层①] 拼接完成日志=True ｜终态=#2000000 已完成
[层②] 工作目录已清空=True   （concatSegments 自己删了 idm_hls_2000000）
[层③] 逐字节一致=True       （独立 HTTPS 重取 64 片拼接 == 产物）
[VERDICT] 三方同立：真站 HLS 产物已验证
```
读数：产物 `193039199_mp4_h264_aac_ld_7.ts` **22,418,060 B**，
SHA256 `dca82b4374ee198b5a605dc03b0924152f6122e594df577a3e9ce8b63a9db0d3`；
下单→终态 **110.3s**（≈203 KB/s）；路径＝无 ffmpeg 的纯 C++ 手动下载
（`startManualDownload → concatSegments`，输出 `.ts`）。前置断言 dataDir/build/PATH 三处 ffmpeg 均无。

**独立重取的哈希不是孤证**：额外加了两道前缀/后缀对照——首片 272412 B 逐字节等于产物开头、
末片 100580 B 逐字节等于产物结尾 ⇒ 排除"顺序错位也能凑出同长"。PMT 解析确认产物是合法单 program TS
（program 1 → H.264 PID 258 + AAC PID 257），与我手搓扫描器先前的乱码读数无关。

**这一轮逼出的真实产品缺陷（不是测具问题）**：`parseMasterPlaylist`（`hls_downloader.cpp:199-207`）
对自适应流**无条件挑 BANDWIDTH 最大**，而 `m_forcedVariant` 只从 `req.extra["hlsVariant"]` 读（`:73`），
全仓没有任何调用点写这个 key（grep：只有 `task_controller.cpp:169/:226` 写 `"format"`，且那是 yt-dlp 的 `-f` 串）
⇒ **产品根本没有画质选择入口**。后果：普通用户从扩展点一条 master.m3u8，会被静默拉去下最高档
（此流 1080p ≈ 570 MB @ ~36 KB/s ≈ 85 分钟），且界面上既没有画质可选、也没有 ETA。
本轮为了拿"完整产物"只能由测试夹具直接指定 240p 子列表——这恰好是这个缺陷的反证。

**两条量具自伤（当场改正，留档）**：
1. 第一版探针把 `gui_ipc("list")` 回执的 `message` **第一行**当任务行判终态，但那行是表头
   「当前 N 个任务」⇒ 状态词永远匹配不到，整轮空转到上限（下载其实 110s 就完了）。改成先按
   文件名过滤出真正的任务行再判。
2. 手搓的 TS PAT/PMT 字节扫描器连续几轮偏移算错（先 `pid` 取在 byte4 而非 byte1-2、
   再把 `program_number` 读在 entry 的第 3-4 字节而非第 1-2 字节），一度印出 300+ 个假 PID 的乱码分布。
   ⇒ 临时手搓解析器不可信；承重判据始终是"独立重取逐字节全等"，结构核对以正确偏移的单次解析为准。

仍然没被这条覆盖的：master 自动选最佳档的**修复**（要加画质入参或 UI 选择，属产品决策，待批准）；
`.ts` 分片仍经 webRequest 通道进列表、子播放列表被标成 `[audio]`（第 52 条遗留的两条登记项，本轮未动）。
`src/` 一行未动（改动只在 `tools/hls_real240.py` 与本文档）。

### 2026-10-05 06:3x（第 54 条）— webRequest 分片抑制落地（缺陷 1 真修 + 端到端证明）；缺陷 2「子播放列表标 [audio]」经核查**不存在**，按证据撤回其描述机制

主控下发两条待修缺陷。逐条先读代码、写能红的测试，再决定动不动产品逻辑。

**缺陷 1（`.ts` 分片经 webRequest 进资源列表）＝真缺陷，已修。**
一手证据：`docs/hls-e2e-1004.txt` 里 popup 行是 `master.m3u8 || seg2.ts || seg1.ts || seg0.ts || favicon`，
storage.session 印出 `seg*.ts[video]`——分片确实作为独立视频列出。根因在 `background.js`：
FILE_PATTERNS 含 `.ts` ⇒ onBeforeRequest/onHeadersReceived 把每个分片当一条资源；而 `.m3u8` **不在** FILE_PATTERNS，
播放列表只由 content script 扫 DOM 后经 reportMedia 进来（所以 m3u8 与 .ts 来自两个通道）。
修法＝一旦某标签页出现过 HLS 播放列表，就把该页裸 `.ts` 从可下载列表去掉；**不碰 INTERCEPT_PATTERNS**
⇒ 用户主动点一个 .ts 链接仍被 IDM 接管，零能力损失。

**这条修复被真实浏览器逼出两次升级（不是纸上加层）：**
1. 只做"reportMedia 命中后打 Set 标志"不够——步骤 B 是**直接导航**进 `tmaster.m3u8`，没有 DOM 可扫、
   reportMedia 不会来标 tab ⇒ 补一条 main_frame 命中播放列表时就地标记。
2. 仍漏：CDP 挂到真 service worker 实测事件顺序为
   `main_frame tmaster.m3u8 → media tmaster.m3u8 → media tmedia.m3u8 → media tseg0/1/2.ts`，
   但冷启动 SW 下分片的 addResource 会先于标记落库（判别式实测：预热+预标时 `hlsSeenTabs=true`、
   seg0.ts 被吞、movie.mp4 保留 ⇒ 逻辑对，纯粹是到达顺序竞态）。最终正解＝**双向收敛**：
   加 .ts 时若同源已有播放列表则丢弃；加播放列表时反向把同源、已入库的裸 .ts 清出去。
   去重与顺序无关 ⇒ 无论谁先到，列表终态恒为"有 m3u8、无其分片"。

新门 `tools/sniff_rules_test.mjs`（node:vm 加载**真实** background.js、抓它注册的监听回调喂合成 details）：
9 条断言含 D1/D1b/D1c 三面 + 三条阴性对照（普通 mp4 仍列、无 m3u8 页的 .ts 仍列、响应头通道同规）+ D2A。
变异都咬过：摘反向清理 ⇒ D1c 残留 0→3；摘正向标志 ⇒ D1b 泄漏 0→3；还原后 PASS=12 FAIL=0。
端到端复算（真 Edge + 真扩展 + 本机 HLS fixture）三步全净：step2 `master.m3u8[video]+favicon`、
step5 `emaster.m3u8`（连 key.bin 也一并不再淹列表）、stepB `tmaster.m3u8`（此前 3× tseg.ts 全消）；
三条下载 VERDICT 不变（降级诚实 / fetchKey 承重 / 真 TS concat 合法）⇒ 改动只影响列表、不影响下载。

**夹具自伤一条（留档）**：内存 storage 桩最初同步 resolve，令并发 addResource 的读-改-写互相覆盖
（假丢更新，报成"两条 m3u8 只存一条"）。真 chrome.storage.session 对同扩展串行处理 ⇒ 改成
per-tab 事务队列 + 宏任务 settle 后忠实。教训：**测异步读改写的夹具必须复刻被测平台的串行化保证，
否则会把桩的竞态误报成产品的缺陷**（本轮差点据此去给产品加锁）。

**缺陷 2（子播放列表被标 [audio]）＝按现有证据不成立，未据此改产品代码。**
穷查所有定性路径：① content.js `scanMediaElements` 把 `<video>`/`<source>` 一律标 `'video'`；
② background `inferType('.m3u8')='file'`，且 `.m3u8` 不走 webRequest（不在 FILE_PATTERNS）；
③ onHeadersReceived 的 audio 分支要求 Content-Type 以 `audio/` 开头，而 mpegurl 是
`application/vnd.apple.mpegurl`（既非 video/* 也非 audio/*，且不在 MEDIA_MIME_TYPES）⇒ 走不到 audio；
④ popup.js 只把未知 type 归 `'file'`，从不归 audio。仓内**每一处**观测（hls-e2e/enc/realts/240p）都把
`.m3u8` 印成 `[video]`，全仓 grep `.m3u8[audio]` = 0 命中。⇒ 主控描述的机制在本代码里不可达；
若真站确有此现象需带具体 URL 复现，本轮不为一个证不出来的假设改分类逻辑（见 [[verify-dont-trust-docs]]：
查不到写"已查，不是缺陷"）。D2A/D2B 两条测试把"永不为 audio"钉成回归护栏（D2B 记录 mpegurl 未被
webRequest 抓到这一事实，将来若有人扩 MIME 集合让它进来，type 必须是 video）。

`src/` 一行未动（改动只在 `browser-extension/background.js`、`tools/sniff_rules_test.mjs`、文档与 NOTICE）。
NOTICE 复签 `408=398+10`（新增 tools/sniff_rules_test.mjs、background.js 字节变化）。

### 2026-10-05 20:1x（第 55 条）— HLS 画质选择入口接通：五层链 + 生产链探针（含一次"假绿门"自我驳回）

批准方案 A 后落地。链路＝content.js capture 卡片 → background hlsVariant → host `req["hls_variant"]`
→ GUI IpcServer add 读字段 → MainWindow::addTaskFromUrl → TaskController::internalAddTask(…, hlsVariant)
→ `req.extra["hlsVariant"]` → HlsDownloader 既有 forcedVariant 匹配（该匹配逻辑本来就在，缺的只是没人喂值）。
默认行为不变：**不选＝最高档**（V1/V3 钉住）；host 拉 master 失败/超时(8s)/无 STREAM-INF ⇒ success=false
⇒ 下拉只剩「最高画质（默认）」+「画质不可用」禁用项，下载能力零降级（E1/E2/E3 三面实测）。

**本轮最贵的一条是方法级的，必须留档**：第一版探针直接 `new HlsDownloader`、自己往 req.extra 塞
`hlsVariant` —— 它绕过了本轮要接的全部四层线。摘掉产品里那行注入，探针照样全绿（MUT_RC=0），
这扇门当时是**假的**。改成走 `TaskController::internalAddTask(..., hlsVariant)` 生产入口后，
同一处变异立刻把 V2 打红（MUT_RC=1，V1/V3 仍绿＝判别式精确）。⇒ 规矩补一条：
**测"接线"的探针必须从接线起点进，不能从终点组件进；每扇新门在宣布成立前先做一次"摘产品行"变异。**

其它实测读数：
- 探针经生产链跑真任务时先崩了一次 `std::bad_function_call`——`m_setSpeed/m_setState` 在
  internalAddTask 里无条件调用（task_controller.cpp:158-159），MainWindow 注入而探针没注 ⇒ 补 noop sink。
- 端到端（真 GUI + 命名管道 `hls_variant:"246440"`）：产物逐字节 == lo 变体拼接（owner_lo=True）。
- **套件出现既有偶发**：IDM_ACTIVATE_PROBE 的 <500ms 轮次断言，带改动 3 跑 2 红、
  `git stash` 后基线代码同样 3 跑 1 红 ⇒ 与本轮无关，登记为待办（不是本轮引入，不许顺手掩盖计数）。
- freshness_gate 本轮如实报 DIRTY_INPUT×4（exe 比已提交输入新＝工作树里有未提交源码）。
  本轮没有再动 src/，commit 后复跑转绿（见下方终态读数）。
- 语义分叉防呆：`.m3u8` 的选择值走 `hlsVariant`（BANDWIDTH 串），非 m3u8 走 `format`（yt-dlp -f 串），
  content.js 按后缀分字段传——混用一个字段会把"未接线"变成"静默错配"。

改动文件：src/{app/ipc_server.{h,cpp}, app/task_controller.{h,cpp}, gui/main_window.{h,cpp},
host/idm_next_host.cpp}、browser-extension/{background.js,content.js}、CMakeLists.txt
（hls_variant_probe 挂在 IDM_BUILD_UI_SNAPSHOT 开关下）、tools/hls_variant_probe.cpp、
tools/disk_e2e_server.py（build_variant_master 夹具，hi/lo 分片路径互斥）。NOTICE 复签 `414=404+10`。
CLI 回退通道（host 未连上 GUI 时）不支持 hls_variant ⇒ 保持默认最高档，已写进代码注释与本条。

### 2026-10-05 20:4x（第 56 条）— P0：IpcServer 空转连接阻塞主线程修复（异步读阶段）+ E 段夹具（含一次"无效证人"自我驳回）

方案 A 批准时从代码里读出的产品缺陷，本轮修完。机制：`handleClient` 旧形状在 **GUI 线程**上
对每条新连接跑 `waitForReadyRead(100)`×2 起步的有界循环 ⇒ 一条"连上一个字节都不发"的客户端
（扩展 checkConnection 轮询、被杀的副实例、探针自己的空转连接）让服务端白占主线程 ≥200ms；
用户每点一次插件界面就卡一下。这条同时解释了 §7-38 的"量具常数"与登记过的 ACTIVATE 偶发红。

**修法**：读阶段拆成 `PendingRead` 状态机（readyRead/graceTimer/disconnected 信号驱动），
槽函数里不再有任何 sleep。保留的原意逐条对上：分片累积不变；"已收半条才继续等"由 graceTimer
（收到过东西才挂，500ms）承担；凑齐完整 JSON 即交付 `handleClientData`。回话/送达/drainStat
取证路径一行未动。管道名新增 `IDM_TEST_PIPE` 覆盖（仅测具；不设＝产品行为逐字不变）。

**过程中我犯了两条、都被实测当场驳回，留档防重犯：**
1. **第一版夹具的证人无效**：用"压在空转连接后面的真请求往返 <150ms"当判据。Windows 命名管道
   允许写先于 accept 完成（worker 线程 waitForConnected 返回前就 write+flush 是常态），字节常常
   在 newConnection 之前已可读 ⇒ 往返量不到那两轮 sleep，变异实测 17ms 照样绿。**假门。**
   正解＝直接量"这一轮本身"：探针自起私有 IpcServer（pid 管道），空转连接建立后连续泵事件 300ms
   取最长一轮。修复后=0ms；把 waitFor 塞回去=**204ms 红**（两面各留原文）。
2. **首检无字节就立即交付会吃掉真请求**：中间版本把"已连接且安静"当空转坐实 ⇒ ack=0/50，
   所有跨线程投递的请求被当哑巴连接处理。空转的正确证人是对端 **disconnected**（真请求不会关自己）。
3. 顺带两条既有事实：ACTIVATE 探针此前从不 show() 窗口（activate 的 showNormal 对隐藏窗等价首次
   show ⇒ ok3 量的不是幂等），本轮补 w.show()；探针服务改 pid 隔离管道前，同机残留实例会让它
   连到别人家服务（第一轮 ack=0/0 peakN=1 就是那个形状——归因走 CLI 复算，非猜测）。

**前后对比读数**（同一台机器、同一枚 fixture）：
| 面 | 修复前 | 修复后 |
|---|---|---|
| E 段：空转连接触发的那一轮 | ≥200ms（变异复现 204ms） | **0ms** |
| D 段：取尽自身峰值 | 210~213ms | **0ms** |
| D 段：IPC 占比 | ~12% | **0%** |
| C 段：连续 50 投 ack | 50/50 | 50/50（能力不降级） |
| B 段：放弃后往返 | 9~14ms | 14ms |

D 段单轮最长仍 ~1711ms（需 <500）⇒ 那是 MainWindow 定时器回调（§7-38 已消融归因、基线同现），
**IPC 已被证明不在其中**；这正是下一步 ② ACTIVATE C→B 的输入。

改动文件：src/app/ipc_server.{h,cpp}（异步读阶段 + handleClientData 公开为交付口）、
src/app/cli_forward.cpp（IDM_TEST_PIPE 覆盖）、tools/ui_snapshot.cpp（E 段夹具 + w.show() +
ACTIVATE 私有管道）。回归全绿：套件 19 模式 PASS=79 FAIL=0（比上轮 +1＝E 段）、引擎 180/0、
hls_variant_probe V0–V3、真 GUI E2E（add hls_variant=246440 ⇒ 产物==lo 拼接、help 正常）、
七道文本门将随本轮 NOTICE 复签后跑齐。证据档 docs/activate-e-idle-block-1005.txt。

### 2026-10-06 00:1x（第 57 条）— ACTIVATE C→B：D 段判据从「单轮最长 <500ms」换成「固定时长内超预算轮数 ≤1」，并当场驳回三条测具假读数

**做了什么**：① IpcServer 空转阻塞修完之后重采 D 段分布，把 B（阈值判据）从拍脑袋的 500ms
改成有来源的量；过程中先证明**旧门与旧归因都建立在测具自己的伪影上**。

**四条被实测驳回的假设（每条都改了代码，不是改了说法）**：
1. **`QThread::create(...)->start()` 在 Windows 上是同步的**，机器忙时它自己占掉 GUI 线程一整轮。
   判别式（同机、同泵法、独立小程序）：A 面每轮新建线程 worst=32ms ｜B 面预热一条复用 worst=0ms。
   ⇒ 干净面上"要么 0 个慢轮、要么 1~2 个 300/1700ms 慢轮"的双峰是**测具造的**。
   修法：对端线程提前起好、整个窗口保持连接、窗口内零线程起灭。
2. **"采满 N 轮就走"会把窗口缩到比产品任何节拍都短**。空闲一轮只花 0~1ms ⇒ 30 轮合计几十毫秒墙钟，
   flushTimer(100ms)/状态栏(250ms) 一次都来不及 fire ⇒ "非零轮=0"读的是我的采样区间。
   实测形状：墙钟 413ms、跑过 20 万轮、非零 0 轮。修法：**退出只看墙钟**（默认 3000ms），样本数只印不当闸口。
3. **收摊循环不许再泵事件**：`while(!idleClosed) processEvents+msleep(5)` 是在等一个"只有泵事件才会发生"的旗子，
   等于把几百毫秒到几十秒的事件循环记在账本之外（E 段一度因此量到 383~415ms 的无归属红）。
   修法：主线程置 abort 后直接 join；E 段改为**先停产品定时器、只取包含 newConnection 的那一整轮**。
4. **消融/bisect 的判据不能要求塌到 1/3**：窗口里并存多路占用时，摘掉一路只让它减半，
   于是每个候选都印"不是它"，而事实是"两个都在贡献"。修法：配对比较（停掉后 <min(两面未停)/2）＋第三面排漂移。

**顺带修掉的三个测具自身缺陷**：定时器普查原先只扫 qApp 对象树 ⇒ 恒印「0 个」（产品的都在 MainWindow 子树），
现改两路并集＋两侧各挂一枚已知 canary 自证扫描有眼；`IDM_TIMER_INDEX=N` 的编号吃 QSet 迭代序 ⇒ 跨进程不稳定
（有一次 idx1 直接段错误），现按 (间隔, 宿主类名) 排序；消融面 canary 不 stop 就 delete ⇒ 回调 msleep 中删对象必崩。

**新门的两面证据**（同一枚二进制 `ui_snapshot.exe sha12=db338bba50ca`）：
| 面 | 分布 | 超预算轮数 | 判据 |
|---|---|---|---|
| 干净（6 轮池化） | 非零轮绝大多数 1ms，每窗口 ≤2 发 ~300ms | 0~1 | PASS |
| 变异 IDM_ATTR_SLEEP=30 | p95≈44 max=45 | 0 | PASS（低于 T 不该响） |
| 变异 IDM_ATTR_SLEEP=60 | p95≈74~75 max=75 | 22~26 | **FAIL** |
| 变异 IDM_ACTIVATE_MUTATE（摘掉 ① 的修复） | max=163~533ms | 15~18 | **FAIL** |

T=50ms 的来源写在源码注释里：干净面非零轮常态是 1ms 量级（Windows 消息到达），
"产品让一轮转不动"这个形状只能在有负载的面标定——30ms 注入 p95≈45 不响、60ms 注入 p95≈75 响 ⇒ T 取中间 = 50ms；
K=1 允许一窗内一发无归属的系统抖动。**旧 500ms 作废**（那是照着"IPC 常数 216ms"拍的，量纲已不存在）。

**C 的判定达成**：D 段峰值不再是固定 216ms 常数，而是有真实分布（干净面 0~2 发/窗口、变异面 22~26 发/窗口）。

**仍然没验证的东西**：干净面那 1~2 发 ~300ms 的**归属仍未定**（普查显示窗口内有 4 个活动定时器，
但 bisect 逐个摘都不减半 ⇒ 多路并存或根本不是定时器）；`[census]` 行现在会明说"这一轮的 0 无人证得过"，
不许把它读成"产品空闲"。引擎自测本轮只跑了 179/1（唯一 FAIL 是 [35] 探针起点=0，属既有项、非本轮引入），
基线 180/0 未复算 ⇒ 不得引用为本轮通过。CI 仍从未在 runner 执行；像素证人/K2 仍在延后队列。

### 2026-10-06 03:4x（第 58 条）— ③ K2：判据第三次重写并**当场用两次变异证明它有牙**；引擎基线复算 180/0，并修掉连跑必坏的夹具

**先纠正一句我上一轮说错的话**：我在 §7-57 里写「唯一 FAIL 是既有项 [35] 探针起点=0」——那是抄来的旧读数。
本轮原样复算：**[35] 两条都是 PASS**，真正的红是别的东西，而且是我自己造出来的（见下面第三段）。

**一、K2 的三次判决（每次都改了代码，不是改了说法）**
- 第 1 版「两次 Tab 必须落在目录输入框」：实测链 `url→dir→浏览...→name` ⇒ 「浏览...」夹在两个输入框中间，
  Enter 永远落不到它身上 ⇒ 台账那句描述不成立，断言永红（红的是我写的期望）。
- 第 2 版「dir 在链上且紧跟 浏览...」：**本轮驳回**。全仓 grep `setTabOrder` = **0 命中**，
  NewTaskDialog 从未声明显式 Tab 序；Qt 默认 Tab 序＝控件创建顺序（new_task_dialog.cpp:56..68），
  所以那条不变式由**框架默认行为**满足，摘掉任何一行产品代码都不会让它红 ⇒ 没有牙的护栏。
  更糟的是它当时印 [PASS]：绿灯谎称"这行断言在保护键盘面"（半坏自检比没有更坏）。
- 第 3 版（本轮落地）：判据换成产品真正写下的契约 —— 按下 OK 必须走到 `onAccepted`，
  其末端 accept() 的对外可见副作用是 `dlg.result().url` 被填上（new_task_dialog.cpp:365..371）；
  再加 a) OK 是默认按钮、b) 「浏览...」不是默认按钮。**链形状降级为只印不判。**

**二、牙的两面证据**（`ui_snapshot.exe sha12=b97682106a69`，HEAD=`5fa781b` 上的工作树）
| 面 | 读数 | 退码 |
|---|---|---|
| 干净 | 确定=default&&auto=1 浏览!=default=1 onAccepted 真跑=1 | 0，[K2] PASS |
| M-A：注释掉 :121 `connect(accepted→onAccepted)` | 前两处仍 1，**onAccepted 真跑=0** | 1，[FAIL] |
| M-B：给 OK 加 `setAutoDefault(false)` | **default&&auto=0** 且 onAccepted 真跑=0 | 1，[FAIL] |

⚠ M-A 的第一次实现是**假牙**：我在旁边挂观察者数 `accepted` 信号——那是 QDialogButtonBox 自己发的，
摘掉产品接线照样数到 1，变异在和自己比。改成读产品自己的输出（result.url）之后才咬得住。
两次变异跑完都已还原（`git status --porcelain src/gui/new_task_dialog.cpp` 空）。

**三、引擎基线复算：180/0，以及一次必须记下来的错误归因**
带认证服务器（`tools/engine_selftest_server.py 18092 …`）连跑四轮全 180/0（`engine_selftest.exe sha12=c33e263e5703`）。
过程中撞到"同一枚二进制第一次 180/0、第二次 33~76 红"，我先后给出两个解释，**都被实测驳回**：
1. 「夹具拆除怕非空残留目录 ⇒ 补重试就好」：加了重试仍然第二次大面积红 ⇒ 假设不成立。
2. 「停笔后句柄未放 ⇒ 面 B 删除失败」：那条断言在红的那一轮里也是绿的 ⇒ 归因错对象。
真正的因是我自己的测具流程：自测**跑完会喊 `/_shutdown` 把服务收掉**（engine_selftest.c:3200 起，
为的是不给 CI 留待 kill 的进程），而我每轮都复用同一个已经死掉的服务器 ⇒ 从第一条 HEAD 探测起全红。
顺带修掉两处真实缺陷（都在测具侧）：
- `[13]` 的目录拆除用裸 `DeleteFileA` + 坏转义 `"%s\*"` / `"%s\%s"`（`\*`、`\%` 是未知转义，
  编译器一直警告）⇒ 改走 `utf8_delete`（含重试；删不动就改名 `.zombie` 让父目录立刻可摘）并修好转义。
- 面 B 造夹具的删除同样加重试。
⚠ 诚实标注：**这两处修复没有被变异证明**（我没有演示"摘掉它就红"），按台账规矩**不占功**；
它们只是让连跑不再需要人记得重起服务器。跑之前先起服务、或信任 exe 自己收摊，二者选一即可。

**四、门禁**：套件 19 模式 PASS=79 FAIL=0（日志 `%TEMP%/idm-next-probes/2026-10-05/suite-k2-20261006T073110Z`）；
build/freshness/host_manifest/live_roster/probe_roster/sniff(12/0) 全 rc=0；引擎 180/0。

**仍然没验证的东西**：K2 的 a)/b) 只被 M-B 一种形态证明过（换个别的破坏方式未必红）；
K1 仍是静态属性证人（offscreen 合成按键到不了快捷键系统，canary 四种发法累计 0）；
CI 从未在 runner 执行；像素证人受本机焦点约束；采集卡真浏览器 E2E 与画质下拉的真实点击面仍未跑。

---

### 2026-10-06 04:3x（第 59 条）— ④ capture 卡片**真浏览器 E2E**：13 条判据全绿，并当场承认「卡片里没有『浏览...』按钮」

**测的是什么形状的问题**：此前所有采集卡证人都是 offscreen 合成（`ui_snapshot` 直接构造 DOM 字符串 /
`tools/ext_shot*.py` 用注入的 stub 页面）。它们能证明"卡片存在、字段齐全"，**不能证明"用户点得到、
字打得进、提交真的走到 GUI"**。④ 的要求就是把这条链在真实环境里跑通。

**测试方法（驱动方式，逐项写实）**：
- 浏览器：**本机已装的 Microsoft Edge Edg/154.0.4258.53**，`--headless=new` 但**不是 mock**——
  真实渲染引擎、真实扩展宿主、真实 native messaging 通道。
- 驱动：**手搓的最小 CDP WebSocket 客户端**（`tools/capture_e2e.py` 内 `class Ws`，与 `nm_e2e.py` 同形），
  只用 `Target.createTarget` / `attachToTarget(flatten)` / `Runtime.evaluate(returnByValue,awaitPromise)` /
  **`Input.dispatchMouseEvent`（真鼠标事件，不是 `.click()`）** / `Input.insertText` / `Page.captureScreenshot`。
  **零新增依赖**（标准库 + 已有栈）。
- 扩展：真实解包加载 `browser-extension/`，ID 由 manifest `key` 推导 = `fmnehlhldcoknecihnlbbcmhpaaapmbp`。
- 主机：真实 `build/idm-next-host.exe`（sha12 `b2a6741688b8`）+ 真实命名管道 + 真实 GUI `idm-next.exe`
  （HEAD `2fbd8ad`；本轮三次运行的 app sha12 见下文 M-C 一节，逐档记录在案）。
  GUI 侧可达性由该脚本自身断言⑦⑧承担（toast + `IPC add 参数:` 行），不再另起 `nm_e2e.py`。
- 视频源：本地夹具 HTTP 服务（127.0.0.1:8251）发 `/hls/master.m3u8`，两条 variant
  BANDWIDTH=6239287（高）/246440（低），**分片内容按档位不同** ⇒ 落盘字节本身标识选了哪档。

**13 条判据（本轮全部为绿，读数档 `%TEMP%/idm-capture-e2e-261006-041940`）**：
| # | 判据 | 一手证据 |
|---|---|---|
| ① | 悬停后悬浮按钮出现 | `.idm-next-float-btn.show` |
| ② | **真鼠标点击**弹出卡片且含 name/dir/fmt | `[卡片] {"w":348,"h":241,...,"btns":["立即下载","加入队列","取消"]}` |
| ③ | 卡片完整落在视口内（不截断） | right 547 ≤ vw 746，bottom 251 ≤ vh 483 |
| ③b | 9 个部件两两不重叠、不越出卡片 | `重叠对=[] 越界=[]` |
| ④ | 画质下拉由**真 host** 填出两条 BANDWIDTH | `vals=["","6239287","246440"]` 文案含 `1080x1920 · 6.2 Mbps` / `96x540 · 0.2 Mbps` |
| ⑤ | 文件名输入框可聚焦且**真键入** | `activeElement="idm-cap-name"` → `value='clicked_name.bin'` |
| ⑥ | 低档位可选中 | select 后 value=246440 |
| ⑦ | 提交后成功回执且卡片自摘 | `toast={"text":"已添加下载: clicked_name.bin","cls":"… success show"} 卡片已关=True` |
| ⑧ | GUI 收到我选的档位 | 日志行 `IPC add 参数: … hlsVariant=246440` |
| ⑨ | GUI 收到我键入的文件名 | 同一行 `file=clicked_name.bin` |
| ⑩ | **反向对照**：不选画质时该字段必须为空 | 走产品自己的 `showCaptureDialogFor`，该行 `hlsVariant=` 空 |
| ⑪ | dir 字段端到端到达 GUI | 显式把保存目录打字进卡片，制品落在时间戳 logdir 而非 Downloads |
| ⑫ | 落盘字节 == 所选低档拼接、且 != 高档 | `文件=master.ts 大小=12288 sha256=7fd14f4740c6 等于低档=True 等于高档=False` |
| — | 自检：低/高档期望值不得相等 | `LO_EXPECT != HI_EXPECT` 打印为 True |

**为这条链新增的产品侧代码（唯一一处，7 行）**：`src/app/ipc_server.cpp` 的 `add` 分支在 `m_addTask(...)`
之后打印一行参数证人 `IPC add 参数: url=… file=… dir=… threads=… queue=… format=… hlsVariant=…`。
没有它，⑧⑨⑩⑪ 只能靠"下载结果长得像不像"间接推断 ⇒ 那是弱证人。兼容性核对：既有测具匹配的子串
`IPC 请求: add → 成功` 未改，套件照旧绿。

**变异 M-C（证明 ⑧ 有牙，不是恒真）——本轮三次运行，逐次带 app sha12**：
| 面 | app_exe sha12 | 判据结果 | 读数档 |
|---|---|---|---|
| 基线（源码原样） | `586b0fac4a6d` | 13 条全绿 | `%TEMP%/idm-capture-e2e-261006-043150` |
| **M-C**：`.arg(queue, format, hlsVar)` → `.arg(queue, QString(), QString())`（witness 的 format/hlsVariant 置空） | `63d42131c5fa` | **恰好一条红**：`⑧ GUI 收到的 hlsVariant == 我在下拉里选的低档位`，其余 12 条绿，rc=1 | `…/idm-capture-e2e-261006-043218` |
| 还原重编 | `591bbd2a374f` | 13 条全绿，rc=0 | `…/idm-capture-e2e-261006-043244` |

⚠ 这一节是**重跑的**：上一轮我口头说过"M-C 跑过、恰好一条红"，但当时没有留下对应读数档
（盘上只有全绿档与两条红的开发期失败档）⇒ 那句话按台账规矩属于"没证据的结论"，本轮补齐。
⑩ 的反向对照是同一判据的另一面：如果 witness 恒印同一个值，⑩ 会先红。

**必须诚实说出的三件事**：
1. **「浏览...」按钮不在这张卡片上。** 它是 `NewTaskDialog`（Ctrl+N 那个桌面对话框）的控件，
   已由 ③ 的 K2 两次变异（M-A 摘 accepted 接线 / M-B 关 autoDefault）证明可点、可开、handler 真跑。
   采集卡 DOM 里没有这个按钮 ⇒ 我没有为它编造通过项，也没有把它算作失败。
2. **`tools/ext_shot*` 那套 stub 测具证明不了提交**：它注入假页面、不发真消息到 host，
   所以历史上"卡片截图 OK"从来不包含 ⑦⑧⑫ 这一段。本轮起才有端到端证人。
3. **像素证人仍受本机焦点约束**：截图是 headless 渲染帧，不等于前台窗口实测；
   ③/③b 用的是几何读数（rect/viewport），不是视觉比对。

**门禁（本轮在 M-C 还原后的二进制上复跑，app sha12=`591bbd2a374f`）**：
- 套件 19 模式 PASS=79 FAIL=0 零断言=0（日志 `/tmp/idm-probes/ui-1006-042602`；跑在 `ui_snapshot.exe d5bd121f37b5` 上）。
- build_gate / host_manifest_gate / live_roster / probe_roster / a_gate(有效轮=6 A面缺失=0) / sniff_rules(12/0) 全 rc=0。
- freshness_gate 在 `97c51ec`（产品代码那笔）**落地后复跑 rc=0、不合格=0** ⇒ "产品 exe == 当前源码"这句话本轮成立。
- 引擎自测 **180/0**（带认证服务器 18092，`engine_selftest.exe a254689c17f5`，落盘 `/tmp/est_run1.log`）。
- NOTICE 由 `notice_verify.py` 自己的口径重签：`src/=945872`、`tools/=941109(51 个)`、`docs/=624301`，
  合计 `402 + 10 = 412`（本轮新增被跟踪文件只有 `tools/capture_e2e.py`）⇒ FAIL=0（⑤ 后重签：src=946490 tools=946543 docs=76/642939，合计 407+10=417），`notice_sha12=8efdf6b55662`；⑥ 后随盘上真值提交后按新跟踪集合再签：`e4f6167caa4c`（tools=52/961779、docs=82/657312、合计 414+10=424；⑥ 的 py 与六份变异日志已入库）。

**仍然没验证的东西**：卡片在**非默认窗口尺寸/缩放下**是否仍不截断（只量了 746×483 这一档）；
「取消」按钮与 ESC 的关闭语义没有判据；多标签页并发弹卡（同名去重、焦点归属）未测；
Edge 版本号升级后 CDP `Input.*` 行为是否不变；CI 从未在 runner 执行过这套 py（它只在本地跑，
且 `capture_e2e.py` **不在任何闸门名单里**——`.github/workflows/ci.yml` 里没有对它的调用，probe_roster 也不管 py）；
M-C 只演示了一种破坏形态（置空），换成语义错位（如档位取反）未必被同一判据抓到；
**真 GUI 与 offscreen 探针共用字面量产品管道名 `idm-next-ipc`**（`IDM_TEST_PIPE` 只在 ACTIVATE 面生效，
`capture_e2e.py` 起的 app 不设它）⇒ `IpcServer::start()` 的 `removeServer()` 会摘掉对方正在监听的 endpoint，
本轮三次 E2E 与套件是**串行**跑的所以没撞上；并行或残留实例下两侧都可能"连到别人家/没人监听"。这是已知形状，
不是本轮新发现（§7-57 记过半边），彻底解法（app 侧 pid 作用域管道）仍未做。

---

### 2026-10-06 05:0x（第 60 条）— ⑤ K1 canary 真实化：**"offscreen 到不了快捷键系统"是我量具用法的错，本轮当场纠正**

**问题**：K1 长期只有静态属性证人（动作存在 + context==Application），canary 四种发法累计恒 0。
台账原话把它归因成"平台限制"。要么产品在走一条没人到达的路径，要么量具瞎——本轮先分诊。

**一、根因（Phase 1，判别式实验，不是猜）**：独立小程序 `k1_canary_scratch.cpp`（%TEMP%，一次性，已删）
在**同一 Qt 6.11.1 / 同一 offscreen 平台**上：裸 QWidget + `show()` + 同键同 context 的 QShortcut，
五种发法（sendEvent 主窗 / postEvent+循环 / activateWindow 后 send / 焦点子件 send / 焦点子件 post）
**全部命中（累计 5）** ⇒ offscreen 合成按键**能**进快捷键系统。
与探针的真实差别只有一个：**KEYNAV 探针从不 `w.show()`**（§7-38：真显示会污染别的读数）⇒
隐藏窗口的 `focusWidget()` 恒空 ⇒ 旧 canary 把键投给了一个没激活、不可见的受理者。
⇒ 判定：**那条 0 属于"量具到不了"（用法错），不属于"现场没有"**；产品路径本身可达。

**二、修法（三层，全绿才算数）**：
1. **canary 重做**：起一枚 show() 过、可聚焦的普通窗口跑 send/post 两种发法。基线读数
   `send(visible-focus)=1 post(visible-focus)=2 (累计 2)` ⇒ 量具有牙这句话现在有证人。
2. **K1 产品链闭环（新增判据段④）**：`w.show()+activateWindow()` 后经 QTimer 排队向焦点件发真 Ctrl+N，
   要求 NewTaskDialog 出现且可见（第一证人）**且** `MainWindow::onNewTask` 进入次数 ≥1
   （第二证人：产品侧测具钩子 `IDM_NEWTASK_COUNTFILE`，未设环境变量时零行为变化）。
   形状沿用 K3 的挂死教训：按键与采样都排在 `outer.exec()` 里，spy(5ms) 负责关窗收摊，
   另有 1.5s qApp 全局兜底 ⇒ M-K1b 那种"开错窗"的路由不会挂死探针（实测 tick=401 正常退出）。
   基线读数 `弹窗可见=1 onNewTask 进入次数=1`。
3. **K1 判据合成四段**：①动作存在 ②context==Application ③canary≥1 ④闭环(弹窗∧handler 计数)，
   `k1ok = ①∧②∧③∧④`，每段各印自己的读数。

**三、变异（三次，各自咬不同段，日志归档 `docs/keynav-mk1/`）**：
| 变异 | 破坏点 | 应红段 | 实测 |
|---|---|---|---|
| M-K1a | `setShortcutContext(Application→Window)` | ② | [FAIL]，②=0、③④仍绿（闭环在 Window 下也通——受理者是聚焦的主窗）⇒ **②是独立牙，不是④的复读** |
| M-K1b | 菜单接线 `onNewTask→onBatchImport` | ④ | [FAIL]，弹窗=0、计数=-1、K3 同时 b)红(c)红（wrongDlg 兜住没挂死） |
| M-K1c | witness 钩子短路（`&& false`） | ④ 的第二证人 | [FAIL]，弹窗=1 但计数=-1 ⇒ **两证人确认可分离：光有弹窗骗不过这条** |
三面跑完全部还原，复跑基线 [PASS]（`baseline-green-*.txt`）。

**四、顺带修掉一处真实的互踩**：K1 闭环必须 `w.show()` ⇒ KEYNAV 探针从此会起产品服务；
上一轮 §7-59 记的"真 GUI 与探针共用管道名"风险从理论变成必然 ⇒ 同批把
`IDM_TEST_PIPE` 的 pid 隔离从 ACTIVATE-only 扩到 KEYNAV（`ui_snapshot.cpp:202`），
套件里 IDM_IPC_PROBE 的 4 条 PASS 不受影响（它仍走产品字面名）。

**五、门禁**：套件 19 模式 PASS=79 FAIL=0（`/tmp/idm-probes/ui-k1-1006-045718`，
ui_snapshot sha12 见提交后复跑）；引擎 180/0（engine_selftest sha12 `a254689c17f5`）；
build/host_manifest/live_roster/probe_roster/a_gate/sniff(12/0) rc=0；
freshness 在产品代码提交前 rc=1（DIRTY_INPUT=main_window.cpp，预期）、提交后转 rc=0。

**仍然没验证的东西**：K1 闭环只在 offscreen 成立，**真窗口管理器（windows 平台）下的 Ctrl+N 没测**
（受本机前台焦点约束，与像素证人同一个边界）；M-K1a 只演示了 context 降级一种形态；
`IDM_NEWTASK_COUNTFILE` 钩子本身没被变异证明"探针忘设时不误伤"（它未设即整块跳过，靠读码保证）；
CI 从未执行；K1 的 tooltip 文本「新建任务(N)」与声明「(Ctrl+N)」不一致这件事仍只印不判。

---

### 2026-10-06 05:3x（第 61 条）— ⑥ 画质下拉的**引擎/数据层**判据：L1–L4 十三道全绿，五面变异各咬指定段

**范围界定**：④ 证的是端到端（真浏览器点选 ⇒ 产物字节），本条证它测不到的四件事——
列表生成、排序规则、默认选择、选中后的值传递。**不是新写解析器复制品**：
L1/L2 跑**真 `idm-next-host.exe`**，L3/L4 跑**真 `hls_variant_probe.exe`（生产链 TaskController⇒HlsDownloader）**。

**一、实现位置（读码定位，全部有本轮实测行为证人）**：
- 列表生成：`src/host/idm_next_host.cpp:357 listHlsVariants()` —— 逐行扫 `#EXT-X-STREAM-INF:`，
  取 `BANDWIDTH=(\d+)` 作 `id`/`fmt`，label=`RESOLUTION · Mbps`，无 BANDWIDTH 跳过，无 URI 跳过。
- 排序：**没有排序步骤**——输出＝master **声明序透传**。UI 默认项「最高画质（默认）」是 content.js:742
  的合成占位、不参与数据层；content.js 不按 height 重排（本轮 grep+实跑双证）。
- 默认选择：`src/protocols/hls_downloader.cpp:198-204` forcedVariant 空 ⇒ 遍历取 max(BANDWIDTH)。
- 值传递：`QString::number(v.bw) == m_forcedVariant` —— **传的是 BANDWIDTH 十进制串，不是索引/URI**；
  未知值不命中 ⇒ 保持 max（回退最高、任务不失败）。

**二、覆盖判定**：L3 默认/传值 = hls_variant_probe V0–V3 **已有**（本轮复算：夹具换成本文件自建后仍 4/4 PASS）；
L1/L2 列表生成与排序 **没有**任何既有测具 ⇒ 新增 `tools/hls_variant_logic_test.py`（13 判据）+
两个最小测具面：host 侧 `IDM_HOST_LIST_JSON=<url>`（跑真 listHlsVariants 印 JSON 即退，不进 stdin 循环）、
probe 侧 `IDM_PROBE_CASE_VARIANT` 单例面（URI 交换夹具下整跑会三条全红失去诊断力 ⇒ 只切一个变量）。

**三、基线读数（HEAD `565800f`，HOST=`9b2c6a56cd2c` PROBE=`b95a02c7e890`）**：13 条全绿——
L1a success / L1b id 序列==声明序 / L1c fmt==id / L1d 值是十进制串 / L1e 无 BANDWIDTH 档被跳 /
L1f 无变体⇒success=false / L2a label 内容+同长自检 / L2b height=[1920,540] 第二证人 /
L2c **乱序声明⇒输出保持声明序（反序夹具，钉"没重排"而不是"碰巧有序"）** /
L3a 默认⇒hi 字节 / L3b 选 246440⇒lo 字节 / L3c 未知值⇒回退 hi / L4 URI 交换夹具⇒hi/lo 各自归位。

**四、变异五面（日志归档 `docs/hls-logic-mk/`，每面跑完当场还原）**：
| 变异 | 破坏点 | 红条签名 |
|---|---|---|
| M-a `v.bw > chosen.bw`→`<` | 引擎默认挑最低 | **仅默认面** L3a/L3c/L4 红、L3b 绿（显式选档不受影响）|
| M-b 匹配改 `toInt()` 当索引 | 值语义 | L3b/L4 红、L3a/L3c 绿（默认逻辑没动）|
| M-c 摘掉 forcedVariant 消费块 | 用户选择被静默忽略 | L3b/L4 红、其余绿 |
| M-d 不跳过无 BANDWIDTH 档 | host 列表生成 | **恰好 L1e 一条红**（ids=['0','246440']）|
| M-e variants 整体反序 | 声明序透传 | L1b/L2a/L2b/L2c 四条齐红 |
⚠ M-e 第一次用 `std::reverse(QJsonArray)` **编译失败**（QJsonValueRef 不可 swap）⇒ 变异体自己也要先过
"它在盘上生效"这道门（构建 rc=0 + 二进制 sha 变了才许跑），否则测的是旧二进制。
三面还原后复跑 13/13 绿（`baseline-restored.txt`）。

**五、过程中驳回的两个假象（都是量具侧）**：
1. probe 输出在 capture pipe 里恒 0 字节：Windows GUI 子系统无控制台时 qInfo/qWarning 走 Debug 通道 ⇒
   `QT_LOGGING_TO_CONSOLE` 已废弃且无效，要 `QT_FORCE_STDERR_LOGGING=1 + QT_ASSUME_STDERR_HAS_CONSOLE=1`。
   差点把"下载其实成功"读成"L3 全红"。
2. 单例面第一版排在 `IDM_HLS_VARIANT_PROBE` 闸之后，而调用方两个都设 ⇒ case 请求跑了整跑面。
   判别式夹具当时表现为"三条全红"——那正是我预判过的**无诊断形状**，归因正确、形状暴露了闸序缺陷。

**六、门禁**：套件 19 模式 PASS=79 FAIL=0（`ui_snapshot b8e4fc20be1e`）；build/host_manifest/live_roster/
probe_roster/a_gate/sniff(12/0) rc=0；freshness 提交前 DIRTY_INPUT×3（就是本轮未提交的 host/probe/py），
提交后复跑转绿；引擎自测本轮未改其输入（`src/core|utils|protocols` 中仅 protocols 被变异触碰又还原，
`git diff src/protocols` 为空）⇒ 沿 05-06 号读数有效，另在同批二进制上复跑 180/0 见提交后终态。

**仍然没验证的东西**：三档以上 master 的排序透传（只测两档）；BANDWIDTH 相同但 RESOLUTION 不同的并列档
（引擎取首个命中，UI 显示两条同值选项这件事没人拦）；host 8s 超时分支与网络错误文案；
yt-dlp 格式列表一侧（非 .m3u8）不在本条范围；L2c 只演示"反序"一种重排形态（按 height 排序未必红 L1b）；
CI 从未执行；`IDM_HOST_LIST_JSON`/`IDM_PROBE_CASE_VARIANT` 两个测具面的存在性靠读码，未入闸门名单。

**补录（提交后终态与一次我自己的读数错误）**：提交 `a7760bd` 后复跑——freshness rc=0（不合格=0）、
build/host_manifest/live_roster/probe_roster/a_gate/sniff(12/0)/notice_verify(FAIL=0) 全 rc=0、
套件 PASS=79 FAIL=0（`ui_snapshot b8e4fc20be1e`）、引擎 **180/0**（engine_selftest `a254689c17f5`）、
logic 测试在 HEAD `a7760bd` 上连跑 4 次 13/13。⚠ 期间我报过一次"logic rc=2"——查一手日志是
**我的调用命令 cwd 被重置回会话工作区**（python 找不到 tools/ 下的脚本），不是产品或测具 flake；
台账规矩照旧：异常读数先读它自己的一手输出再定性。

---

### 2026-10-06 05:5x（第 62 条）— ⑦ 测试覆盖总盘点与延迟项审计（零代码轮，报告入库 `docs/coverage-audit-1006.md`）

**做了什么**：把 §7 第 24–29、56–61 条的全部「仍然没验证的东西」去重成 **22 项 × A/B/C 三类**
（A 现实影响 6 ｜B 验证完整性 10 ｜C 环境限制 6），每项带出处、风险一句话、最小验证路径与工作量档；
另按 REFACTOR_PLAN.md 做战略对照（用户口中的 "piclist.md" 仓内不存在，已如实标注并按字面基线执行）。

**审计中的两次自我驳回（都是我的量具，不是产品的错）**：
1. **A2"死 UI"误判**：`grep "HashVerifyDialog("` 判全仓无构造点 ⇒ 差点写进结论"入口缺失"。
   放宽到类名再数，构造点在 `task_detail_dialog.cpp:263` 的**换行调用形状**里；
   `git log --all --name-status` 显示自基线 `874427e` 从未被删。净结论降级为"功能活着、零测试"。
   教训固化：**否定性 grep 交付前必须拿一个已知正例照穿自己的模式串**。
2. **A1"引擎放大风险"过头**：实读 download_core.c:119-122 后收回——`\?\` 不规范化仅 ≥260 长路径生效；
   净结论不变（IPC→引擎全链无 dir/name 清洗 + `..` 语义随长度漂移），但措辞降到已证形状，
   并明写"未做真遍历 PoC"。

**最高优先项**：A1（唯一安全项，链条完整：网页→扩展卡片(④ 已证 dir 端到端)→IPC(无校验)→引擎裸拼接建文件）。
次优 A2 对拍/A5/A3/A4（全部可复用 ⑤ 的 `IDM_NEWTASK_COUNTFILE` 钩子，小量）。

**口径声明（报告 §5/§6 原文同步）**：本 md 与审计报告自身入库即改变 NOTICE 的 docs/ 计数
（现值 FAIL=3：docs 82→83、字节、合计 424→425）⇒ **本轮不声称"含报告的树七门全绿"**；
代码输入集合逐字节未变（`git status --porcelain -- src resources … tools` 空）、live_roster rc=0，
套件/引擎沿 HEAD `85224ab` 既有读数（同二进制同源码，不重跑是声明不是遗漏）。NOTICE 复签：docs=83/674911、合计 415+10=425，复签值以 `notice_verify` 汇总行为准（本轮迭代到 FAIL=0 的定点号印在该工具输出里，不在这里抄数——抄了就会因这段文字本身改变 docs 字节）。

---

### 2026-10-06 06:2x（第 63 条）— ⑧ A1 负例夹具轮：PathGuard 探针 + [37]，先红归档，修法只提案未实施

**要求原话**："不碰代码、纯审计"延续到本轮 = **产品源码零改动**（`git status --porcelain -- src resources … CMakeLists.txt` 空），只交付测具与读数。

**一、A1 判定 = 确认，但口径比审计报告更窄也更准（一手读数 `docs/a1-red/red-baseline-1006.txt`）**：
- filename 面：**引擎有清洗**——`[37 读数] 输入「..\..\evil_a1.bin」存下filename=「.._.._evil_a1.bin」`
  （sanitize_filename_utf8 把 `\ / : * ?` 全换 `_`）。但 PathGuard G2 证明 **IPC 转发值仍含 `\`**
  ⇒ 桌面 GUI 手填路径那条路（NewTaskDialog→internalAddTask）不经过 dlmgr_add 的清理 ⇒ 暴露真实存在。
- dir 面：**全链零清洗坐实**——G1 `success=1 转发dir=「..\evil_dir_a1」`；[37②] state 文件里越界 dir
  经 load_state 原样进队列（可查=1）。三条注入路（IPC add / CLI / state 恢复）同形状。
- 长度依赖形状：`\?\` 不折叠仅 ≥260 生效（download_core.c:119-122）⇒ 短遍历靠 OS 折叠出根、
  超长连折叠都没有——同一输入两种语义，必须被夹具钉住。

**二、交付的测具（两条，共 6 断言，基线形状 = 该红的红、该绿的黑盒对照也绿）**：
| 测具 | 断言 | 基线 |
|---|---|---|
| `IDM_PATHGUARD_PROBE`（ui_snapshot 新模式：真 IpcServer + pid 私有管道 IDM_TEST_PIPE 扩面 + 回调只记转发值，零落盘零触网） | G1 dir 越界须拒/收敛；G2 name 越界须清；G3 正例逐字原样转发 | **G1 FAIL / G2 FAIL / G3 PASS**，rc=1 |
| engine_selftest `[37①②③]` | ①name 引擎证人 ②dir state 恢复证人 ③正例对照 | **①PASS ②FAIL ③PASS**，套件 182/1 |
⚠ 新模式**未进 CI PROBES 与 PROBE-ROSTER 表**（probe_roster 现算会把它列为文档缺行）⇒ 登记为修好轮收尾项；本轮刻意不进门（红门不许进闸门名单，这是台账规矩）。

**三、过程中驳回的一条假牙（自查）**：G1 第一版谓词用 `!fwd.dir.startsWith(rootPg.left(2))` 判越界——
相对路径 `..\evil_dir_a1` 不以盘符开头就喊越界，那会把合法相对目录也算坏 ⇒ 改成"转发值含 `..` 分量"
这一条与被测缺陷同名的谓词，并把"拒绝式修复"显式列为另一支绿（`!ok` 也算过）。
另：d1/d2/d3 读数第一版用了 printf 风格 `%d/%s` 塞进 `QStringLiteral().arg()` ⇒ Qt 报
`Argument missing`、印出字面量（跑出来的读数行没有插值）——**读数印了不等于读数对了**，
grep 一手日志才看见，改 `%1..%5` 后复跑才有上表那些带值的行。

**四、修法提案（§7 of coverage-audit；等授权，未实施）**：拒绝式（建议）或收敛式，落点 ipc_server.cpp add 分支
+ internalAddTask 兜底 + 引擎 dlmgr_add/load_state 入参检查；白名单根怎么定是**产品决策交主控**；
TaskInfo/summary 需增最终落点字段否则清洗对不对无法逐字核；变异计划三条（摘 IPC 清洗⇒G1/G2 红、
摘引擎检查⇒[37②] 红、写成"含点就拒"⇒G3/[37③] 红）。

**仍然没验证的东西**：夹具证到"受理/转发"层，**没有做真写出根外的 PoC**（边界内不写敏感路径；
且 start 会触网）⇒ "实测可越界写文件"这句话仍未被证，被证的是"参数原样抵达引擎并被受理"；
dir 面证人读不出收敛后的具体路径（TaskInfo 无 save_dir，提案第 4 条就是补这个）；
web_server/CLI 两个旁路入口未单独建夹具（它们汇入同一条 internalAddTask，按共享段处理）；
新测具未进门；CI 从未执行。

---

### 2026-10-06 06:4x（第 64 条）— ⑨ A2：哈希校验对拍 + 入口链探针 `IDM_HASHVERIFY_PROBE`，六面变异（五面咬中、一面如实登记"不可见"）

**一、入口定位（读码，全部本轮实测行为证人）**：
用户路径 = 双击任务列表行（main_window.cpp:538 doubleClicked→onTaskDoubleClicked:1099）
→ 非模态 TaskDetailDialog（:1114）→ 「校验文件完整性...」按钮（task_detail_dialog.cpp:136，
connect :143 → onVerifyFile）→ onVerifyFile 用 **dlmgr_get_task_summary** 拿 save_dir+filename
拼完整路径（:252-257，拼法 `dir + QDir::separator() + fn`）→ `HashVerifyDialog dlg(this, fullPath)` exec。
参数：**文件路径字符串**（不是任务 id——出了对话框就没有 id 了）；结果展示：对话框内
m_resultLabel（十六进制串）+ m_matchLabel（✓/✗/橙色跳过三态），无状态栏/日志出口。

**二、探针六断言（真控件、真信号链：findChild 拿 combo/edit/button，click() 走真 onVerify 分块循环）**：
| 断言 | 判据 | 基线 |
|---|---|---|
| H0 | 夹具 `abcde` 的三算法结果 == **certutil（Windows 自带独立实现）现场生成向量**（SHA-256 36bbe50e…、MD5 ab56b4d9…、SHA-1 03de6c57…；不是 Qt 自算自对拍） | PASS |
| H1 | 对话框分块结果 == QCryptographicHash::addData(&f) 独立重算（同库两种调用形状互印） | PASS |
| H2 | 预期值正确⇒「✓ 校验通过」（含大写输入，验 :158 toLower 归一）；错一位⇒「✗ 校验失败」——两分支都有牙 | PASS |
| H3/H3b | 详情对话框里按钮**逐字全等**存在且 enabled | PASS |

**三、变异四面（各红各段，跑完当场还原，证据档 `docs/hashverify-a2/probe-evidence-1006.txt`）**：
| 变异 | 破坏点 | 签名 |
|---|---|---|
| M-⑨a `computed==expected`→恒真 | 比对语义 | **恰好 h2b=0**（错值也报通过），其余全绿 |
| M-⑨b MD5 标签挂 Sha1 | 算法映射 | h0=0，读数 md5=「03de6c…」sha1 仍对 ⇒ 点名到算法粒度 |
| M-⑨c 结果截断 left(32) | 输出完整性 | h0=0 h1=0、**h2 仍绿**（比对两侧同被截断仍相等）⇒ H1 的第二证人价值当场演示 |
| M-⑨d 按钮改名加后缀 | 入口可寻性 | h3=0 h3b=0 |
| M-⑨a4 摘 expected 侧 toLower | 大小写归一（预期侧） | **恰好 h2c=0**（喂大写预期 ⇒ 判不匹配），其余全绿 |
| M-⑨a3 摘 computed 侧 toLower | 大小写归一（计算侧） | **不可见**——两侧都归一时该变异与未变异在对话框任何输出上都无法区分。诚实登记：h2c 只钉住了 expected 半边；computed.toLower() 那半边**没有可区分的观测形状**，不硬造判据（造了就是恒真断言）。要让它有牙得改夹具语义（比如产品将来允许"大小写敏感模式"），那是需求不是测试。 |

⚠ M-⑨a3/a4 的排查过程本身贡献了一条测具教训：第一版 h2c 用"good[0]==lower"当证人，
而 good 面喂的是**大写**预期值——`toHex()` 恒产小写 ⇒ `good[0]` 永远不等于大写串，
h2c 的形状被两次误读（一次"红得不合预期"、一次"绿得可疑"）。最终形状：**h2a 用小写输入+逐字全等、
h2c 用大写输入+通过判定**，两条各钉一侧归一，且都在盘上一手日志里可复算。

**四、本轮抓回的三条测具缺陷（都在交付前修掉并留字）**：
1. **sed 替换串含 `/` ⇒ 整条命令没执行**，我差点把"未改动的绿"记成 M-⑨a 的结果——变异生效证人=先 grep 标记串在盘上（`grep -c M-⑨a`=1）再跑门（§二十二 的同族）。
2. **H3 第一版谓词用 `contains("校验文件完整性")`** ⇒ M-⑨d 改名"校验文件完整性(占位)"照样绿。**contains 是入口类断言的固定无牙形状**（§二十一第四例），改逐字全等后同一变异立刻红。
3. d0/d2a/d3 又犯了 printf 风格塞 `.arg()`（⑧ 刚记过的同形错），读数行带 `%d/%s` 字面量——靠一手日志发现，改 `%1..%n`。教训升级：**同类错误第二次发生就要进长期记忆**，已写。

**五、门禁**：build/host_manifest/live_roster/a_gate/sniff/notice_verify rc=0；freshness rc=0（产品源码零改动，`git diff --stat src/` 空）；probe_roster rc=1 = PATHGUARD+HASHVERIFY 两个新模式未进文档表/CI 名单（沿 ⑧ 的规矩：修好轮一起收尾进门）。套件沿 e871ec7 前的 79/0 未重跑（本轮只动 ui_snapshot 一个新增分支，不触碰其它模式代码——这个"没跑"是声明不是遗漏）。

**仍然没验证的东西**：H3 只证按钮存在且可用，**没点它**——点了会 exec() 起嵌套模态循环，探针无人收尸（K1 同款坑，登记为缺口；"按下⇒对话框出现"这一段目前无断言）；onVerifyFile 的路径拼接只在 -999（任务不存在⇒fullPath 退化 filename）形状下走过，正常完成任务的 dir+sep+fn 拼接无逐字证人；大文件（跨 4MB 块边界）只有 H1 的单形状对拍，进度条百分比无断言；错误路径（文件打不开⇒QMessageBox）刻意不进探针（模态框）；browse 按钮/QFileDialog 未测；computeHash() 那个成员函数**无人调用**（onVerify 自己重写了一遍分块循环——两份哈希实现并存，死代码疑云，属作者权问题只报不删）；**computed.toLower() 那半边归一无可区分观测（M-⑨a3 实测"不可见"），当前夹具形状下它是一行语义正确但测不到的代码**。
### 2026-10-06 07:4x（第 65 条）— ⑩ A3：剪贴板回调探针 `IDM_CLIPBOARD_PROBE`，四面变异各咬一条 + 给「红门」开了一类登记口径

仓根 `D:/visual studio/lianxi/MFC/idm-next`，起点 HEAD `a4ce4b8`。产品源码本轮**零改动**（四次变异跑完逐字节还原，`git diff --stat src/` 空）。
证据目录 `docs/clipboard-a3/`（README + 六份原文，逐文件 sha12 见 README 表）。

**一、定位结论（一手 grep + 读码，不是推测）**：全仓 `clipboard()` 调用只有三处 ——
写侧两处（`history_dialog.cpp:130` 双击历史行复制 URL；`hash_verify_dialog.cpp:189` 「复制结果」按钮复制摘要）、
读侧一处（`main_window.cpp:858` `onClipboardChanged` 读剪贴板）。
需求里问的"选中任务后复制**任务信息**"这一族**在仓内不存在**（没有右键复制菜单、没有复制任务详情到剪贴板的入口）⇒
如实报"有调用但只有那两个入口"，不硬造探针。**写入格式=纯文本单行**（URL 或十六进制摘要），无 HTML/RTF 富格式。

**二、五条断言（C1/C1b/C2/C3/C4，全绿 rc=0）**：
C1 双击短 URL ⇒ 剪贴板逐字==该行 URL 且标签报出该链接；C1b 90 字符长 URL ⇒ **剪贴板全长不截断**而标签折成 68 字符（截断只属于显示层）；
C2 「复制结果」按钮逐字实名存在 + **两面**（空结果不改写=早退守卫有牙 / 有结果逐字写入并读回=写入本身有牙）；
C3 合法链接进读侧 ⇒ 恰好弹一个「检测到下载链接」框且正文首条非空行逐字==URL（大写 `.CLIPZIP` 靠 `toLower` 归一命中小写白名单）；
C4 负例三连（非链接 / 白名单外扩展 / 与上次相同）⇒ 一次都不弹。

**三、三条第一版判据被实测驳回（先取证再改期望，没一条是靠放宽过的）**：
① C1b 原写 `!labelB.contains(20 连 x)` —— 产品截断留的是 `left(60)`，夹具去掉前缀后剩 70 个 x，
   截断点落在 x 族**内部**（标签含 46 连 x）⇒ 谓词恒假、夹具恒红。改成 `剪贴板==全长 ∧ 标签长度<URL长度`。
② C3 的 `before=spy.seen` 原本取在 `clipboard()->setText()` **之后**：`setText` 自己会 fire `dataChanged`，
   第一个框在那一刻就弹了 ⇒ 差分恒 0，读数出现"首见文本非空却报命中弹窗=0"的自相矛盾。快照必须放在 setText 之前。
③ 正文提取原来只跳到第一个 `'
'`，而模板是 `…：

%1

是否添加…` ⇒ 尾部段落一起留下、逐字比较恒不等；改为取第一条非空行。
这三条都是**测具的错**，不是产品的错；如果当时直接放宽判据或删断言，就会把三条真信号洗白。

**四、四面变异（各咬一条、互不重叠，原文 `%TEMP%` 备份 + 跑完逐字还原；每面先 grep 确认标记串已在盘上再重编）**：
M-⑩a 摘 `history_dialog.cpp:130` 的 `setText(url)` → exe `bbf7b8fcaafe` → **C1+C1b 红**，且标签仍显示「已复制链接: …」
（证明标签与剪贴板是两个独立字段，光看 UI 永远发现不了写入没发生）；
M-⑩b 摘 `hash_verify_dialog.cpp:189` 的 `setText(hash)` → exe `0e7efd677a75` → **只有 C2 红**，
而同一行的"空结果未改写=1"仍绿 ⇒ 这就是我给 C2 补正例面的理由：**期望值为 0 的那一面给不了"应当写"的牙**；
M-⑩c 写入内容改成 `url.left(60)` → exe `7e022722de70` → **只有 C1b 红**（26 字符那条不受影响 ⇒ 判别式隔离成功）；
M-⑩d 摘读侧扩展名守卫 `if (!extOk) return;` → exe `2ab8dafdcbd0` → **只有 C4 红**，新增弹窗=2 而不是 3
（"非链接"那条被前缀门挡住 ⇒ 2 这个数本身就是过滤链顺序的证据）。还原复绿 exe `fc642b28777f`。

**五、给「红门」开的登记口径（新类型 + 两面夹具）**：`probe_roster` 原先只有 断言型/人眼对照 两类，
而"断言型 ⇔ 必须在 CI 名单"这条铁律把 ⑧ 的红门逼进两难：进了 CI 常红、不进则 ROSTER-MISSING 点名。
新增第三类 **待修型（刻意不进闸门名单）**，规则只收紧不放宽：待修型必须不在名单、断言型/人眼对照的原规则一字未动，
并在 §1.4 写明"待修型条目必须同批在 §7 登记转成断言型的收口条件"——当前唯一住户 `IDM_PATHGUARD_PROBE`，
收口条件=A1 dir 面清洗落地那轮改标断言型并进 CI。`HASHVERIFY`(6 条)/`CLIPBOARD`(6 条)本轮进 CI 名单，
名单 19→21，`TOTAL_PASS ≥50` 钉到 **≥70**（本机 21 模式现算 96 条，余量 26）。
新类型自己也要有牙：`mutation_fixture_live_roster.py` 加 M12/M13 两面（打 probe_roster），
现 **16/16 按预期**（BASELINE+11 旧面+BASELINE-ROSTER+M12+M13，还原核对干净）。
顺带修了夹具自身的一处口径错误：它原来用"[RED] 字样"当"期望绿"的判据，会把与本门无关的红（NOTICE 没复签到定点）
误判成夹具 BAD ⇒ 改成本门末行的机器事实（`live_roster OK` / `[PROBE_ROSTER] OK`）。

**六、门禁现值（全部在提交后的树上复跑，不是变异中间态）**：build/host_manifest/live_roster/probe_roster/lint_layering/a_gate/sniff/notice_verify/freshness **九道 rc=0**（sniff 那一道本轮改测具后已转绿，见本节第七项）
（PATHGUARD 不再让 roster 红，因为它已被合法登记为待修型）；
套件 24 模式 PASS=92 FAIL=2 零断言=0，问题模式只有 `IDM_PATHGUARD_PROBE(F=2,rc=1)`＝**按设计红**（A1 未修，见 §7-63），`ui_snapshot.exe sha12=d9565a213d45`；
引擎自测当场复算 **182 通过 / 1 失败**（唯一红＝`[37②]` dir 面 state 恢复，同属按设计红），`engine_selftest.exe sha12=d5936975a14a`；
NOTICE 复签到定点：`436 = 426 + 10`（⑩ 的证据目录 `docs/clipboard-a3/` 入库后，
`docs/` 行从 85/699960 变成 **94/716729**、合计从 427 变 **436**；`tools/=1003379`、`.github/=37222`；
恒等式里那个 10 是**仓根散装文件数**，与 `browser-extension/` 自身的 19 个文件是两个不同的数，别混——上一版 NOTICE 把它写成 `417+10=427` 时这个歧义就存在了，本轮顺手把话说死），
两轮迭代后 FAIL=0，`notice_sha12` 以 `notice_verify` 当场输出为准（工具号 `16f38fd0dd63/242行`）。

**仍然没验证的东西**：① offscreen 下 `QClipboard::dataChanged` 由真实系统输入（Ctrl+C）触发的链路仍未证——
C3/C4 是用 `QMetaObject::invokeMethod` 直接驱动 handler 证的，**信号连接面（`main_window.cpp:444-445`）只有静态证人**；
② Yes 分支（弹窗点「是」⇒ `internalAddTask`）没走：模态框由测具 `done(QMessageBox::No)` 收尸，点 Yes 会真的建任务、
落盘、触网，本轮边界不允许；③ 剪贴板富格式（HTML/图片）本仓不涉及，但也因此没证过"复制多行任务信息"这类形状；
④ CI 仍未执行过一次（push 属用户边界），`TOTAL_PASS ≥70` 是新写的、从未在 runner 上响过；
⑤ PATHGUARD 的 G1/G2 仍红＝A1 dir 面零清洗未修，修法提案在 `docs/coverage-audit-1006.md` §7-63 等授权。

**七、sniff E2E 本轮复跑 rc=12（capstone 未闭合）——一手取证后定性为「测具假设错 + 一条真实产品缺陷」**：
链条本身全绿（`[VERDICT] 链条接上：网页识别 → popup → 真点击 → host → GUI 收下`、任务进『已完成』、`.idmtmp` 残留=0），
红在最后一步取证。逐手证据：fixture 服务器 `/media/dom.mp4` = 24 B ftyp 头 + 200000×`0x01` ⇒ **恰 200012 B**；
本轮（08:04:38）落盘的是 `~/Downloads/dom (2).mp4`，`sha256(dom.mp4)==sha256(dom (1).mp4)==sha256(dom (2).mp4)`
=`267989ec0181…`（三文件同 size 同 sha）⇒ 字节完整写对，只是**名字被改了**。
原因不是引擎没同步：引擎 `pick_free_filename` 会就地更新 `t->filename`（download_core.c:1334 注释自明）。
是 GUI 只在**建行时**读一次名字：新建任务走 `TaskController::internalAddTask`，
`row.fileName = name.isEmpty() ? QUrl(url).fileName() : name`（task_controller.cpp:90/142/203，全部来自 URL），
完成后的 tick 只刷状态/速度/提示（main_window.cpp:1500-1513 `continue`），**从不回读 `info.filename`**；
唯一读引擎名字的那行（:1520）在"恢复的任务没有 Qt 回调"那个分支里。
⇒ 用户可见后果：**同名重下时列表显示 `dom.mp4` 而盘上是 `dom (2).mp4`**，双击/打开目标会指错文件。
测具那一侧同时错两处：① 拿死名 `f_expected = Downloads/dom.mp4` 当唯一取证点（disk_e2e.py:251 同形状隐患）；
② 它的注释写着「向 GUI 问这个任务的保存目录（ipc_server 的 info 命令带路径）」但代码根本没问，直接用了注册表默认目录。
修法候选（属作者权，等点名才做）：GUI 在状态 tick 里对已完成任务回读 `dlmgr_get_task_info().filename` 并 `setFileName`；
或 IPC `list` 改为从引擎现取文件名。测具侧改法：先向 GUI/host 要最终落点名，再按那个名字取证。

**处置进展（所有者点名后）**：测具那一半已修——`tools/sniff_e2e.py` 的取证点不再写死死名，改为
①点击前取 GUI 已有任务 ID 集作基线、②本轮新 ID 必须不在基线内（ID 判别式）、③最终文件名向
IPC `info <id>` 的「文件:」行现取，再只在默认目录按那一个名字比 size/sha256；新增 `gui_ipc_json()`
是因为 `disk_e2e.gui_ipc` 只会发裸 `{"command":...}`，带编号的命令进不去。**产品那一半没动**（仍等授权）。
两面变异证明这条新判据不是自我实现：M-⑩E 把 `ipc_server.cpp:335` 的 `.arg(row->fileName)` 换成常量假名 ⇒
capstone 红且点名该假名（证明测具真在吃 GUI 给的名字）；M-⑩F 临时伪造"所有 ID 点击前就存在" ⇒
capstone 红并报"不是本轮的"（证明它不会拿"名字碰巧对上"蒙混）。M-⑩F 用的 env 开关是一次性脚手架，
已从源码删除（不留永久后门）。原文三面归档 `docs/clipboard-a3/sniff-{capstone-green,capstone-red,mut-gui-bogus-name,mut-forged-baseline}-*.txt`。
夹具残留 `~/Downloads/dom*.mp4` 共 7 份经逐份 sha256 核对为本轮产物后按指示清除（清点档
`sniff-dom-artifacts-before-delete.md`），用户其他文件未触碰。

最终读数（提交后的树）：sniff **rc=0**、capstone 单轮闭合，`app_exe_sha12=e1f62f7d3b7e`；套件 24 模式
PASS=92 FAIL=2（唯一红＝IDM_PATHGUARD_PROBE，按设计），`ui_snapshot.exe sha12=ea506238e63d`；
引擎自测 182/1（唯一红＝[37②]，按设计），`engine_selftest.exe sha12=56fa9e75f292`。
清残留时又发现一次运行落的 `dom.mp4`（同 sha256 `267989ec0181…`、200012 B），一并删除 ⇒ Downloads 内 `dom*` 计数=0。

### 2026-10-06 11:4x（第 66 条）— ⑪ A4：拖拽回调只读审计，零代码改动

仓根同上，起点 HEAD `cac4b32`。报告落 `docs/drag-audit-1006.md`；本轮 `src/` 一行未动
（提交前后各查一次 `git diff --numstat cac4b32..HEAD -- src/` 为空）。

**普查口径**：先跑正例对照证明 grep 没瞎（同套 `--include` 下 `clipboard()` 命中 5 行），再逐符号数
`setAcceptDrops`=1、`dragEnterEvent`=定义 1、`dropEvent`=定义 1、`QMimeData`=2、
`dragLeaveEvent`/`dragMoveEvent`/`startDrag`/`mime->data(...)` **全部 0**
⇒ 全仓只有 **MainWindow 一个入向 drop 面**，无子控件重写、无外向拖出。中文侧"拖放/拖入"交叉核对
另两处（`:549` 空状态插画不拦截、`:1901` 文案）均为说明文字，不构成第二入口。

**结论：2 个回调 + 1 个下游执行体；安全/已查不是缺陷 3 条，A 类风险 3 条，B 类验证缺口 3 条。**
- R1（B）`dragEnterEvent:957` 谓词只有 `hasText()||hasUrls()` ⇒ 门过宽，靠下游 StrictMode 兜底，属无人断言的口径。
- R2（A）`dropEvent:972` 取文本后**无长度上限** ⇒ MB 级字符串进单行编辑器与 URL 解析。
- R3（A）`:985-988` **失败静默**：IPC/web 侧有 `if (id<=0) Log.warn; return;`（`addTaskFromUrl:1754-1757`），
  剪贴板分支至少记 info，拖放这条只有成功分支 ⇒ 用户点完确定什么都没发生且日志无从区分。
- G1（A）dir 面零清洗＝**继承 ⑧ A1，不是新增缺陷**：拖本地文件得 `file:///…` 会被对话框拒
  （scheme 白名单 http/https + ftp/ftps/magnet/.torrent，`new_task_dialog.cpp:348-359`、
  `torrent_downloader.cpp:93-113`），但 `.torrent`/`localTorrent` 那支仍把 dir 原样交给未清洗的下游。
- U1（B）**拖放路径零测试覆盖**：`grep -rn "dragEnter|dropEvent|QMimeData" tools/*.cpp tools/*.c` = 0 命中
  ⇒ ⑦ 当时登记的 A4 建议至今未被做掉，本轮复核为真。
- U2（B）一次拖 N 个只取第一个（`:967-970`）行为合理但从未断言；且拖放传 `queue=""` ⇒ 立即开下、
  **绕过 `maxConcurrent`**（队列侧才有 `qMax(1, m_maxConcurrent())`，`queue_scheduler.cpp:42,71`，默认 4）。
- C1/C2（已查不是缺陷）：无 `startDrag` ⇒ 不存在 MIME 外泄面；未实现 leave/move 是 Qt 默认忽略即可。

**一处需要定夺的台账不一致（只报不自行改）**：`coverage-audit-1006.md:22` 把 A4 分类列写成 **A**，
而 `:61` 的汇总行把「剪贴板监听 / 拖放 / 托盘」整体归到 **否**。我的判断：拖放的**覆盖缺口**是 B（U1），
拖放的**行为缺陷**（R2/R3）才是 A——两个号不该共用一格。改哪一处属作者权，等点名。

**仍然没验证的东西**：① 全程静态读码，`QDropEvent` 从未真的发给过 MainWindow ⇒
"拖放能弹出预填好链接的对话框"目前是推断不是观测；② `file://` 被拒依赖那段代码被执行，未实测拖真实文件；
③ R2 未测出"多少字节开始卡"；④ 未跑"连拖 100 次"的稳定性，只证明了代码层面一次 drop 至多建 1 个任务；
⑤ CI 仍未执行过。

### 2026-10-06 12:4x（第 67 条）— ⑫ A4 R2/R3 修复：`IDM_DRAGDROP_PROBE` 先红后绿 + 四面变异各咬一条合取项

仓根同上，起点 HEAD `1b767c6`。产品改动**只有 `src/gui/main_window.cpp`、+18 行 0 删除**（`git diff --stat -- src/` 当场复算），
探针在 `tools/ui_snapshot.cpp`，证据目录 `docs/drag-a4/`。

**一、改了什么**：
- **R2** dropEvent 取到 url 后、开对话框前加长度上限 ⇒ `Log::warn("拖放内容过长（N 字符），已忽略…")` + `acceptProposedAction()` + return。
  **上限没有新造常量**：取引擎自己的 URL 缓冲 `sizeof(DownloadTask.url)`（`download_core.h:67 char url[2048]`）——
  超过它的输入本来就会被 `strncpy` 截成另一个 URL，静默截半不如明确拒掉；且 `DownloadTask` 是匿名 struct typedef，
  没有成员指针类型可写 ⇒ 用一个实例字段算 sizeof（编译期常量，跟着引擎缓冲一起变）。
- **R3** 成功分支后补 `else Log::warn("拖放添加任务失败: %1")`，措辞对齐 `addTaskFromUrl` 里既有的 `"IPC 添加任务失败: %1"`。

**二、TDD 顺序（先红归档再修）**：`docs/drag-a4/red-baseline-1006.txt`＝修复前的原文，D1 PASS / D2 FAIL / D3 FAIL。
三条断言里 **D1 是期望非 0 的正例对照**（正常 URL 必须真弹出对话框且预填逐字==URL、并被点确定 accept）——
没有它，"drop 一律什么都不做"也能让 D2/D3 蒙成绿。D3 的满额前置**现场制造且可复算**：
`dlmgr_add` 灌到拒绝（实测 `首个拒绝下标=127`、`dlmgr_list=128` == `MAX_TASKS`），跑完逐个 `dlmgr_cancel` 收摊。

**三、四面变异（各咬一条合取项，互不重叠；每面都 grep 确认标记串已在盘上才重编）**：
| 面 | 做法 | exe_sha12 | 红的判据 |
|---|---|---|---|
| M-⑫A | `if (false && url.length() >= kMaxDropUrlLen)` | `bd2f47f626b7` | 只 D2 红，读数回到 `交给对话框的长度=32767` |
| M-⑫B | 保留 return、删掉那条 warn | `33bed21725d5` | 只 D2 红，`有界或拒=1 但 告警=0` ⇒ 证明 D2 是两个合取项、不是一层皮 |
| M-⑫C | 失败支 `Log::warn` → `Log::info` | `3f9aeab16488` | 只 D3 红 |
| M-⑫D | `if (id > 0)` → `if (true)` | `05cac1c3dc53` | 只 D3 红（失败被当成功记 info） |
还原复绿 exe `57568c3f8423`、RC=0。**四面全部如实登记，无一"没咬到"**。

**四、四条被实测驳回的测具假设**（都是我的错，不是产品的错；每条都换了真证据才改期望）：
① 直接 `sendEvent(drop)` **挂死**——`dlg.exec()` 阻塞，控制权进不到采样器（第一版卡十分钟）⇒ 改 K3 同款
   queued 触发 + 外层 `QEventLoop` + qApp 守卫；② **不发 `dragEnterEvent` 就没有 drop 语义**（真实拖放里 drop
   之所以送达是因为 enter 被 accept 过）⇒ D1 当时也红，是我没敲对门；③ 守卫用 `reject()` 关对话框 ⇒
   `exec()` 返回 Rejected ⇒ `internalAddTask` 根本不执行 ⇒ D3 永远看不到失败支，必须**点 OK** 走 `onAccepted`
   （顺带踩到 `NewTaskDialog::result()` 把 `QDialog::result()` 覆盖成返回 `NewTaskInput`，取状态得走基类）；
   ④ `w.findChild<TaskListModel*>()` **恒为 nullptr**（模型是成员不是子 QObject），且"立即开始"的任务占并发槽、
   并不都进引擎队列计数 ⇒ 靠 GUI 灌不满 128，改为按引擎口径 `dlmgr_add`/`dlmgr_list` 制造满额。

**五、一个新加的仪器卫生规矩：探针把自己的 sha12 印进输出**。第一轮 M-⑫C 与 M-⑫D 两份归档**字节完全相同**
（同为 `65af65b70fd5`）——两面红的是同一个判据、读数一字不差，光看日志分不出是哪枚二进制跑的，归档就失去证人。
现在 `[DRAGDROP 身份] exe_sha12=` 由探针自己算（与外部 `sha256sum < … | cut -c1-12` 当场对得上，同一算法），
每面原文独立指向一枚产物。另记两次工具性错误：heredoc 里的反斜杠-n 序列会被字符串转义吃掉，
在 C++ 源码里造出裸换行的字符串字面量（**本会话第三次同类**，已进长期记忆）；以及 `timeout 120 env …`
给出 **RC=0 且零输出**——`timeout` 在这套 MSYS 工具链上的退码不可信，判"跑过了"必须看有没有断言行。

**六、名单与门禁**：`IDM_DRAGDROP_PROBE` 进 §1.4 表与 CI 闸门名单（模式 24→25、CI 名单 21→22），
`TOTAL_PASS` 钉到 ≥70（本机 22 个闸门模式现算 **95** 条，余量 25；套件总数 96 里含红门 PATHGUARD 自己那条 G3，
所以两个号分别是 95/96，别混）。roster 夹具仍 16/16。

### 2026-10-06 15:3x（第 68 条）— ⑬ A4 U2 修复 + 台账口径订正 + R1 定案：D4/D5 两条新断言、七枚变异、公开 API 擦不掉幻影任务这件事

**一、U2 修的是什么**：`dropEvent` 原先 `url = mime->urls().first()`，其余**静默丢弃** ⇒ 用户拖 10 个链接只下 1 个、
而且没有任何一句话告诉他另外 9 个没了。现在 URL 面逐条收、文本面按行拆（与「批量导入」同口径
`batch_import_dialog.cpp:139`），R2 上限改为**逐条**判（超长的那几条单独计数、批次层面报一次「有 N 条内容过长」，
不因一条坏数据丢掉整批），一次 drop **仍只弹一个**对话框；确认后其余按同一目录/线程数一并受理，
文件名一律留空让引擎按各自 URL 推断（手填的名字不该复用给别的 URL），重复 URL 跳过不重建，
每条走 R3 已有日志通道，末尾补 `拖放批次完成: added/total`。

**二、选型：遍历全部，不采用"取第一个 + Log::info 告知忽略"**。任务书给了两个候选，后者只是把缺陷改得更礼貌——
日志不在主界面上，用户看不到，行为仍然丢数据；而"扰民"这个成本其实不存在（批量路径不弹第二个窗）。
⚠ 同时纠正我上一轮写进 §7-66/审计表的错话：**`queue=""` 不绕过 `maxConcurrent`**。控制器用的是
`startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot()`（`task_controller.cpp:101/153/173/214/261`），
`hasFreeSlot()` 拿运行数比 `qMax(1, m_maxConcurrent())`（`queue_scheduler.cpp:38-48`，默认 4 `settings.h:174`）
⇒ `queue=""` 只表示"不受队列归属限制"，满额的条目留在列表里等位。

**三、清场：公开 API 擦不掉 D3 留下的 128 条幻影任务**（D4 连红三轮的真因，与产品无关）。三条路都实测否证：
`dlmgr_cancel` 只置状态、摘槽要等 worker 收尾而收尾要求 `chunk_count>0`（这些任务从没 start）；
`dlmgr_remove_completed()` 只收 COMPLETED/FAILED（`download_core.c:2373`），对 CANCELLED 恒 0；
`dlmgr_remove()`→`detach_slot` 在"还有别人 `worker_alive>0`"时只登记 `pending_detach`，
而 `sweep_pending_detach_locked` 的唯一驱动点是某个 worker 退场（`:1430`）⇒ 倒序摘也实测 `已摘除=5120` 而残留仍 128。
正解是走引擎自己的清空通道：`dlmgr_load_state(<不存在的路径>)` 在读文件之前先调 `reset_all_tasks`（`:2081`）
做 `memset(g_tasks)`+`g_task_count=0`+`g_next_id=1`，随后 `fopen` 失败返回 -1 也不影响清空已经发生。
读数行印 `清场残留/基线条数/清空轮数`，清不干净就让 D4 红并说明为什么红。

**四、两条没牙的判据被变异当场抓出来**（都是我的测具错，不是产品的错）：
① D2 原判据 `告警含「拖放」`——我先跑了一枚把守卫写成 `one.length() >= 0` 的**坏变异**（语义反转成"全拒"），
D1/D3/D4/D5 一起红、**D2 反而绿**：它接受"根本没开对话框"当"有界"的替代，而那条 warn 来自别处。
改成钉 R2 自己那句「内容过长」后，MUT-F（只摘上限）与 MUT-G（保留拒收、只摘 warn）**各自只红 D2**。
② D5 第一版判据"有一行含该 URL"——`TaskController::internalAddTask` 自己就
`Log::info("添加任务 #N (队列:…): url")`（`task_controller.cpp:228`），摘掉 `dropEvent` 里的逐条日志后 D5 仍然绿
（实测 info 行数=5、三条 URL 全被控制器那侧命中）。改成只认 **dropEvent 自己的前缀** + **去重 id 计数**才有区分度。

**五、七枚变异**（详表与原文在 `docs/drag-a4-u2/README.md`，每面带自己的 `exe_sha12`）：
MUT-A U2 回退成只取第一条 ⇒ D4+D5 红；MUT-B 批量支两种日志都摘 ⇒ 只 D5 红；MUT-C 只摘成功日志 ⇒ 只 D5 红；
MUT-F 只摘 R2 逐条上限 ⇒ 只 D2 红（`交给对话框的长度=32767`＝修复前真实形状）；MUT-G 保留拒收只摘其 warn ⇒ 只 D2 红；
MUT-H 只摘批次汇总行 ⇒ 只 D5 红。**作废一枚**：首版 R2 变异写成 `>= 0` 把守卫反转，不是受控变异，
但它值回票价——正是它暴露了 D2 没牙。**未被变异证明的护栏**（不占功）：D4 的 `乱序探针 可查=` 只在残留≠0 时有信息量，
属诊断行；`本次取消=` 无断言依赖。

**六、台账口径**：`coverage-audit-1006.md:22`（A4 记 A）与 `:61`（拖放整体记"否"）的矛盾按两层口径消解——
**A=现实影响的行为缺陷**（R2/R3 已修、G1 未修随 A1）、**B=验证完整性缺口**（U1 暂缓、U2 覆盖侧由 D4/D5 补上）、
**已查不是缺陷**（R1、C1、C2、file:// 主护栏）。⑦ 台账本体不改写（那是一手记录），订正只落在
`docs/drag-audit-1006.md` §2/§4。**R1 定案为"已查不是缺陷"、不改代码**：下游 `NewTaskDialog::onAccepted` 用
`QUrl(StrictMode)` 只放过 http/https+host 非空（`new_task_dialog.cpp:350-352`）、aria2/magnet（:346）、
磁盘真实存在且 `.torrent` 结尾（:353-354）三支，否则明确弹错并 return（:355-358）；R2 又关掉了唯一的物理伤害面
（MB 级字符串进单行编辑器）；而在上游收窄谓词会把 magnet/ftp/.torrent 三条合法支路在"能不能放手"这步砍掉，
等于把下游的分类复制一份到上游、两处规则从此必须同步。**边界**：这条定案只证到"accept 之后不会造成错误下载或卡界面"，
没证"任何文本拖进来体验都好"（会弹一个错误框，设计态），也没实测过真实桌面文件拖入。
**U1 标"已登记，待后续"**：D1~D5 已真发 `QDropEvent`，但 U1 原意还含真实桌面文件 `file://` 与跨进程 drop，offscreen 给不了。

**七、名单与门禁**：`IDM_DRAGDROP_PROBE` 断言 3→5（§1.4 表同步），CI 闸门名单不变（模式已在册，无新增）⇒ `TOTAL_PASS` 那个号不动（现算仍 **95**，阈值 ≥70）。
本轮 src/ 改动只落在 `MainWindow::dropEvent` 一个函数体内（`git diff -U0` 十个 hunk 全在该函数），
零诊断残留；所有变异产物已从盘上撤下并与开工前的 U2 备份逐字节同哈希（`1e846630adc6`）。
