# ⑬ A4 U2 修复 — 夹具与变异证据（2026-10-06）

仓根 `D:/visual studio/lianxi/MFC/idm-next`，分支 master。哈希算法固定
`sha256sum < 文件 | cut -c1-12`；探针输出里 `[DRAGDROP 身份] exe_sha12=` 由二进制自己算，
与本表外部复算同一算法（⑫ 定的规矩：两面红同一判据时归档文件会字节相同，只靠日志分不出是谁）。

## 一、本轮改了什么

| 面 | 文件 | 改动 |
|---|---|---|
| 产品 | `src/gui/main_window.cpp::dropEvent` | **U2**：原先只取 `urls().first()`，其余静默丢弃 ⇒ 拖 10 条只下 1 条。现在 URL 面逐条收、文本面按行拆（与「批量导入」同口径 `batch_import_dialog.cpp:139`），R2 上限改为**逐条**判，一次 drop 仍**只弹一个**对话框，确认后其余按同一目录/线程数一并受理，每条走 R3 已有日志通道，末尾补一条批次汇总。 |
| 夹具 | `tools/ui_snapshot.cpp::IDM_DRAGDROP_PROBE` | 新增 D4（批量全收）+ D5（批量逐条留痕）；D2 判据换成有牙的版本（见 §四）；清场通道重做（见 §三）。断言 3→5。 |
| 台账 | `docs/drag-audit-1006.md` | §2 分类列按 A/B 两层口径订正（消解 `coverage-audit-1006.md:22` vs `:61` 的矛盾）、§4 决策记录（R1 定案 / U2 选型理由 / U1 暂缓）、§5 未验证清单刷新。 |
| 名单 | `docs/ARCHITECTURE.md` §1.4 | `IDM_DRAGDROP_PROBE` 断言条数 3→5。CI 名单不变（模式已在册）。 |

## 二、五条断言各钉什么

| # | 钉住的行为 | 唯一证人 |
|---|---|---|
| D1 | 正常 URL 拖入 ⇒ 恰好弹一个对话框且预填**逐字**等于该 URL | 期望非 0 的正例对照；没有它，"drop 什么都不做"能让 D2~D5 全绿 |
| D2 | R2 长度上限 | `告警含「内容过长」`（见 §四第 2 条）＋ 交给对话框的长度有界 |
| D3 | R3 失败支有 warn | 引擎灌到 `dlmgr_add` 拒绝（现场制造，`nListed>=100` 当分母）后 warn 含「添加任务失败」 |
| D4 | U2 批量全收 | 引擎条数现算增量 `==3` ＋ 产品自己的 IPC `list` 行数 `>=3` ＋ 对话框峰值恰 1 |
| D5 | U2 每条走 R3 日志通道 | `拖放添加任务 #N:` 正则抓 id、**去重后 ==3** ＋ 批次汇总行存在 |

## 三、清场：为什么公开 API 擦不掉 D3 留下的 128 条幻影任务

D3 为了造"引擎满额"直接 `dlmgr_add` 灌到返回 -1。这些任务**从未 start**，于是：

- `dlmgr_cancel` 只置 `status=TASK_CANCELLED`；真正摘槽要等 worker 收尾，而收尾要求 `chunk_count > 0` ⇒ 永远没人摘。
- `dlmgr_remove_completed()` 只收 COMPLETED/FAILED 两态（`download_core.c:2373`），对 CANCELLED 恒返回 0。实测 `已摘除=0`、`baseN` 仍 128。
- `dlmgr_remove()` 会走到 `detach_slot`，但 `detach_slot` 在"还有别的任务 `worker_alive>0`"时只登记 `pending_detach` 不缩数组（`:724`），而 `sweep_pending_detach_locked` 的唯一驱动点是某个 worker 退场（`:1430`）⇒ 同样永久滞留。实测倒序摘也 `已摘除=5120` 而残留仍 128。

正解：`dlmgr_load_state(<不存在的路径>)` —— 它在读文件之前先调 `reset_all_tasks`（`:2081`），
`memset(g_tasks)` + `g_task_count=0` + `g_next_id=1`；随后 `fopen` 失败返回 -1 也不影响清空已发生（`:2086`）。
读数行同时印 `清场残留/基线条数/清空轮数`，清不干净就让 D4 红并说明为什么红。

