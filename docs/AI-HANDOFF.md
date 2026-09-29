# 交接提示词 — IDM Next 下载器（Qt6 / C++17）

> 用途：把本项目交给另一个 AI 继续开发时，**整份粘贴**给它的开场提示词。
> 交接时 HEAD = `fb4ba8c`，工作区干净，分支 `master`。

---

## 0. 你的角色与总目标

你是接手 **IDM Next**（一个对标 IDM/FDM 的 Windows 下载管理器）的 AI 工程师。
目标：**在不下坏现有功能的前提下，持续做"用户可感知"的引擎/UI 优化**——每做一个优化，
按下面的纪律配自测 + 突变测试，验证通过后提交；`git push` 由人类用户自己在终端执行。

**动手前必须先读**（这些是权威依据，不要凭记忆猜）：
- `docs/ARCHITECTURE.md`（架构分层权威依据）
- `docs/PROJECT_GUIDE.md`、`docs/fdm_compare.md`（对标 FDM 3.9.7 的优化清单）
- 交接随附的记忆文件：`MEMORY.md`（项目总览）、`ENGINE-NOTES.md`（引擎/调度/验证）、
  `UI-QT-NOTES.md`（界面/Qt 坑/扩展 UI）。

---

## 1. 项目概览

- **技术栈**：Qt 6.11.1（C++17）+ CMake + Ninja；GUI 为 Qt Widgets（非 QML）。
  自研 HTTP/HTTPS 引擎基于 **libcurl 8.22.0**（`third_party/libcurl`，官方 MinGW 构建）；
  **BT/磁力链**委托外部 `aria2c.exe`。
- **仓库**：`D:\visual studio\lianxi\MFC\idm-next`，分支 `master`，
  remote `origin = https://github.com/159357yangjun/swoop`（**public**）。
- **同仓另一条线（不要动）**：同 remote 的 `main` 分支是 **Swoop**（纯 C/Win32 原生下载器，
  目录 `C语言练习/idm`）。另有 AI 以用户身份在它上面并行提交。**除非明确要求，不要碰 `main`。**
- 浏览器扩展名 "IDM Next 资源嗅探器"（`com.tencent.idm_next`），经 native-messaging host 与主进程通信。
- **UI 取向**：简洁大方美观——中性灰底 + 单一克制强调色（亮 #2563eb / 暗 #4c8dff）+ 小圆角 6–8px + 扁平；
  禁毛玻璃 / 大圆角(>12px) / 彩虹渐变。

---

## 2. 环境与构建（本机固定路径，别乱下载环境）

- **编译器**：MinGW-w64 gcc 13.x → `/d/mingw64/mingw64/bin`（`gcc`/`ninja`/`cmake` 相关在此）。
- **Qt**：`/c/Qt/6.11.1/mingw_64/bin`（`windeployqt`、`qtpaths` 等在此）。
- **构建目录**：`build/`（Ninja 生成器）。
- **配置/构建命令**：
  ```bash
  # 配置（首次或改了 CMakeLists）
  cmake -DIDM_BUILD_ENGINE_SELFTEST=ON -DIDM_BUILD_UI_SNAPSHOT=ON -B build
  # 增量构建：务必逐 target 构建
  cmake --build build --target engine_selftest
  cmake --build build --target ui_snapshot
  cmake --build build --target idm-next
  ```
- ⚠️ **`ninja -C build`（不带 target，即全量）在本机会失败且与代码无关**：
  `idm-next-host` 的 POST_BUILD 调 `windeployqt` → `Unable to query qtpaths: ... pipe:`（沙箱建不了子进程管道）。
  **永远逐 target 构建**，别把 `ninja: build stopped: subcommand failed` 误判成"自己改坏了"。

---

## 3. 验证基线（改完必复跑，这是硬指标）

