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
| R1 | `dragEnterEvent` 只看 `hasText()\|\|hasUrls()` | **已查不是缺陷**（⑬ 定案） | 门过宽但下游有 StrictMode 兜底 ⇒ 不构成可用漏洞；理由见 §4-R1 | 探针构造 `QMimeData` 三种：纯文本段落 / `file://` URL / `https://x/a.zip`，各发一次 `QDropEvent`，断言 accept 集合 == `{文本, https}`（`file://` 允许 accept 但必须被对话框拒） | 小 |
| R2 | drop 取的文本无长度上限 | A（**已修 ⑫**） | 拖一篇长文 ⇒ MB 级字符串进单行编辑器与 URL 解析，界面卡顿且无提示 | 夹具：`setText(QString(2*1024*1024,'a'))` 发 drop，断言要么拒（早退）要么截到上限，且 `m_urlEdit->text().length()` 有界 | 小 |
| R3 | 失败静默（`id<=0` 无任何反馈） | A（**已修 ⑫**） | 用户拖了链接、点了确定、什么都没发生，也无法从日志区分是被拒还是失败 | 夹具：mock/注一个必失败的 url 走同一路径，断言出现一条 warn 日志或状态栏提示（对齐 `addTaskFromUrl:1754` 的形状） | 小 |
| G1 | dir 面零清洗（继承 ⑧ A1） | A | 拖放/新建任务的 dir 不受授权根约束，越界目录原样透传 | 已在册：等 A1 修法授权后由 PATHGUARD G1/G2 + engine[37②] 转正收口（勿重复造夹具） | 中（随 A1） |
| U1 | 拖放路径**零测试覆盖** | B（**已登记，待后续 ⑬**） | ⑦ 就登记过 A4"探针零覆盖"，本轮复核仍然为真：`grep -rn "dragEnter\|dropEvent\|QMimeData" tools/*.cpp tools/*.c` = 0 命中 | 新增 `IDM_DRAGDROP_PROBE`：离屏 `QApplication` 下 `new QMimeData` + `setUrls/setText` + `QDropEvent` 发给 MainWindow，断言对话框预填文本 == 拖入 URL（模态需 K3 同款 queued-trigger + 内层轮询，⑨⑩ 已有可抄形状） | 中 |
| U2 | 一次拖 N 个 URL 只取第一个 | B→A（**已修 ⑬**） | 行为合理但从未断言；将来有人"顺手改成批量"会同时改变并发语义（queue="" ⇒ 立即开下、绕 maxConcurrent）。⚠ 括号里那句是**错的**，订正见 §4-U2 | 夹具：`setUrls({a,b,c})`，断言仅生成 1 个对话框、且三条都进引擎队列（判据随修复改向，见 §4-U2） | 小 |
| C1 | 外向拖出（`startDrag`）不存在 | — | 已查，不是缺陷：全仓 0 命中 ⇒ 无需审计其 MIME 暴露 | 无需 | — |
| C2 | `dragLeaveEvent`/`dragMoveEvent` 未实现 | — | 已查，不是缺陷：Qt 默认忽略即可，无视觉残留风险（窗口级 accept 状态由 Qt 自管） | 无需 | — |

**计数**：入口回调 **2 个**（dragEnter / drop）+ 下游执行体 1 个（对话框 onAccepted）。

按上述表，**口径分两层数**（⑬ 定稿；原先把两类混在一个"风险条数"里就是 §3 那个矛盾的根源）：

| 口径 | 条数 | 明细 |
|---|---|---|
| **A 类 = 现实影响的行为缺陷** | **3** | R2（已修 ⑫）、R3（已修 ⑫）、G1（未修，随 A1） |
| **B 类 = 验证完整性缺口** | **2** | U1（已登记，待后续 ⑬）、U2（覆盖缺口侧；修复见下） |
| **已查不是缺陷** | **4** | R1（⑬ 定案，理由 §4-R1）、C1、C2、file:// 主护栏（§1 C-⑪-3） |

