# ⑦ 测试覆盖总盘点与延迟项审计（2026-10-06）

**读数身份**：仓根 `D:\visual studio\lianxi\MFC\idm-next`，HEAD `85224ab`（ahead 136，未 push）。
本轮**零代码改动**（纯审计；`git status --porcelain -- src tools` 为空由提交前后各查一次自证）。
⚠ 本文件自身是 git 跟踪件 ⇒ 它的入库会让 NOTICE 的 docs/ 计数+字节数变化，**不得声称七道门在
"含本报告的树"上全绿**——那要等复签轮（见 §6）。

**取证范围**：台账 §7 全部「仍然没验证的东西」段（第 24–29、56–61 条，即 ①–⑥ 及直接前史），
REFACTOR_PLAN.md 战略清单，以及本轮现场 grep/读码复核（每条都标了出处；凡"已查，不是缺陷"也如实入表）。
分类定义：**A=有现实影响**（功能缺失/用户可见/安全相关）、**B=验证完整性**（覆盖度不够但功能本身可能没问题）、
**C=环境限制**（offscreen vs 真窗口、CI、本机焦点等外部约束）。工作量档：小≤半小时可闭环，中=需新测具面或跨层接线，大=需产品级改动或装机/出包类被禁动作。

---

## 1. 汇总表（去重后 22 项）

