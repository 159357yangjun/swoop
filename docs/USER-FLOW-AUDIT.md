# 用户全流程静态走查报告（2026-09-24）

> 前提：**不运行 GUI**。逐跳把「一条用户路径」映射到状态机与消息处理，
> 只记录能落到具体代码行、且能解释用户症状的缺陷。
> 走查路径：安装 → 启动 → 新建任务 → 排队 → 暂停/恢复 → 重启恢复 → 设置 → 托盘 → 外部唤起 → 退出。

## 结论摘要

| 级别 | 数量 | 说明 |
|---|---|---|
| P1 | 4 | 会造成「点了没用 / 进度错 / 覆盖已有文件 / 文件名乱码」 |
| P2 | 6 | 视觉与交互承诺对不上、可恢复性差 |
| 未修 | 3 | 需要更大改动或无 GUI 无法验证，已记录风险与设计草图 |

自测基线从 12 项扩到 **15 项，全绿**；新增断言逐条做过突变验证（见文末矩阵）。

---

## 一、已修缺陷

### P1-1 排队中的任务「暂停」是空操作
- **症状**：任务多到触发排队后，选中一条点「暂停」，下一瞬间它自己又跑起来。
- **根因**：`task_pause()` 开头 `if (t->status != DL_DOWNLOADING) return;`。
  排队任务状态是 `DL_QUEUED`，函数直接返回、状态不变；而 `schedule_queue()` 每个 250ms
  tick 会把所有 `DL_QUEUED` 都拉起来。定时调度的「暂停全部」同样漏掉排队任务。
- **修法**：`task_pause()` 增加 `DL_QUEUED → DL_PAUSED` 分支（幂等）。

### P1-2 续传时线程计数起点错误 → 进度条会超过 100%
- **症状**：反复暂停/续传同一个 8 段任务后，进度显示 `140.0%` 这类数字。
- **根因**：`conn_arg_t.written` 在 `task_start()` 里被初始化为 `0`。
  分段线程退出时执行 `t->seg_written[a->seg] = a->written;` —— 于是「暂停前的进度」
  被覆盖成「只有本轮收到的字节数」。下次续传读到的 `already` 偏小 → 请求范围变宽、
  重复字节被再次累加进 `downloaded`。
- **为什么不丢数据**：起点偏小意味着线程总会从更早处下到段尾，最终文件仍是完整的；
  坏的是计数与显示。
- **修法**：抽出纯函数 `task_seg_plan(seg_start, seg_len, already, &start, &len, &written0)`，
  其中 **`written0 = already`**；UI 端再加 100% 兜底。

### P1-3「全部开始」会把已完成的任务重置重下
- **症状**：点了「全部开始」，已经下好的大文件被整份重新下载 / 覆盖。
- **根因**：`start_all()` 对 `DL_COMPLETE` 执行
  `downloaded=0; total=0; seg_count=0; seg_written[]=0` 并置回 `DL_QUEUED`；
  `task_start()` 走「全新下载」分支 → `CreateFileW(..., CREATE_ALWAYS)` 立刻截断原文件。
- **修法**：`start_all()` 跳过 `DL_COMPLETE`（批量语义 = 把没下完的跑起来）；
  要重下请选中单条点「开始 / 续传」。

### P1-4 URL 里的 UTF-8 `%XX` 解码错误 → 中文文件名全乱
- **症状**：从中文站点下载，文件名变成 `ä¸æ–‡.mp4`。
- **根因**：`category_filename_from_url()` 里
  `out[j++] = (wchar_t)((hi << 4) | lo);` —— **一个 `%XX`（一个字节）塞进一个 `wchar_t`**。
  `%E4%B8%AD` 是 UTF-8 的「中」三字节，被拆成 3 个独立字符。
- **修法**：改为两步 —— ① 先收集**字节**（`%XX` 直接落字节；URL 里本来就有的非 ASCII
  宽字符按 UTF-8 编出去，含代理项对）；② 对整段字节做一次 `MultiByteToWideChar(CP_UTF8)`。
- **副作用修好**：分类归档（`.mp4` → `Videos`）此前也可能因乱码后缀判错。

### P2-1 去重挡住了「重新下载」
- 已完成/已失败的旧任务仍参与 `find_dup()` 比对 → 想重下同一链接只能先去删记录。
- **修法**：`find_dup()` 跳过 `DL_COMPLETE` / `DL_ERROR`（它们不再写文件）。

### P2-2「全部暂停」会被定时调度悄悄恢复
- `pause_all()` 不写 `user_paused`（这对「定时暂停」是**正确**的，到点要能自动开始），
  但菜单项复用了同一个函数 → 用户手动「全部暂停」，到点又被整体拉起。
- **修法**：拆成 `pause_all_user()`（菜单用，写 `user_paused`）与 `pause_all()`（调度用，不写）。

### P2-3 菜单写着 `Ctrl+N`，但根本没有快捷键表
- `.rc` 里只有文案 `\tCtrl+N`，没有 `ACCELERATORS` 资源；消息循环也没有
  `TranslateAccelerator` → 按 Ctrl+N 毫无反应。
- **修法**：新增 `IDR_ACCEL`，`main.c` 挂
  `LoadAcceleratorsW` + 在 `GetMessage` 循环里优先 `TranslateAcceleratorW`。