⚠ U2 在 ⑬ 被**实现层修掉**（拖 N 条只下 1 条 ⇒ 现在全收），所以它同时是"A 类行为缺陷已修"和
"B 类覆盖缺口由 D4/D5 补上"。两个号仍分开记：**修的是产品，补的是证人**，不能因为修了就宣布 B 类清零。
G1 一条不进本轮计数面（未动，等 A1 授权）。

## 3. 与 ⑦ 审计报告的关系（订正与延续）

`docs/coverage-audit-1006.md:22` 当时把 A4 记为"dropEvent 有实现、无断言"，建议的最小路径正是
"构造 `QMimeData(dropUrls)` 发 `QDropEvent` 给 MainWindow，断言 dlg 预填文本"。本轮复核：
该行**仍然成立、未被做掉**（tools/ 零命中），故 U1 继续沿用它的编号语义，只是把判据从"预填对不对"
扩到"accept 集合 + 失败有没有反馈 + 长度有没有界"三条。

⚠ 一处**需要标注的不一致**（不自行改台账，交回定夺）：`coverage-audit-1006.md:22` 的分类列写的是 **A**，
而 `:61` 的汇总行把「剪贴板监听 / 拖放 / 托盘」整体归到 **否（非 A 类）**。两处对"拖放是不是 A 类"给出相反答案。
我的判断：拖放的**覆盖缺口**是 B（U1），拖放的**具体行为缺陷**（R2/R3）才是 A——两个号不该混用一格。

**⑬ 定案（本节从"待裁"转为"已按上述口径订正"）**：矛盾按上面那张两层表消解，即
`:22` 那格的 A 指的是**行为缺陷面**（R2/R3，已修），`:61` 汇总行的"否"指的是**覆盖缺口面**（U1/U2 = B）。
两格都不算写错，错的是它们共用了同一个词"类"。`coverage-audit-1006.md` 本体不改写（那是 ⑦ 的一手记录），
订正只落在本文件 §2 这张表；下批若重开 ⑦ 台账，应以本表为准回填。

## 4. ⑬ 决策记录（R1 / U2 / U1）

### R1 —— 定案「已查，不是缺陷」（不改代码）

`dragEnterEvent` 的谓词确实宽（只看 `hasText() || hasUrls()`），但"宽"要造成用户可见的伤害才需要修。
本轮把下游那段**实测调用链**读完，三个理由合起来足以让这条降级：

1. **纯文本进不了引擎**。拖进来的串最终交给 `NewTaskDialog::onAccepted`，那里用
   `QUrl(url, QUrl::StrictMode)` 校验，只放过三支：`http/https` 且 host 非空
   （`new_task_dialog.cpp:350-352`）、`TorrentDownloader::isAria2Url`（magnet/ftp 系，:346）、
   以及**磁盘上真实存在**且以 `.torrent` 结尾的路径（:353-354）。三者都不满足 ⇒
   QMessageBox 明确说明"链接格式不正确"并 `return`（:355-358），任务一条也不会建。
   所以"记事本里拖一段散文"的结果是**一次可理解的拒绝**，不是静默错误下载。
2. **R2 已经把唯一的物理伤害面关掉**。原先真正的代价是 MB 级字符串进单行编辑器；
   现在超长条目在进入对话框之前就被拒并留一条 warn（`main_window.cpp` R2 段），
   门过宽不再附带界面卡顿这个后果。
3. **收窄谓词会误伤真实用法**。若在 `dragEnter` 就要求 http/https，那么用户在对话框里
   还能合法使用的 magnet / ftp / `.torrent` 三条支路会在"能不能放手"这一步被砍掉——
   那是把下游已经做好的分类工作复制一份到上游，两处规则从此必须同步维护。

