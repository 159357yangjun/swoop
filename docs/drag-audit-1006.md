# ⑪ A4 拖拽回调审计（只读，2026-10-06）

仓根 `D:/visual studio/lianxi/MFC/idm-next`，分支 master，起点 HEAD `cac4b32`（ahead 152）。
本轮**零代码改动**：`git status --porcelain` 除所有者明示保留的未跟踪文件 `idm-next-glass-ui.html` 外为空，
`git diff cac4b32..HEAD -- src/` 必为空。读数身份：`ui_snapshot.exe sha12=ea506238e63d`（同 ⑩ 终态那枚）。

## 0. 普查方法与"工具没瞎"的证据

先跑正例对照证明 grep 通道是通的：同一套 `--include` 下 `clipboard()` 命中 **5** 行（⑩ 已核实的那三处调用 + include），
所以"拖拽家族命中数少"不是扫描器坏了。逐符号计数（`src/**.{c,cpp,h}`，去注释前）：

| 符号 | 命中 | 位置 |
|---|---|---|
| `setAcceptDrops` | 1 | `main_window.cpp:407` |
| `dragEnterEvent` | 定义 1 + 声明 1 + include 1 | `main_window.cpp:955`、`main_window.h:146`、`:80` |
| `dropEvent` | 定义 1 + 声明 1 + include 1 | `main_window.cpp:962`、`main_window.h:147`、`:81` |
| `QMimeData` | 2 | `main_window.cpp:82`(include)、`:965`(使用) |
| `dragLeaveEvent` / `dragMoveEvent` | **0** | — |
| `startDrag` / `QDrag::exec`（外向拖出） | **0** | — |
| `QMimeData::data(...)` / `html()` / `textList()` | **0** | — |

⇒ **全仓只有一个拖放入口面：`MainWindow` 的入向 drop**。没有子控件重写、没有外向拖出、没有自定义 MIME 读取。
中文侧交叉核对（"拖放/拖入"）另见 `main_window.cpp:549`、`:1901`（空状态插画鼠标透明不拦截拖放 + 文案提示），
均为 UI 说明文字，不构成第二个入口。

## 1. 回调清单与逐条判据

### C-⑪-1 `MainWindow::dragEnterEvent` — main_window.cpp:955-960

```cpp
if (event->mimeData()->hasText() || event->mimeData()->hasUrls())
    event->acceptProposedAction();
```

| 判据 | 结论 | 证据 |
|---|---|---|
| MIME 白名单 | **风险 R1（口径过宽）** | 谓词只有 `hasText() \|\| hasUrls()` ⇒ 任何含纯文本的拖拽都放行：浏览器地址栏拖出的选区、Word/记事本里任意一段文字、聊天软件里的任意句子。下载链接校验被推到下游对话框（见 §2 缓解），门本身不设类型门槛 |
| 大小上限 | **未知 U1** | 这里不看内容长度；`hasText()` 对超大文本同样为真 |
| 路径遍历 | **不适用（此函数不读文件名）** | 只做 accept/reject，无取值 |
| 并发 | **不适用** | 无副作用 |
| 错误处理 | **安全** | 不接受即隐式 ignore（Qt 默认），无崩溃路径 |

### C-⑪-2 `MainWindow::dropEvent` — main_window.cpp:962-989