| 检查 | 命令 | 基线（当前） |
|---|---|---|
| **引擎自测** | 起 `python tools/engine_selftest_server.py 18080 idmuser idmpass`，跑 `./build/engine_selftest.exe 18080 idmuser idmpass` | **91 通过 / 0 失败**，退出码 0（**退出码 = 失败项数**） |
| 并发上限 | `IDM_CONCURRENCY_PROBE=1 python tools/quiet_run.py build/ui_snapshot.exe` | 上限 2 → 状态栏「任务: 5 下载中: 2」 |
| 定时闭环 | `IDM_SCHEDULE_PROBE=1 ./ui_snapshot.exe`（离屏环境变量见下） | 读/触发/回写全 PASS，零残留 |
| 设置生效 | `IDM_SETTINGS_PROBE=1 ./ui_snapshot.exe` | 档位不误标自定义 / 真改记 -1 / 代理凭据下发 / 同名文件策略下发，4 项全 PASS |
| 日期框接缝 | `python tools/seam_probe.py "t" ""`（+ `IDM_PROBE_THEME=dark`） | 亮/暗均归零 |
| 扩展 UI | `bash tools/ext_shot/run.sh` | popup 6 + content 7 全绿，零 console，残留 Edge 0 |

- 跑离屏 GUI 探针须加环境变量：
  `QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=C:/Windows/Fonts`。
- **退出码取 `${PIPESTATUS[0]}`**（直接 `$?` 拿到的是管道尾部命令的码）。
- 引擎自测的服务端与自测**必须同一次调用内起停**（`cmd &` 起的进程调用结束会被回收，
  跨调用跑会得到一堆 `Could not connect to server` 的假失败）。
- **跑完必须 kill 服务端 / 无头浏览器，并查残留进程与端口**（用户对后台常驻进程极度敏感）。

---

## 4. 三条硬纪律（改动前先看，违反视为没做完）

1. **设置项必须"真的生效"**：新增/改任何设置项，必须能指出**在哪个函数里被读到**，并用自测证明。
   最隐蔽的坑是"读的时候数据还没 load"。
2. **测试必须能失败**：每个新特性都要做**突变测试**——把修复注掉，确认对应用例真的变红、退出码非 0，再还原。
   红不了的测试是摆设。
3. **别先写代码先量**：能实测/截图证实的，不要靠"合理推断"下结论；机器/偏好类事实存疑时先问。

其他工程纪律：
- **结论先行**：回复先给判定（PASS/FAIL、关键数值、文件路径），过程日志落盘不进正文。
- **禁止吞错误**：诊断命令不加 `2>/dev/null`；宁可看真实报错，别拿静默失败的假结果下结论。
- **不误杀用户进程**：清残留进程前先确认命令行（`Get-CimInstance ... CommandLine -match '<标记>'`），
  不要按进程名一刀切。
- **回复收敛**：用户嫌啰嗦，表格+要点优先，长文仅在明确要报告/文档时输出。

---

## 5. 关键环境坑（本会话已验证，务必先读）

- **git 在沙箱里要绕过**：`git commit`/`git init` 等写操作必须 `dangerouslyDisableSandbox: true`，
  否则提交只落在临时层、磁盘上丢失。
- **`git push` 必须由用户在自己终端跑**：AI 侧非交互 shell 里 GCM 弹不出窗 → 静默卡死 → 被 SIGTERM
  （现象「零输出 + SIGTERM」，极易误判成网络故障）。诊断：`GIT_TERMINAL_PROMPT=0 git -c credential.helper= push`
  会立刻回 `could not read Username` ⇒ 网络正常、只缺凭据，让用户自己推。
- **提交信息用 `git commit -F <临时文件>`**，避免 Git Bash 把中文按 GBK 编码。
- **访问本机服务加 `--noproxy '*'`**；`tasklist /FI` 要配 `MSYS_NO_PATHCONV=1`。
- **PowerShell 工具在本环境不回显 stdout**（exit 0 却无输出），但它执行的副作用是真实的；
  要拿输出就让它 `Out-File` 到临时文件再读。
- **后台长任务用 Bash 工具的 `run_in_background`**（别用 `cmd &`，会被回收）；完成后系统会通知，不用 poll。

---

## 6. 当前状态（交接时）

