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

---

# 第二次：真的用它下了一条（2026-10-01 21:58→21:59，pid 23968）

上面那次只是"起得来 + 交接"。这次用 computer-use 真操作了它：点「新建任务」→ 填 URL → 改保存目录 → 点「确定」，
任务跑完并**逐字节核了盘**。

## 操作序列（每步都有回读，不是"我点了"）

| 步 | 动作 | 回读到的证据 |
|---|---|---|
| 1 | `activate_window`（窗口原先被最小化） | `restored:true wasMinimized:true foregroundStatus:"matched"`，bounds 1193×756 |
| 2 | 点「新建任务」(60,91) | 弹出 `QApplication.NewTaskDialog`，元素树里出现 下载链接/保存目录/文件名/线程数/确定/取消 |
| 3 | `type_text` 填 URL | 元素 5 `Value:"http://idmuser:idmpass@127.0.0.1:18080/small.bin"` ✓ 48 字符全对 |
| 4 | `type_text` 改保存目录 | **掉了两个字符**：`idm-gui-215114` → `idm--ui--15114` |
| 5 | 改用 `set_value` | `verification:"matched"` `verifiedValue:"C:/Users/yyyy/AppData/Local/Temp/idm-gui-215114/dl"` ✓ |
| 6 | 点「确定」 | 第一次点到了 `QDialogButtonBox` 组（元素 11）⇒ 对话框没关；改点元素 12 才提交 |
| 7 | 结果 | 表格出现 `small.bin / 已完成 / 2.66 MB/s / 10-01 21:59 / 127.0.0.1`，侧栏 分类 1·其他 1·已完成 1，状态栏 `任务: 1 下载中: 0` |

## 盘上核账（这才是"能不能用"的答案）

```
文件: %TEMP%\idm-gui-215114\dl\small.bin
size = 1048576  期望 1048576           ⇒ 长度对 True
逐字节与服务器载荷 (bytes(i % 251)) 全等 ⇒ True
sha256 前 16 位：下载文件 631b84027d6b9e52 == 独立重算的期望载荷 631b84027d6b9e52 ⇒ 相等
  （两套实现：① 读文件与 exp 逐字节比较；② 分别对 exp 与落盘文件求 sha256 再比——不共用同一次判断）
服务端 /_stats: auth_ok=10  ranges=9  max_cur=8
```
> 订正：这一节初稿在这里写的是 `be2099c214f63a3c`——那是上一节里 **aria2c.exe 拷贝**的哈希，
> 被我错当成下载文件的哈希抄进来了。下载文件的真值是 `631b84027d6b9e52`（上面两行是它的来历）。
⇒ **8 线程并发 Range 真的发生了**（`max_cur=8`），URL 里的 Basic 凭据被 GUI 这条路正确带上（`auth_fail=0`），
产出文件不是"大小对但内容错位"那一类——正是 P0 修的那件事，在真实用户路径上第一次端到端过。

## 三条只有真操作才会撞见的观察

1. **这台机器的输入不是独占的。** 第 4 步 `type_text` 丢了 `g` 与 `2`；而且我启动应用后、还没操作之前，
   它的 stderr 里已经出现 `[INFO] "视图缩小: 12pt" → "11pt" → "视图放大" → "视图缩放重置"`
   ——**不是我发的**（那时我连窗口 id 都还没拿到）。同机有四路会话在跑，键鼠事件会落到当前焦点窗口。
   ⇒ 结论：在这个盒子上做 UI 自动化，**每个写入都必须回读校验**（`set_value` 的 `verification:"matched"` 就是干这个的），
   不能"我输入了 X"就当"框里是 X"。
2. **`大小` 列在已完成任务上显示 `-`**（截图与 UIA 树都是 `-`，而文件实际 1,048,576 字节）。
   这是 P2（进度字段对已完整落盘的下载不报数）**第一次在真实 UI 上被看见**，不是引擎读数里的推断。
3. **UIA 的组节点会吃掉点击**：`QDialogButtonBox` 是元素 11、`确定` 是元素 12，点 11 返回 `succeeded`
   但对话框不关——**"调用成功"不等于"点中了要点的控件"**，得看后置状态（我第二次带 `post_action_state` 才发现）。

## 收尾

服务器（我起的，占 127.0.0.1:18080）与 app 实例（pid 23968）在取证后关闭：
应用实例**必须**关——它是单实例门闩的持有者，留着的话用户自己打开 IDM Next 会交接到我这个
用临时数据目录的实例上，看到的是我造的假任务。取证文件保留在 `%TEMP%\idm-gui-215114\`。
