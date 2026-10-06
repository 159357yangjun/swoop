# ⑫ A4 R2/R3 修复 — 测具、红基线与四面变异（2026-10-06）

仓根 `D:/visual studio/lianxi/MFC/idm-next`，起点 HEAD `1b767c6`。
产品改动只在 `src/gui/main_window.cpp`（**+18 行、0 删除**），探针在 `tools/ui_snapshot.cpp`（新模式 `IDM_DRAGDROP_PROBE`）。

## 复算命令

```bash
ninja -C build ui_snapshot idm-next
sha256sum < build/ui_snapshot.exe | cut -c1-12
QT_QPA_PLATFORM=offscreen IDM_DRAGDROP_PROBE=1 ./build/ui_snapshot.exe; echo RC=$?   # 期望 RC=0、三条 PASS
```

## 改了什么（R2 / R3）

| 项 | 落点 | 形状 |
|---|---|---|
| R2 | `main_window.cpp` dropEvent，取到 url 之后、开对话框之前 | `url.length() >= sizeof(DownloadTask.url)` ⇒ `Log::warn("拖放内容过长（N 字符）…")` + `acceptProposedAction()` + return。**上限不新造常量**：直接取引擎自己的 URL 缓冲（`download_core.h:67 char url[2048]`），超过它的输入本来就会被 `strncpy` 截成另一个 URL——静默截半不如明确拒掉 |
| R3 | 同函数成功分支之后补 `else` | `Log::warn("拖放添加任务失败: %1")`，措辞对齐 `addTaskFromUrl`（IPC/web 侧）里既有的 `"IPC 添加任务失败: %1"` |

## 三条断言（先红后绿）

| 断言 | 判据 | 修前 | 修后 |
|---|---|---|---|
| D1 正例对照 | 正常 URL 拖入 ⇒ 恰好一个对话框、预填逐字==URL、且**被点确定 accept** | PASS | PASS |
| D2 (R2) | 3 MiB 文本 ⇒ 不得整串交给编辑器（没开对话框或文本 ≤8192）**且**有一条含「拖放」的 warn | **FAIL**（`交给对话框的长度=32767`） | PASS（`长度=0 峰值=0 告警=1`） |
| D3 (R3) | 引擎满额时拖合法 URL ⇒ `internalAddTask` 返 -1 ⇒ 必须有含「添加任务失败」的 warn | **FAIL**（无 warn） | PASS（`warn#1 "拖放添加任务失败: …"`） |

D3 的满额前置是**现场制造并可复算**的：`dlmgr_add` 灌到拒绝（实测 `首个拒绝下标=127`、`dlmgr_list=128`，
等于 `MAX_TASKS`），不靠"大概会失败"。跑完 `dlmgr_cancel` 逐个收摊。

## 四面变异（各咬一条合取项，互不重叠）

每面都：改前 `assert count==1` → 改后 grep 确认标记串已在盘上 → ninja 重编 → 读 exe 自印的 sha12 → 跑探针。

| 变异 | 做法 | exe_sha12 | 结果 | 归档 |
|---|---|---|---|---|
| M-⑫A 摘掉 R2 守卫 | `if (false && url.length() >= kMaxDropUrlLen)` | `bd2f47f626b7` | **只 D2 红**，读数回到 `长度=32767` | `mut-12A-guard-removed.txt`（`286e7ef4f1ad`） |
| M-⑫B 拒了但不告警 | 删掉那条 `Log::warn`，保留 return | `33bed21725d5` | **只 D2 红**，`有界或拒=1 但 告警=0` ⇒ 证明 D2 是两个合取项而不是一层皮 | `mut-12B-silent-reject.txt`（`105bcbb25a54`） |
| M-⑫C 失败支不告警 | `Log::warn(…失败…)` → `Log::info(…)` | `3f9aeab16488` | **只 D3 红** | `mut-12C-no-warn-on-fail.txt`（`56001ae3c6d2`） |
| M-⑫D 成功判据恒真 | `if (id > 0)` → `if (true)` | `05cac1c3dc53` | **只 D3 红**（失败被当成成功记 info） | `mut-12D-success-always.txt`（`d572d30f317d`） |
| 还原复绿 | 逐字节回写修复版 | `57568c3f8423` | RC=0、三条 PASS | `green-after-fix-1006.txt`（`c6d0d027d2c3`） |

⚠ **为什么探针现在把自己的 sha12 印进输出**：第一轮 M-⑫C 与 M-⑫D 两份归档**字节完全相同**
（`65af65b70fd5` == `65af65b70fd5`）——两面红的是同一个判据、读数一字不差，光看日志根本分不出是哪枚二进制跑的。
加了 `[DRAGDROP 身份] exe_sha12=` 后每面原文都能独立指向一枚产物；该哈希与外部
`sha256sum < build/ui_snapshot.exe | cut -c1-12` 当场对得上（同一算法）。

## 过程中被实测驳回的四条（都是测具的错，不是产品的错）

1. **直接 `sendEvent(drop)` 会挂死**：`dropEvent` 里 `dlg.exec()` 阻塞，控制权永远进不到采样器
   ⇒ 第一版卡十分钟。改成 K3 同款：触发排进队列 + 外层 `QEventLoop` + 挂在 qApp 上的守卫定时器。
2. **不发 `dragEnterEvent` 就没有 drop 语义**：真实拖放里 drop 之所以送达，是因为 enter 阶段被 accept 过。
   第一版跳过 enter ⇒ D1 正例对照也红（门是绿的、只是我没敲对门）。
3. **守卫用 `reject()` 关对话框 ⇒ D3 永远看不到失败支**：`reject()` 让 `exec()` 返回 Rejected，
   `internalAddTask` 根本不执行。必须**点 OK** 走 `onAccepted → accept()`。
   （顺带踩到 `NewTaskDialog::result()` 把 `QDialog::result()` 覆盖成返回 `NewTaskInput` ⇒ 取状态得走基类。）
4. **`w.findChild<TaskListModel*>()` 恒为 nullptr**（模型是成员不是子 QObject），且"立即开始"的任务占并发槽、
   并不都进引擎队列计数 ⇒ 靠 GUI 灌不满 128。改为直接按引擎口径 `dlmgr_add`/`dlmgr_list` 制造满额。

另外两次工具性错误也记下来：`python - <<PY` 里的 `\n` 会被字符串转义吃掉，造成 C++ 源码里出现裸换行的字符串字面量
（本会话第三次同类，已进长期记忆）；以及 `timeout 120 env …` 那次给出 **RC=0 且零输出**——
`timeout` 本身在这套 MSYS 工具链上的退码不可信，判"跑过了"必须看有没有断言行，不能只看退码。