| 判据 | 结论 | 证据 |
|---|---|---|
| MIME 白名单 | **部分安全** | 只消费 `urls()` 与 `text()` 两种形式（`:965-973`），**从不**调 `mime->data(...)` 取二进制 ⇒ 不存在"拖进来一个字节流直接当文件写"的形状 |
| 大小上限 | **风险 R2** | `url = mime->text().trimmed()`（`:972`）之后**没有任何长度上限**；整篇文档被当 text 塞进来时它是一整个 QString 一路进 `NewTaskDialog::setUrl` → `m_urlEdit->setText(url)`（`new_task_dialog.cpp:setUrl`），单行编辑器吃 MB 级文本既慢又难看，且这条值最终还会交给 URL 解析 |
| 路径遍历 | **已知缺口 G1（继承 ⑧ A1，非新增缺陷）** | 拖**本地文件**时 `urls().first().toString()` 得到 `file:///…`：① scheme 不在对话框白名单内 ⇒ 会被拒（§2），② 但一旦用户把文本改成 `.torrent` 结尾或走 `localTorrent` 分支（`new_task_dialog.cpp:353` `QFile::exists(url)`），dir 参数就来自 `inp.saveDir`；而 **dir 面全链零清洗**是 ⑧ 一手证过的（PATHGUARD G1 转发原样含 `..`、engine[37②] state 恢复原样进队列）。⇒ 拖放不是新漏洞源，但它落在**同一条未修的 dir 面**上 |
| 并发限制 | **安全（一次拖 N 个不会崩，也不会起 N 个任务）** | `:967-970` 明确 `urls.first()` ⇒ 一次 drop 至多产生一个任务，其余 URL 静默丢弃。真正的并发约束在别处：队列任务受 `QueueScheduler` 的 `qMax(1, m_maxConcurrent())` 限（`queue_scheduler.cpp:42,71`，默认 4，`settings.h:174`），而**拖放传 `queue=""` ⇒ 立即开下、绕过 maxConcurrent**（`:983-985` 末参为空串）。这不是崩溃风险，是"设计如此但没人写过断言" |
| 错误处理 | **风险 R3（三面不一）** | 对比其它入口：IPC/web 侧 `addTaskFromUrl` 有 `if (id <= 0) { Log.warn; return; }`（`main_window.cpp:1754-1757`）；剪贴板 Yes 分支只在 `id > 0` 时记 info（`:896-900`）；**拖放这一条只有成功分支**（`:985-988`），`internalAddTask` 返回 ≤0 时既不提示也不记日志，用户只会看到对话框关掉、列表里什么都没出现 |

### C-⑪-3 下游唯一实际执行体：`NewTaskDialog::onAccepted` — new_task_dialog.cpp:336-371

拖放的真实校验发生在这一层（不是 drop 回调里）：

- URL 用 `QUrl::StrictMode` 校验，scheme 白名单为 **http/https**（`parsed.scheme()` 判定，`:348-352`）
  **+ ftp/ftps/magnet/.torrent**（`TorrentDownloader::isAria2Url` → `isTorrentUrl`，`torrent_downloader.cpp:93-113`）。
  ⇒ `file://` 落不进任何一支 ⇒ **拒绝并弹 warning**（`:356-359`）。这是"拖本地文件会不会变成下载/写出"的主护栏。
- 保存目录 `validateSaveDir`（`:303-320`）：`QDir::cleanPath` + 非空 + 不是文件 + 可创建。
  ⇒ **注意口径**：`cleanPath` 会把 `a/../b` 折叠成相对根的正确路径，但它**不是白名单根约束**——
  用户/攻击者给什么绝对路径都接受，这正是 ⑧ 报过的"没有任何一行代码检查 dir 是否落在授权根内"。
- 文件名 `m_nameEdit->text().trimmed()` 原样传给 `internalAddTask`（`:367`）⇒ 引擎侧
  `sanitize_filename_utf8`（`download_core.c:484/1549`）会清 `\ / : * ?`（⑧ [37①] 实测 `..\..\evil_a1.bin` → `.._.._evil_a1.bin`）。

## 2. 汇总表（项 / 分类 / 风险 / 最小验证路径 / 工作量）

分类沿用 ⑦ 的口径：**A=现实影响**（功能/用户可见/安全）、**B=验证完整性**、**C=环境限制**。