| # | 项 | 类 | 风险（一句话） | 最小验证路径 | 量 | 出处 |
|---|---|---|---|---|---|---|
| A1 | IPC `filename`/`dir` **无任何清洗**（扩展/host/GUI 三入口同一通道） | **A-安全** | 恶意或被劫持的扩展可向任意路径写文件（路径遍历/覆盖）；引擎只映射磁盘满错误，无写前校验 | 在 `task_controller.cpp` 入口加负例夹具：`dir="../../Windows"` 必须拒或 `cleanPath` 收敛到白名单根；先红归档再修 | 中 | 本轮 grep：ipc_server.cpp:215-224、engine_selftest 无 traversal 项（已查，确实没有） |
| A2 | HashVerifyDialog **零测试**（入口链完整：详情对话框:263 构造并 exec，main_window:1114 起详情框） | **A** | 战略清单打 [x] 的"文件完整性校验"没有任何已知向量对拍——哈希算错/大文件分块边界都不会被发现 | 给 hash 计算函数补 engine_selftest 已知向量用例（空文件/1MiB/跨块 + NIST 标准串） | 小 | 本轮读码修正初稿误判（详见 §2-A2）；tools/ 对 hash_verify 零命中 |
| A3 | 剪贴板监听自动捕获链接：功能在、断言无 | A | 用户每天碰的路径；若 Qt 版本升级打断信号链，静默失效无人知道 | offscreen 下 `QApplication::clipboard()->setText(url)` + 泵事件 + 断言对话框/任务出现 | 小 | REFACTOR_PLAN 阶段5 [x]；ui_snapshot 无 clipboard 断言（本轮实测 grep 0 命中） |
| A4 | 拖放 URL 预填 NewTaskDialog：dropEvent 有实现、无断言 | A | 同上——真实用户路径无证人 | QTest-free：构造 QMimeData(dropUrls) 发 `QDropEvent` 给 MainWindow，断言 dlg 预填文本 | 小 | main_window.cpp:962-967；探针零覆盖 |
| A5 | 托盘"新建任务"回调（main_window.cpp:211）无断言 | A | 托盘菜单是关窗后的主交互面；回调断线＝点了没反应 | K1 闭环同款形状：直接调 controller 回调并数 onNewTask 计数钩子（⑤ 已交付该钩子） | 小 | 本轮读码；K1 的 IDM_NEWTASK_COUNTFILE 可直接复用 |
| A6 | 首次安装 aria2c/ffmpeg 引导冻结（数秒~数十秒）改前后无量化 | A(边界C) | 用户首启体验；受"不装机"边界，只能半真测 | 组件自愈四调用点之一改为可注入路径计时（不动产品逻辑），印"跳过下载因缓存已在"的耗时对比 | 中 | §7 第 24 条清单第 1 项 |
| B1 | 干净 ACTIVATE 面 1~2 发 ~300ms 归属未定（bisect 逐个摘都不减半） | B | 可能是多路并存 ⇒ [census] 已明说这个 0 无人证得过，不影响判据成立但不影响"产品空闲"这句话 | 双源同注 canary 差分（一次注两路、看减半模式）或延长窗口统计显著性 | 中 | §7-57/§7-61 |
| B2 | K2 a)/b) 各只被 M-B 一种破坏形态证明 | B | 换形态未必红 ⇒ "有牙"覆盖面窄 | 再设计 1~2 个独立破坏面（如摘 default 按钮 setDefault 之外的 autoDefault 组合） | 小 | §7-58 |
| B3 | K1 段② 只演示 context 降级一种形态；witness 钩子"未设即跳过"靠读码 | B | 同上 | 补一个"context 改成 Widget"以外的第二形态（如摘 setShortcutContext 整行） | 小 | §7-60 |
| B4 | capture 卡片：取消/ESC 语义、多标签并发弹卡、非默认窗口尺寸截断 | B | 卡片可用性边缘；E2E 只走了 happy path 一档尺寸 | capture_e2e.py 加三条判据（取消后 toast 不出现；两 tab 先后弹卡各自关闭；viewport 缩放 150% 重测几何） | 中 | §7-59 |
| B5 | HLS：三档以上 master、并列 BANDWIDTH 不同 RESOLUTION（UI 出两条同值选项、引擎取首个命中） | B | 真站存在并列档时用户看到两个同名选项、下到哪个不确定 | hls_variant_logic_test.py 夹具参数化到三档+并列档，L2c/L1e 自然扩面 | 小 | §7-61 |
| B6 | host listHlsVariants 的 8s 超时分支与网络错误文案无测具 | B | 失败路径文案错/分支不可达不会被发现 | IDM_HOST_LIST_JSON 指到一个 accept-but-never-respond 的本地端口 ⇒ 断言 success=false+message 形状 | 小 | §7-61 |
| B7 | yt-dlp 格式列表一侧（非 .m3u8）数据层无判据 | B | 画质下拉的另一半走 yt-dlp -F 解析，本仓从未验过其生成 | 需要 yt-dlp 在场（用户缓存已有）⇒ 薄封装一层可注入解析函数再谈；或登记为"依赖外部制品，不进闸门" | 中 | §7-61 范围界定 |
| B8 | `[36②]` 变异不占功："声明值不被盘上数字换掉"只是代码写着 `<=0` | B | 半坏自检比没有更坏的邻域：这条绿灯不代表那条有牙 | 造"盘上>声明"夹具（截断后补齐又超长？需 Range 语义设计）或承认永不可判别并撤旗 | 中 | §7 第 24 条 |
| B9 | D3 `end=-1` 来源未穷举（state 手工改坏等） | B | 未知来源走进同一分支可能带出新形状 | storage.c 载入面 fuzz 一轮 JSON 字段组合（离线，不碰产品） | 中 | §7 第 24 条 |
| B10 | 3263ms 事件未复现、`[attr]` 三面从未同时拿到 | B | 偶发顶格延迟归因悬案；不影响门（预算轮数制已吸收） | 长窗口采样（套件外单独跑 10 分钟）留一手分布 | 中 | §7 第 24 条 |
| C1 | windows 平台（真窗口管理器）Ctrl+N 未测 | C | offscreen 已证可达（⑤），真 WM 差异只剩激活时序 | 前台焦点让位时跑一次 KEYNAV 面 `QT_QPA_PLATFORM=windows`（受本机焦点约束，用户在场时 5 分钟） | 小 | §7-60 |
| C2 | CI 从未在 runner 执行（136 笔未推，push 属用户边界） | C | 所有门的"第一次真实执行"还没发生；本地全绿对新加门无证明力 | 无本地通道——只能等用户 push；届时优先读 `check-runs/{id}/annotations` | — | 每轮台账固定项 |
| C3 | 像素证人受本机焦点约束（headless 渲染帧≠前台窗口） | C | 视觉回归只能证 DOM 几何，不能证观感 | 与 C1 同场人工看一眼；不新增自动化 | 小 | §7-59/§7-60 |
| C4 | 真 GUI 与 offscreen 探针共名管道 `idm-next-ipc`（host 硬编码管道名） | C(A? ) | 并行会话/残留实例互摘 endpoint ⇒ 假红/假绿都在这条路上；套件内已由 ⑤ 的 IDM_TEST_PIPE 扩面解决，**跨进程并行仍未解** | app 侧 ipcServerName() 尊重环境变量已存在（cli_forward.cpp:18）⇒ 缺的是 host 侧同一开关；改动小但要过 host_manifest_gate | 中 | §7-59/§7-60 |
| C5 | Edge 升级后 CDP `Input.*` 行为、capture_e2e/hls_variant_logic_test 不在任何闸门名单 | C | py 测具是一次性手动跑的，坏了不会有人被提醒 | 登记进 live_roster 的"未编译桶"同类表（只登记不进门，避免把浏览器不确定性塞进 rc） | 小 | §7-59/§7-61 |