### P2-4 对话框控件被显式 style 顶掉了默认样式
- RC 语法里，**一旦显式写 style，默认 style 会被整体替换**。
  `EDITTEXT ..., ES_AUTOHSCROLL` 丢掉 `WS_BORDER | WS_TABSTOP`
  → 输入框**没有边框**、Tab 键切不到下一个控件；`CHECKBOX` 未写 `BS_AUTOCHECKBOX`，
  而 `WM_COMMAND` 只处理 `IDOK/IDCANCEL` → 勾选框点了不保持。
- **修法**：逐控件显式补 `WS_BORDER | WS_TABSTOP` / `BS_AUTOCHECKBOX | WS_TABSTOP`，
  两个对话框补 `DS_CENTER`。

### P2-5 单实例转交 URL 的编码不一致
- 命令行 `lpCmdLine` 是 **ANSI（中文系统 = GBK）**，直接塞进 `WM_COPYDATA`；
  接收端按 **UTF-8** 解 → 带中文的链接变乱码。
- **修法**：`forward_to_existing()` 先 `CP_ACP → 宽 → CP_UTF8` 再发。

### P2-6 新建任务的同步探测把对话框卡住
- `new_task_dlg` 的 `IDOK` 分支里直接 `schedule_queue()` → `task_start()` → **同步**
  `http_probe()`。主机不可达时按系统默认可等 60s，对话框「确定」按下去迟迟不关。
- **修法**：① 连接/发送超时收紧到 10s / 20s；② 对话框不再直接启动，交给 250ms 定时器
  —— 窗口先关、任务先显示成「排队中」，而不是按了确定像死机。

---

## 二、未修（已记录）

### 1. 首次探测仍是同步的（P1 级体验问题，改动风险高）
`task_start()` 在 UI 线程里做 `http_probe()`。彻底解决要把它挪进一个「启动线程」：
- 线程先做探测 + 建目录 + 建文件，再 spawn 分段线程；
- `threads_expected` 需设为 `1 + 分段数`，启动线程退出时再 `+1`，避免 tick 提前判完成；
- `task_pause()` 在启动线程探测期间被调用会与之竞态，需要 `running` 双重检查。

本机**无法运行 GUI**验证线程竞态，故本轮只做超时收敛 + 延后启动，不盲目重构。

### 2. 删除任务不删磁盘上的部分文件
IDM 会弹窗询问「是否同时删除文件」。默认不删更安全（防止误删用户的文件），暂留。

### 3. 批量操作入口偏少
列表无右键菜单、无 Ctrl+A，多选只能靠鼠标框选 / Ctrl 点选；批量动作只能走菜单项。

---

## 三、验证与突变矩阵

**终态基线**
```
make all                        EXIT=0（唯一警告：http.c 的 -Wcast-function-type）
swoop_selftest.exe                PASS 15/15
swoop_nmhost.exe --selftest       PASS fails=0
objdump -p swoop.exe              静态导入 = 7 个（IDM-parity 未破）
残留进程                         无
```

**自测新增项**
| 字段 | 覆盖内容 |
|---|---|
| `queue` | `queue_slots` + 新增 `queue_pick`（必须跳过 `user_paused` / 在跑的 / 已完成的） |
| `cat` | 新增 UTF-8 `%XX` 用例：`%E4%B8%AD%E6%96%87.mp4 → 中文.mp4` |
| `segplan` | `task_seg_plan` 六组，重点 `{0,1000,400} → start=400, len=600, written0=400` |
| `paused` | 排队任务 `task_pause` 必须变 `DL_PAUSED`，且幂等 |
| `res` | `LoadAccelerators(IDR_ACCEL)` / 两个菜单 / 两个对话框模板 / 图标都真的存在 |

> `res` 这一项存在的原因：资源 ID 写错或 `.rc` 语法问题 **windres 不报错**，
> 只在运行时静默失效。为此 `Makefile` 让 `swoop_selftest.exe` 也链上 `build/resources.res`。

**突变矩阵（注入 → 精确变红 → 还原复绿）**
| 突变点 | 期望变红的字段 | 实测 |
|---|---|---|
| `queue.c` 删掉 `user_paused` 判断 | `queue` | `queue=20` ✅ |
| `category.c` 恢复「字节直接当宽字符」 | `cat` | `cat=25`（正是新断言）✅ |
| `download.c` `out_written0 = 0` | `segplan` | `segplan=101` ✅ |
| `download.c` 注掉 `DL_QUEUED` 分支 | `paused` | `paused=3` ✅ |
| `resources.rc` 删掉快捷键表 | `res` | `res=1` ✅ |

---

## 四、仍待人工确认的项

以下**只能靠真实桌面操作**确认，静态走查给不出结论：

1. 托盘区图标、气泡通知、双击隐藏/显示的实际表现。
2. 多选（框选 / Ctrl 点选）在真实列表上的手感与批量暂停/删除表现。
3. Ctrl+N 快捷键在真实焦点下的行为（含对话框打开时的表现）。
4. 设置对话框改样式后的控件边框与 Tab 顺序观感。
5. BT/磁力：需把 `aria2c.exe` 放到 `swoop.exe` 同目录（本机没有，程序会退回 PATH 查找）。
6. 定时调度到点触发（依赖系统时钟跨过设定分钟）。