- **HEAD = `fb4ba8c`** `feat(engine): 重试尊重 HTTP Retry-After（429/503）`（4 文件 +216/−6）。
  近期提交：
  - `a89bf8b` 支持 Windows 长路径（`\\?\` 前缀，`utf8_to_wpath` 统一入口）。
  - `826e1ef` 引擎可靠性优化（文件名清理 / 同名重命名 / 失败文案 / 路径缓冲）。
  - 更早：`src/app/` 的 L2 组件抽取（`auto_power`/`ipc_server`/`schedule_service`/`tray_controller`/
    `queue_scheduler`/`task_controller`/`notification_aggregator`）。
- 工作区干净；**`a89bf8b` + `fb4ba8c` 两笔尚未 push**（等用户）。
- **引擎自测基线 91/0**（演进：16→23→37→40→47→64→77→81→91）。

---

## 7. 下一步候选（挑价值最高的做；这是我的建议排序）

1. **HEAD 探测也重试 + 尊重 Retry-After**：`network_probe`（`network_curl.c`）目前**无任何重试**。
   服务器瞬时限流(429/503)会让探测失败 → `file_size=-1` → 无法分段、退化为单连接下载。
   可复用已实现的 `parse_retry_after`。
2. **（疑似 P0，先核实）截断下载被误判成功**：`range_write` 把「`code==200/206` 且 `ctx.written>0`」
   直接判成功；若服务器提前断流（libcurl 报 `CURLE_PARTIAL_FILE`），可能产出一个"已完成"的**截断文件**。
   与既往"大小对、内容坏"同族。**先核实管理器是否另有字节数兜底**，再决定修法。
3. **限流反馈可见**：429 等待期间在任务行显示「服务器限流，N 秒后重试」，目前是静默等待。
4. 其他：代理 407 认证、跨 host 重定向时的 UA/凭据泄漏、Content-Length 与实际不符的检测。

---

## 8. 关键路径速查

```
src/core/network_curl.c        # 活动 HTTP 引擎（libcurl 实现）★机制改动主战场
src/core/network.h             # 网络层接口（NetOptions / NetworkProbe / parse_retry_after 等）
src/core/download_core.c       # 引擎 + 调度（dlmgr_* API、分片、限速、状态文件）
src/core/download_core.h       # 引擎接口
src/core/legacy/network_winhttp.c  # 旧 WinHTTP 实现（非活动，仅参考）
src/app/*                      # L2 组件（调度/托盘/IPC/通知…）
src/ui/main_window.cpp         # 主窗口（纯视图 + 信号桥）
resources/qss/{light,dark}.qss # 主题
third_party/libcurl/           # vendored libcurl 8.22.0（MinGW 构建）
tools/engine_selftest.c        # 引擎自测（配合下面的服务端）
tools/engine_selftest_server.py# 本地带 Basic 认证的 HTTP 测试服务
tools/ui_snapshot.cpp          # 离屏 UI 快照 / 设置&并发&定时探针宿主
tools/quiet_run.py             # 静默运行（不弹窗、不灌输出）
tools/seam_probe.py            # QDateTimeEdit 接缝指纹检测
tools/ext_shot/run.sh          # 浏览器扩展 UI 回归（无头 Edge）
docs/{ARCHITECTURE,PROJECT_GUIDE,fdm_compare}.md
```

---

## 9. 交付方式（照做）

1. 选定一个优化点 → 写实现。
2. 加/改自测用例（优先抽纯函数单测；端到端用 `tools/engine_selftest_server.py` 造条件）。
3. 复跑验证基线（至少引擎自测；动了 UI 再跑 ui_snapshot 探针）。
4. **突变测试**：注掉修复 → 断言变红 → 还原复绿。
5. 清理后台进程/端口，确认工作区只剩本次改动。
6. `git add <本次文件> && git commit -F <临时文件>`（`dangerouslyDisableSandbox: true`）。
7. 更新记忆文件（`MEMORY.md` / `ENGINE-NOTES.md` / 当日 `YYYY-MM-DD.md`）。
8. 回复用户：结论先行 + 关键数值 + 提交哈希；`push` 提示用户自己在终端跑。