⚠ 边界写清楚：这条定案证到的是"**accept 之后不会造成错误下载或卡界面**"，
它**没有**证明"任何文本拖进来都有好体验"（会弹一个错误框，这是设计态而非缺陷）。
另外 §4-2 那句仍然成立：**我没有实测过拖一个真实桌面文件进窗口**，`file://` 一支是靠
`.torrent` 后缀那条判断推出来的。

### U2 —— 最终决策：**遍历全部 + 只弹一个对话框**（不采用"取第一个 + 告知忽略"）

任务书给了两个候选，我选前者，理由按用户实际会撞到的那一次动作排：

- **"拖 10 个链接只下 1 个、而且不告诉我另外 9 个没了"本身就是缺陷**。备选方案（取第一条 +
  `Log::info` 说忽略了其余）只是把这个缺陷改成"更礼貌的缺陷"：日志不在主界面上，
  用户看不到，行为仍然是丢数据。
- **扰民的那个成本其实不存在**：批量路径**不弹第二个对话框**。目录与线程数取用户在
  同一个对话框里确认过的值，文件名一律留空让引擎按各自 URL 推断（手填的名字不该复用给别的 URL）。
- **并发语义没有被绕过**（同时订正 §2-U2 原先那句括号里的错话）：批量走的是
  `internalAddTask(..., queue="")`，而控制器用的是
  `const bool startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot();`
  （`task_controller.cpp:101/153/173/214/261`），`hasFreeSlot()` 拿运行数比
  `qMax(1, m_maxConcurrent())`（`queue_scheduler.cpp:38-48`，默认 4，`settings.h:174`）。
  ⇒ `queue=""` 表示"不受队列归属限制"，**不是**"不受并发上限限制"；满额的任务留在列表里等位。
  我上一轮写的"立即开下、绕 maxConcurrent"是错的。
- **每条都留痕**：成功走 `Log::info("拖放添加任务 #N: url")`，失败走 R3 那条 warn，
  批次结束再补一条 `拖放批次完成: added/total`。重复 URL（同一条在文本里出现两次）跳过不重建。
- **仍受 R2 保护**：长度上限现在是**逐条**判的，超长的那几条单独计数并在批次层面报一次
  「有 N 条内容过长」，不会因为一条坏数据把整批丢掉。

### U1 —— 已登记，待后续（本轮不做）

`IDM_DRAGDROP_PROBE` 已经在真发 `QDropEvent`/`QDragEnterEvent`，所以 U1 的字面判据（"零覆盖"）
事实上已被 D1~D5 消掉一部分；但 U1 原意还包含**真实桌面文件的拖入**（`file://` 一支）与
跨进程 drop，这两点 offscreen 夹具给不了。故保留编号、标"已登记，待后续"，不虚报为已完成。


## 5. 仍然没验证的东西

1. **⑪ 那轮**没有运行时证人（静态读码 + grep）。⑫⑬ 之后这一条只对部分句子仍然成立：
   D1~D5 已经真发过 `QDragEnterEvent`/`QDropEvent`，"拖放会弹出预填对话框""超长被拒并有告警"
   "失败有 warn""批量全收且逐条留痕"都有 offscreen 证人。**仍然是推断的**：真实桌面文件拖入、
   真实浏览器地址栏 drop、以及跨进程（资源管理器 → idm.exe）那一段——见第 2 条。
2. `file://` 被拒依赖对话框那段代码被执行；我没有实测过拖一个真实桌面文件进窗口的结果。
3. R2 的长度上限影响（单行编辑器吃 MB 文本）是从调用链推的，未测出"多少字节开始卡"。
4. 大文件/多文件的真实并发压力未测：D4 只证明了一次 drop 的 3 条都进引擎并排队，
   **没有**跑过"连拖 100 次"的稳定性，也没有验证满额之后排队的 3 条最终会不会各自下完。
5. CI 从未执行（push 属所有者边界），本报告与 ⑩ 的新模式一样，都还没有 runner 上的证人。