| # | 项 | 类 | 一句话风险 | 最小验证路径 | 工作量 |
|---|---|---|---|---|---|
| R1 | `dragEnterEvent` 只看 `hasText()\|\|hasUrls()` | B | 门过宽但下游有 StrictMode 兜底 ⇒ 目前不构成可用漏洞，属"无人断言的口径" | 探针构造 `QMimeData` 三种：纯文本段落 / `file://` URL / `https://x/a.zip`，各发一次 `QDropEvent`，断言 accept 集合 == `{文本, https}`（`file://` 允许 accept 但必须被对话框拒） | 小 |
| R2 | drop 取的文本无长度上限 | A | 拖一篇长文 ⇒ MB 级字符串进单行编辑器与 URL 解析，界面卡顿且无提示 | 夹具：`setText(QString(2*1024*1024,'a'))` 发 drop，断言要么拒（早退）要么截到上限，且 `m_urlEdit->text().length()` 有界 | 小 |
| R3 | 失败静默（`id<=0` 无任何反馈） | A | 用户拖了链接、点了确定、什么都没发生，也无法从日志区分是被拒还是失败 | 夹具：mock/注一个必失败的 url 走同一路径，断言出现一条 warn 日志或状态栏提示（对齐 `addTaskFromUrl:1754` 的形状） | 小 |
| G1 | dir 面零清洗（继承 ⑧ A1） | A | 拖放/新建任务的 dir 不受授权根约束，越界目录原样透传 | 已在册：等 A1 修法授权后由 PATHGUARD G1/G2 + engine[37②] 转正收口（勿重复造夹具） | 中（随 A1） |
| U1 | 拖放路径**零测试覆盖** | B | ⑦ 就登记过 A4"探针零覆盖"，本轮复核仍然为真：`grep -rn "dragEnter\|dropEvent\|QMimeData" tools/*.cpp tools/*.c` = 0 命中 | 新增 `IDM_DRAGDROP_PROBE`：离屏 `QApplication` 下 `new QMimeData` + `setUrls/setText` + `QDropEvent` 发给 MainWindow，断言对话框预填文本 == 拖入 URL（模态需 K3 同款 queued-trigger + 内层轮询，⑨⑩ 已有可抄形状） | 中 |
| U2 | 一次拖 N 个 URL 只取第一个 | B | 行为合理但从未断言；将来有人"顺手改成批量"会同时改变并发语义（queue="" ⇒ 立即开下、绕 maxConcurrent） | 夹具：`setUrls({a,b,c})`，断言仅生成 1 个任务且其 URL==a | 小 |
| C1 | 外向拖出（`startDrag`）不存在 | — | 已查，不是缺陷：全仓 0 命中 ⇒ 无需审计其 MIME 暴露 | 无需 | — |
| C2 | `dragLeaveEvent`/`dragMoveEvent` 未实现 | — | 已查，不是缺陷：Qt 默认忽略即可，无视觉残留风险（窗口级 accept 状态由 Qt 自管） | 无需 | — |

**计数**：入口回调 **2 个**（dragEnter / drop）+ 下游执行体 1 个（对话框 onAccepted）。
按上述表：**安全/已查不是缺陷 3 条**（C-⑪-3 的 file:// 主护栏、C1、C2）；**A 类风险 3 条**（R2、R3、G1）；
**B 类验证缺口 3 条**（R1、U1、U2）。

## 3. 与 ⑦ 审计报告的关系（订正与延续）

`docs/coverage-audit-1006.md:22` 当时把 A4 记为"dropEvent 有实现、无断言"，建议的最小路径正是
"构造 `QMimeData(dropUrls)` 发 `QDropEvent` 给 MainWindow，断言 dlg 预填文本"。本轮复核：
该行**仍然成立、未被做掉**（tools/ 零命中），故 U1 继续沿用它的编号语义，只是把判据从"预填对不对"
扩到"accept 集合 + 失败有没有反馈 + 长度有没有界"三条。

⚠ 一处**需要标注的不一致**（不自行改台账，交回定夺）：`coverage-audit-1006.md:22` 的分类列写的是 **A**，
而 `:61` 的汇总行把「剪贴板监听 / 拖放 / 托盘」整体归到 **否（非 A 类）**。两处对"拖放是不是 A 类"给出相反答案。
我的判断：拖放的**覆盖缺口**是 B（U1），拖放的**具体行为缺陷**（R2/R3）才是 A——两个号不该混用一格。

## 4. 仍然没验证的东西

1. **没有运行时证人**：本轮是静态读码 + 一手 grep，`QDropEvent` 从未真的发给过 MainWindow ⇒
   "拖放能弹出预填好链接的对话框"这句话目前是**推断**，不是观测（U1 就是它）。
2. `file://` 被拒依赖对话框那段代码被执行；我没有实测过拖一个真实桌面文件进窗口的结果。
3. R2 的长度上限影响（单行编辑器吃 MB 文本）是从调用链推的，未测出"多少字节开始卡"。
4. 大文件/多文件的真实并发压力未测：我只证明了"一次 drop 至多建 1 个任务"这个代码事实，
   没有跑过"连拖 100 次"的稳定性。
5. CI 从未执行（push 属所有者边界），本报告与 ⑩ 的新模式一样，都还没有 runner 上的证人。
