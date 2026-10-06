# ⑩ A3 剪贴板回调验证 — 测具与四面变异（2026-10-06）

仓根 `D:/visual studio/lianxi/MFC/idm-next`，本轮起点 HEAD `a4ce4b8`。
探针模式 `IDM_CLIPBOARD_PROBE`（`tools/ui_snapshot.cpp`），五条断言 C1/C1b/C2/C3/C4。

## 复算命令

```bash
ninja -C build ui_snapshot idm-next            # -DIDM_BUILD_UI_SNAPSHOT=ON 已在缓存里
sha256sum < build/ui_snapshot.exe | cut -c1-12  # 身份算法固定
QT_QPA_PLATFORM=offscreen IDM_CLIPBOARD_PROBE=1 ./build/ui_snapshot.exe; echo RC=$?   # 期望 RC=0、五 PASS
```

## 代码定位（本轮一手 grep + 读码）

| 面 | 位置 | 形状 |
|---|---|---|
| 写侧① | `src/gui/history_dialog.cpp:122-133` `onRowDoubleClicked` | 入口=历史表格双击 ⇒ `clipboard()->setText(url)`（**全长不截断**）+ `m_countLabel`「已复制链接: 」+（url>60 ? left(60)+「…」: url） |
| 写侧② | `src/gui/hash_verify_dialog.cpp:185-191` `onCopyComputed` | 入口=对话框「复制结果」按钮（`:102` connect，`:82` 实名）⇒ `hash.isEmpty()` 早退；否则 `setText(m_resultLabel->text())` |
| 读侧 | `src/gui/main_window.cpp:853-901` `onClipboardChanged` | 信号连接 `:444-445`（`QClipboard::dataChanged`，仅当 `clipboardMonitor()` 为真才连）⇒ 过滤链：开关→单行(无空格/换行)→四前缀(http/https/ftp/magnet)→扩展名白名单(`toLower` 归一，空扩展与 magnet 放行)→`static lastUrl` 去重→`QMessageBox::question("检测到下载链接")` |
| 全仓 clipboard 调用点 | `grep -rn "clipboard()" src` | 只有上述两处写入 + 一处读取（`main_window.cpp:858`）。**没有**"右键菜单复制任务信息"这类入口 ⇒ 需求里的"复制任务信息"场景在仓内不存在，如实登记，不硬造探针 |

## 变异表（四面，各咬一条，互不重叠）

原文全部先备份到 `%TEMP%`，跑完逐字节还原（`git diff -- src/` 为空）。每面都先 `grep` 确认标记串已在盘上再重编。

| 变异 | 落点 | exe sha12 | 红的那条 | 读数要点 | 归档 |
|---|---|---|---|---|---|
| M-⑩a 摘掉写入调用 | `history_dialog.cpp:130` `setText(url)` → 注释 | `bbf7b8fcaafe` | **C1 + C1b** | `读回A=0 读回B=0`（标签仍显示 ⇒ 证明标签与剪贴板是两个字段） | `mut10a.txt`（`04541b339a40`） |
| M-⑩b 摘掉另一处写入 | `hash_verify_dialog.cpp:189` `setText(hash)` → 注释 | `0e7efd677a75` | **C2** | `有结果逐字写入=0`，而 `空结果未改写=1` 仍绿 ⇒ 正例面才有牙 | `mut10b.txt`（`d8f8158708d3`） |
| M-⑩c 写入错误内容 | `setText(url)` → `setText(url.left(60))` | `7e022722de70` | **仅 C1b** | `读回B=0`（26 字符 URL 不受影响 ⇒ C1 仍绿，判别式隔离成功） | `mut10c.txt`（`8297075303f9`） |
| M-⑩d 摘掉读侧守卫 | `main_window.cpp:878` `if (!extOk) return;` → `if (false && !extOk)` | `2ab8dafdcbd0` | **仅 C4** | `三种噪声新增弹窗=2`（非链接那条被前缀门挡住，所以是 2 不是 3） | `mut10d.txt`（`ac04f2980a9c`） |
| 还原复绿 | 三文件回原字节 | `fc642b28777f` | 无 | `RC=0`、五 PASS | `mutrestore.txt`（`c773c3729f1c`） |

绿色基线（修复判据后、变异前）：exe `7b1290d6cb36` ⇒ 本文写作过程中同一源码又重编一次得 `fc642b28777f`，
两份输出逐字节相同（`c773c3729f1c`）——**编译器把两次产物哈希不同这件事暴露成"读数必须带 exe sha12"，而不是结论不同**。

## 两条第一版判据被实测驳回（先取证再改期望）

1. **C1b**：第一版写 `!labelB.contains(QString(20,'x'))`。产品截断保留的是 **left(60)**，
   夹具 URL 去掉 `https://ex.test/` 后剩 70 个 x ⇒ 截断点落在 x 族**内部**（标签含 46 连 x），
   该谓词恒假、夹具恒红。正确形状：`剪贴板==全长 URL` ∧ `标签长度 < URL 长度`（`68 < 90`）。
2. **C3**：第一版在 `clipboard()->setText(goodUrl)` **之后**才快照 `before=spy.seen`。
   `setText` 本身会让 Qt fire `dataChanged` ⇒ 第一个框已经弹过，差分恒 0，
   于是出现"首见文本非空却报命中弹窗=0"的自相矛盾读数。修法：`before` 必须在 `setText` 之前取。
   同时正文提取改成"取第一条非空行"——模板是 `…：\n\n%1\n\n是否添加…`，
   只跳到第一个 `\n` 会把尾部段落一起留下 ⇒ 逐字比较恒不等。