## 四、被实测否证的三个夹具假设（都记下来，防止下一版复活）

1. **"取消之后再 remove 就能清场"** ⇒ 上节三条实测否证。
2. **D2 原判据 `告警含「拖放」` 没牙**：MUT-F/G 之前我先跑了一枚 `one.length() >= 0` 的坏变异（把守卫反成"全拒"），
   D2 照样绿——因为它接受"根本没开对话框"当作"有界"的替代，而那条 warn 来自别处。
   改成钉 R2 自己那句「内容过长」后，同一形态立刻红。
3. **D5 第一版判据"有一行含该 URL"没牙**：`TaskController::internalAddTask` 自己就
   `Log::info("添加任务 #N (队列:…): url")`（`task_controller.cpp:228`），
   摘掉 `dropEvent` 里的逐条日志后 D5 仍然绿（实测 info 行数=5、三条 URL 全被控制器命中）。
   改成只认 **dropEvent 自己的前缀** + **去重 id 计数**后才有区分度。

⚠ 顺带纠正我自己上一轮的错话：`queue=""` **不**绕过 `maxConcurrent`。
控制器用的是 `startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot()`
（`task_controller.cpp:101/153/173/214/261`），`hasFreeSlot()` 拿运行数比 `qMax(1, m_maxConcurrent())`
（`queue_scheduler.cpp:38-48`，默认 4）。批量条目满额时留在列表里等位。

## 五、七枚变异各咬哪一条

| 面 | 摘掉什么 | 红的是 | exe_sha12 | 文件 |
|---|---|---|---|---|
| MUT-A | U2 整条回退成"只取第一条"（`for i<0`） | **D4 + D5**（增量 1≠3、去重 id 1≠3） | `9eb5e3343509` | `mut-13A-u2-reverted-first-only.txt` |
| MUT-B | 批量支成功与失败都不留痕 | **只 D5**（D4 仍绿 ⇒ 条数与日志是两个独立面） | `f8b58d5c9e99` | `mut-13B-batch-no-log.txt` |
| MUT-C | 批量支只摘成功日志、留失败 warn | **只 D5** | `fd6300edd6a0` | `mut-13C-batch-no-success-log.txt` |
| MUT-F | 只摘 R2 的逐条长度上限（收集逻辑不动） | **只 D2**（`交给对话框的长度=32767`＝修复前的真实形状） | `18a0b671ccd1` | `mut-13F-r2-cap-removed.txt` |
| MUT-G | R2 保留拒收、只摘它的 warn | **只 D2**（证明"唯一证人"那半独立有牙） | `e0e9178bd4d8` | `mut-13G-r2-warn-removed.txt` |
| MUT-H | 只摘批次汇总行 | **只 D5** | `2278c74fe298` | `mut-13H-no-batch-summary.txt` |
| （作废）MUT-E | 首版 R2 变异写成 `one.length() >= 0`，把守卫**反转**成全拒 | D1/D3/D4/D5 一起红、D2 反而绿 ⇒ 不是受控变异，只留下"判据没牙"这个发现 | `7c5ae6f69f4e` | 未归档 |

**未被任何变异证明的护栏**（如实登记，不占功）：
- D4 里的 `乱序探针 可查=1`：清场成功后数组为空，探针恒过 ⇒ 它只在"残留≠0"时才有信息量，属诊断行而非判据。
- D4 里的 `本次取消=0`：只是读数，没有断言依赖它。

## 六、终态

`green-13-u2.txt` = 恢复干净源码、**由 `build_gate` 重编之后**的那枚产物上跑的
（`exe_sha12=9d38670b32e3`，与 `[BUILD_GATE] OK exe=ui_snapshot.exe sha12=` 那行当场对得上；
先前一次同源码运行的哈希是 `96f47bd3e634`——链接时间戳进了哈希，所以归档只认门禁产物这一份），D1~D5 全 PASS。
所有变异产物已从 `src/` 撤下：`grep -cE "MUT-|if \(false\)" src/gui/main_window.cpp` = 0，
且 `src/gui/main_window.cpp` 与本轮开工前的 U2 备份逐字节同哈希（`1e846630adc6`）。