---

## 2. A 类逐项风险评估与最小验证路径（要点展开）

- **A1 路径遍历**：这是全表唯一"安全相关"项。攻击面链条完整存在：网页→扩展卡片（④ 已证 dir 字段端到端可达 GUI）→IPC（无校验）→引擎按给定路径建文件。**已查，不是缺陷的部分也要写清**：URL 空值有拒绝（ipc_server.cpp:226），"没有任何一行代码检查 dir/name 是否落在用户选择的根内"是本轮 grep 的一手结论（关键词 cleanPath/sanitize/traversal 在 src/app、src/storage/storage.c 零命中）。引擎侧两条已读实的边界事实（修正我初稿"放大风险"的过头说法）：`\?\` 长路径前缀确实"禁用 `..` 折叠"（download_core.c:121 注释原文），**但只在宽路径 ≥260 字符时才加**（:119-122，普通短遍历路径仍走系统规范化）；而 :231/:296 是裸 `snprintf("%s\\%s", dir, filename)` 拼接、无清洗 ⇒ 净结论不变：**全链没有任何一处校验 dir/name 落在授权根内**，且 `..` 的实际语义随路径长度漂移（短路径被 OS 折叠、超长不折叠）——这种"同一输入两种行为"本身就该被负例夹具钉住。最小路径按 TDD：先写必红的负例夹具（engine_selftest 或 logic test 均可），再决定清洗策略（拒绝 vs 收敛到白名单根），策略本身交主控定夺。
- **A2 的一次自我驳回（值得记，因为它差点把结论编反）**：我初稿据 `grep "HashVerifyDialog("` 判它"全仓无人构造＝死 UI"。判别式复查发现构造点在 **同一行换行的调用形状**：`task_detail_dialog.cpp:263` 是 `HashVerifyDialog dlg(this, fullPath.isEmpty() ? filename : fullPath);` ——我的模式串要求紧跟 `(` 的是 `this`，于是漏掉了唯一实例化点。入口链其实完整（托盘/双击→TaskDetailDialog→「校验文件完整性...」按钮→dlg.exec()），`git log --all --name-status` 亦显示自基线 `874427e` 起从未被删。**净结论降级为：功能活着，但零测试**——"查不到"有两解（真没有 / 我的量瞎），这次是我的量瞎，正例对照就是把匹配串放宽到类名本身再数一次。
- **A3/A4/A5** 共同形状：REFACTOR_PLAN 打了 [x] 的用户可见功能里，键盘/鼠标/E2E 三类证人覆盖了 Ctrl+N、Tab、卡片，唯独这三条"入口触发面"一条断言都没有。好消息是 ⑤ 交付的 `IDM_NEWTASK_COUNTFILE` 钩子让它们的最小验证都变成"发一个事件+读一个计数"的小改动。

## 3. 战略对照（REFACTOR_PLAN.md = 仓内现行战略文档）

说明：用户口中的 "piclist.md" 在仓内不存在；最接近的两处是 §7-54 引用的上游 PicList 审查规约（方法论，非能力清单）与本文件。按本审计的字面要求，以 REFACTOR_PLAN.md 为战略基线，**若主控另有所指请指认文件**——这不阻塞下面已能定的结论。

| 战略承诺 | 现状 | ①–⑥ 是否触及 | 判定 |
|---|---|---|---|
| BT/磁力（libtorrent 或 aria2 RPC） | TorrentDownloader 经 protocol_factory **在主线里**（aria2 式 RPC） | 引擎自测 0 命中、探针 0 命中 | **能力在、验证为零**（A 类候补：下载失败对用户可见；列入下批候选） |
| FTP/SFTP | FtpDownloader 未实现（待 vcpkg/libcurl） | — | 计划内欠账，非测试问题 |
| SQLite 持久化 | history_store 用 Qt SQL（history.db 实测存在），tasks/state 仍 JSON | S1/S2 覆盖 JSON 往返 | 迁移未完成是**计划事实**；JSON 面已有牙 |
| spdlog 替换 qDebug | 未做（logger.h 注释明示） | — | 计划内欠账 |
| 安装包打包 | 未做，且**被任务书 §2.1 写死禁止** | — | 永久边界，不进队列 |
| 文件完整性校验 | 入口链完整（详情框按钮→对话框 exec），**测试为零** | 否 | 见 A2 —— [x] 名副其实，但"已实现"不等于"已验证" |
| 剪贴板监听 / 拖放 / 托盘 | 代码在、断言无 | 否 | 见 A3/A4/A5 |
| 速度曲线图（Qt Charts） | ui_snapshot 有 ZOOM/TOOLPROBE 面部分触及 | 部分 | 维持 B 以下 |

## 4. 优先级建议（不含插队含义，顺序仍由主控下发）

1. **A1**（唯一安全项，且修复面清晰：入口清洗 + 负例夹具）。
2. **A2 配哈希已知向量用例**（入口已确认活着，欠的只是对拍；纯引擎侧，不动 UI）。
3. A5→A3→A4（三条共用 ⑤ 钩子，合计约一轮的量）。
4. B5/B6/C1/C3（都是小量补牙）。
5. C4（跨进程管道隔离——要动 host 产品代码，须与 host_manifest_gate 同批设计）。
6. B1/B8/B9/B10 归"测量悬案"池：不重复投工，等某个既有轮的窗口顺带采。
7. C2 不由我推进（push 边界在用户）。

## 5. 本轮自身的不诚实面（照例单列）

- **本报告的"零改动⇒读数有效"论证有一个洞**：报告文件本身落进 git 跟踪的 `docs/` ⇒ NOTICE 的 docs/ 计数与字节数在入库那刻起就与我引用的 `e4f6167caa4c` 不符。处置：§6 只报"代码输入未变"的门，NOTICE 明示为待签状态；复签归下一轮（或本批随提交一起跑 notice_verify 迭代到定点）。
- freshness 对 ui_snapshot/hls_variant_probe 两个目标在本轮**必然 DIRTY_INPUT**——它们的输入集含 `tools/`，而本报告新增一个 tools 外的 docs 件不触发它们、但 ⑥ 收尾时 `tools/hls_variant_logic_test.py` 曾以未跟踪态存在过；引用 §7-61 补录里的 rc=0 时要记住那是 a7760bd 时刻的值。
- **两条我自己的量具错，都在交付前被抓回**：①§2-A2 的"死 UI"误判（模式串太严漏掉换行调用形状，放宽重查即纠）；②A1 初稿"引擎放大风险"过头（实读 download_core.c:119-122 后收回：`\?\` 不规范化只在 ≥260 长路径生效）。同形教训入台账：**否定性 grep 必须先拿一个已知正例照穿自己的模式串**。
- A1 的定性在本轮做过一次**过头→修正**：初稿写"引擎层把风险放大"，实读 download_core.c:119-122 后收回——`\?\` 不规范化只在 ≥260 长路径时生效；净结论（无校验+长度依赖行为）不变但措辞降级为已证形状。仍**未做真遍历 PoC**（边界内不写敏感路径），"实测可写出目录外"留给负例夹具那一轮去证或去否证。
- 战略对照基于 REFACTOR_PLAN.md 的 2026-08-12 快照；文档可能落后于口头决策。用户口中的 "piclist.md" 仓内不存在（最接近的是 §7-54 引用的上游 PicList 审查规约，属方法论非能力清单）——按字面以 REFACTOR_PLAN.md 为基线，若另有所指请指认文件。
- 本轮未重跑套件/引擎（零代码改动 ⇒ 沿 HEAD `85224ab` 既有读数有效）。

## 6. 本轮验证了什么（口径收窄）

- 代码输入集合逐字节未变：`git status --porcelain -- src resources third_party/libcurl/include CMakeLists.txt tools` 空（本轮只新增 docs/ 一份 .md）。
- live_roster rc=0（新 md 不在其扫描面，顺带复跑确认没被我的表外文件绊到）。
- **未跑**：全套件/引擎/logic 测试——按上面口径它们测的是同一二进制同一源码，重跑只会得到 §7-61 补录里已有的数。**这个"没跑"是声明，不是遗漏。**

---

## 7. ⑧ A1 负例夹具轮（2026-10-06，TDD：先红归档，修法未实施）

**A1 判定 = 确认，但两处修正初稿口径**（一手读数 `docs/a1-red/red-baseline-1006.txt`）：

| 面 | 一手证据 | 定性 |
|---|---|---|
| filename | `[37 读数] 输入「..\..\evil_a1.bin」存下filename=「.._.._evil_a1.bin」`；PathGuard G2 转发值仍含 `\` | **引擎有清洗**（sanitize_filename_utf8 把 `\`→`_`），但 **IPC/GUI 层原样转发**——桌面 GUI 的 NewTaskDialog 路径不走 dlmgr_add 的清理 ⇒ 暴露面真实存在，只是比初稿窄 |
| dir | `[G1 读数] success=1 转发dir=「..\evil_dir_a1」`、`[37②] load_state=0 越界dir任务可查=1` | **全链零清洗**（add/IPC/state 恢复三条路都原样受理）⇒ A1 的核心就是 dir 面 |
| 放大形状 | download_core.c:119-122：`\?\` 不折叠仅在宽路径 ≥260 生效 | 短遍历路径会被 OS 折叠到根外（相对 save_dir），超长路径连折叠都没有 ⇒ 两种长度两种语义，都需要钉住 |

**交付的测具（本轮只加测具，产品代码零改动）**：
- `IDM_PATHGUARD_PROBE`（ui_snapshot 新模式，3 断言）：真 IpcServer + pid 私有管道 + 回调只记转发值。基线 **G1 FAIL / G2 FAIL / G3 PASS**。
- engine_selftest `[37①②③]`（3 断言）：dlmgr_add 的 name 证人 + load_state 的 dir 注入。基线 **①PASS（引擎已清）/ ②FAIL（dir 无清洗）/ ③PASS（正例没被误杀）**。
- ⚠ 两个新模式**尚未进 CI 名单与 PROBE-ROSTER 表**（probe_roster 会红）——登记为修好轮的收尾项，本轮不进门。

**修法提案（等授权，不在本轮实施）**：
1. **策略二选一，主控定夺**：(a) 拒绝式——IPC add 收到含 `..` 分量或绝对性可疑的 dir 时直接 `success=false, message="保存目录不合法"`；(b) 收敛式——`QDir(root).absoluteFilePath()` 折叠后必须仍以授权根为前缀，否则回落默认目录并印一行读数。建议 (a)：扩展卡片的 dir 本来就允许留空/手填，拒绝的用户代价低，静默改路径反而制造"下载到奇怪地方"悬案。
2. **落点唯一化**：清洗放 `ipc_server.cpp` add 分支（三入口 host/CLI/web 共用它）+ `task_controller.cpp internalAddTask` 兜底一层（覆盖 state 恢复与定时下载等非 IPC 路）。引擎侧 `dlmgr_add` 的 save_dir 也补一道"不含 `..` 分量"的入参检查（纵深，且 load_state :2134 那条恢复路在引擎内，只有引擎自己拦得住）。
3. **白名单根怎么定**：GUI 侧以 `Settings.defaultSaveDir()` 所在盘符下的用户目录树为根做前缀判断，还是"任意绝对路径都收、只拒 `..` 分量"？前者更安全但会打断"另存到 D:\downloads"这类合法用法。**这是产品决策，不是工程决策**。
4. **可核性补丁**：TaskInfo/summary 增加最终落点字段（或在 witness 行印 canonical 后的 dir），否则将来清洗对不对只能靠行为猜——本轮 dir 面证人停在"被受理与否"就是这个缺口的直接后果。
5. **变异证明计划**（修好轮执行）：摘 IPC 清洗 ⇒ G1/G2 从绿转红；摘引擎入参检查 ⇒ [37②] 红；把清洗写成"含点就拒" ⇒ G3/[37③] 红。三条各打一段，缺一不算闭环。
