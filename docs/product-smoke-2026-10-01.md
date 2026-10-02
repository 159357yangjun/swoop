# 产品 exe 本机冒烟（第一次真的跑它）— 2026-10-01 21:36

被测：`D:\visual studio\lianxi\MFC\idm-next\build\idm-next.exe`；`exe_sha12=fe0f82754e16`，mtime 2026-10-01T15:49:23，size 2124565
（就是第 20 条那笔重编产出的**新**二进制，不是 `45bbbab03006`）。
跑法：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/idm_next_smoke.ps1`

## 脚本原文输出（逐字）

```
== EXE D:\visual studio\lianxi\MFC\idm-next\build\idm-next.exe
== exe_sha12=fe0f82754e16 mtime=2026-10-01T15:49:23 size=2124565
== ISOLATED_DATA_DIR C:\Users\yyyy\AppData\Local\Temp\idm-smoke-1001-213619
== INSTANCE1 pid=7648 alive_after_5s=True
== INSTANCE2 pid=29276 exited_within_15000ms=True exitcode=(unreadable) (blank or unreadable is NOT the same as 0)
== TRIAGE instance1_stderr_bytes=494 mentions_listening_name=True mentions_activate=True instance2_stderr_bytes=0
== DATA_DIR contents the app created (all inside the isolated dir):
       12288  \history.db
         558  \inst1.err
           0  \inst1.out
           0  \inst2.err
           0  \inst2.out
     5649408  \aria2\aria2c.exe
SMOKE_OK exe_sha12=fe0f82754e16 logdir=C:\Users\yyyy\AppData\Local\Temp\idm-smoke-1001-213619
== CLEANUP pid=7648 terminated_within_1s=True
== CLEANUP pid=29276 terminated_within_0s=True
```

## 实例#1 的 stderr（GBK→UTF-8 解码后逐字）

```
warning: Environment variable QT_LOGGING_TO_CONSOLE is deprecated, use
QT_ASSUME_STDERR_HAS_CONSOLE and/or QT_FORCE_STDERR_LOGGING instead.
[INFO] "IDM Next 启动（GUI 模式）"
QSystemTrayIcon::setVisible: No Icon set
[INFO] "已加载主题: light"
[INFO] "主窗口初始化完成"
[INFO] "HistoryStore 已打开: C:\\Users\\yyyy\\AppData\\Local\\Temp\\idm-smoke-1001-213619/history.db"
[INFO] "IPC server 已启动，监听 idm-next-ipc"
[INFO] "BT/磁力组件 aria2c 已就绪（开箱即用）"
[INFO] "IPC 请求: activate → 成功（排队 0ms，处理 16ms，送达=1，回话时客户端=在）"
```

## 这次验到了什么（每条都指到上面一行）

1. **新二进制能起**：`INSTANCE1 alive_after_5s=True`，且 Qt 日志走完启动序列到 `主窗口初始化完成`。
2. **IPC 真的在监听**：`IPC server 已启动，监听 idm-next-ipc`。
3. **单实例交接成功**：第二个实例自己退了（`exited_within_15000ms=True`），而第一个实例的 stderr 里出现
   `IPC 请求: activate → 成功（排队 0ms，处理 16ms，送达=1，回话时客户端=在）`
   ⇒ 这正是 CI 那段注释里写的"两句都有 = 交接成功"的分诊形状，本机第一次拿到。
4. **数据隔离有效**：`HistoryStore 已打开: …\idm-smoke-1001-213619/history.db` ⇒ 没碰 %LOCALAPPDATA%，
   依据是 `src/utils/app_paths.cpp:14-19`（设了 `IDM_DATA_DIR` 就不碰注册表）。
5. **aria2c 那 5.6MB 不是下载来的**：隔离目录里的 `aria2\aria2c.exe` 与用户缓存
   `C:\Users\yyyy\idm_shots\_data\aria2\aria2c.exe` **sha256 前 16 位相同（`be2099c214f63a3c`）**，
   而用户那份 mtime 仍是 2023-11-15（我没动过他的缓存）。⇒ 是从本地缓存拷进数据目录的。

## 顺手量到的三条产品侧观察（只记不修，等定）

- `QSystemTrayIcon::setVisible: No Icon set` ⇒ 托盘项是在**没有图标**的情况下 setVisible 的；
  Windows 上这会表现为托盘里一个空白/不可见条目。具名证据就是上面那行 Qt 警告，出现在
  `主窗口初始化完成` 之前、`已加载主题` 之后。这是外观/可发现性缺陷候选，不是崩溃。
- `QT_LOGGING_TO_CONSOLE is deprecated`（Qt 6.11）⇒ `ci.yml` 与我这份脚本都靠它把日志引到 stderr；
  今天仍然生效（否则 `instance1_stderr_bytes=0`），但后继开关是 `QT_FORCE_STDERR_LOGGING`。
  记在这里，等它真失效时不用重新考古。
- `exitcode=(unreadable)`：PS 5.1 从 `Start-Process -PassThru` 读不到已退进程的 ExitCode。
  脚本把空值显式印成 `(unreadable)` 而不是 0——**空白不等于通过**；交接成立与否由第 3 条那行 activate 作保。

## 我自己在这份脚本上踩到的两个坑（都修在脚本里，不是修在读数上）

1. **PowerShell 5.1 把无 BOM 的 UTF-8 当 ANSI 读**：第一版脚本里写了中文提示，解析在第 30 行炸
   （`$(subexpression) 缺少 '}'`），而且第二次我把中文留在字符串里时，`INSTANCE2` 那行直接从我的 grep 结果里消失了。
   ⇒ 现在文件头写着 ASCII-only，脚本里 0 个非 ASCII 字符（`python` 逐字符核过）。
2. **一次采样就把"还在"当结论**：第一次跑印了 `CLEANUP_WARN pid=20860 still alive`，
   但复核 `tasklist //FI "PID eq 20860"` 与 `IMAGENAME eq idm-next.exe` 都是"没有匹配的任务"
   ⇒ 那是 1 秒采样太紧，不是没杀干净。现在轮询到 10 秒并印最终状态（本轮 `terminated_within_1s=True`）。

## 收尾

日志目录 `C:\Users\yyyy\AppData\Local\Temp\idm-smoke-1001-213619` 里的 `history.db` 与 5.6MB `aria2` 拷贝
由脚本自己删（它造的），两份 `.err/.out` 留下归档后由本节记录替代，目录已删。
盘上没有我起的残留 `idm-next.exe`（`tasklist` 两条查询都无匹配）。
