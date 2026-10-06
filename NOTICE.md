# NOTICE — 开源范围、授权现状与第三方组件清单

> **本文件不是许可证，不授予任何权利。**
> 它只做两件事：① 声明"哪些东西属于开源交付物、哪些不属于"；② 记录仓库里 vendored 的第三方组件及其许可证出处。
> 授权条款由项目所有者决定，不由本文件决定。
> 表内所有数字与"文件:行"证据由 `tools/notice_verify.py` 从盘上重算核对，不一致即 `exit 1`：
> `python tools/notice_verify.py`（本机 `python3` 不存在，是 `python`；只读，不改任何文件、不联网）。

## 1. 开源范围

| 路径 | 文件数 | 字节数 | 是否在开源交付范围内 |
|---|---|---|---|
| `src/` | 114 | 945061 | ✅ 在——IDM Next 应用本体（GUI + 下载核心） |
| `tools/` | 50 | 900421 | ✅ 在——自测与门禁脚本 |
| `docs/` | 71 | 616985 | ✅ 在 |
| `resources/` | 18 | 35001 | ✅ 在 |
| `installer/` | 2 | 17674 | ✅ 在 |
| `.github/` | 2 | 37113 | ✅ 在 |
| `third_party/` | 125 | 20168154 | ⚠️ 在范围内但**不是本项目的原创代码**，见 §3 |
| `browser-extension/` | 19 | 666798 | ❌ **不在**——浏览器扩展不属开源交付物（所有者口头指示）；处置见 §2 |
| 仓根散装文件 | 10 | — | ✅ 在（`CMakeLists.txt`、`README.md`、`NOTICE.md`、`vcpkg.json`、`REFACTOR_PLAN.md`、`开发文档.md`、`.gitignore`、`.gitattributes`、`.editorconfig`、`.clang-format`） |
| **合计（`git ls-files`）** | **411** | | 恒等式 `401 + 10 = 411`，由 `--verify` 当场断言 |

复算命令（在仓库根跑；注意 `bc` 在本机**没有**，所以求和走 `awk`，别抄成 `| bc`）：

```bash
for d in src tools docs resources installer .github third_party browser-extension; do
  printf '%-20s files=%-4s bytes=%s\n' "$d" \
    "$(git ls-files -- "$d" | wc -l)" \
    "$(git ls-files -- "$d" | while IFS= read -r f; do [ -f "$f" ] && wc -c < "$f"; done | awk '{s+=$1} END{print s+0}')"
done
git ls-files | grep -vc /          # 仓根散装文件数
git ls-files | wc -l               # 合计
```

## 2. 授权现状（这一节是"事实 + 复算命令"，不是"决定"）

每一行都要能由**只拿到这个仓的人**当场复算；复算不了的（要联网、或要看另一个工作树）单独标出来，
不混进"可复算"那一堆。

| # | 事实 | 复算命令（在仓库根跑） | 本轮读数（2026-10-01 11:5x） | 只拿到仓能否复算 |
|---|---|---|---|---|
| L1 | 当前分支 `master`（IDM Next）**没有任何许可证文件** | `git ls-files \| grep -icE '(^\|/)(LICENSE\|LICENCE\|COPYING\|UNLICENSE)([.][A-Za-z]+)?$'` | `0` | ✅ 能（`--verify` 断言） |
| L2 | GitHub 给本仓库显示的许可证徽章是 **MIT** | `curl -sS https://api.github.com/repos/159357yangjun/swoop/license` | `path="LICENSE"`、`sha=594869ea5e6b28156af2402367269f2c28f2fb62`、`size=1063`、`license.spdx_id="MIT"` | ⚠️ 要联网 |
| L3 | 徽章那份 MIT 具体是**哪个文件** | `git rev-parse origin/main:LICENSE`（离线，比 L2 的 sha） | `594869ea5e6b28156af2402367269f2c28f2fb62` ← 与 L2 **逐字相同** ⇒ 本地 ref 与公开内容一致 | ✅ 能（`--verify` 断言） |
| L4 | 该文件内容首行与版权行 | `git show origin/main:LICENSE \| sed -n '1p;3p'` | `MIT License` / `Copyright (c) 2026 杨珺` | ✅ 能（`--verify` 断言） |
| L5 | MIT **不属于 `master`**，它属于默认分支 `main` | `git symbolic-ref refs/remotes/origin/HEAD` | `refs/remotes/origin/main` | ✅ 能（`--verify` 断言） |
| L6 | 整个历史里改过根 `LICENSE` 的提交只有 1 条 | `git log --all --oneline -- LICENSE \| wc -l` | `1`（`18f937d IDM Next v0.1.0: Windows 原生 C 下载器首发`） | ✅ 能 |
| L7 | 所有者已在**另一个工作树**里把 MIT 改成 **GPL-3.0-only**（含源文件 SPDX 头），但**这条改动没有推** | 只读：`git -C "D:/visual studio/lianxi/swoop/idm" log -1 --format='%h %s' 80a1db7`；`git -C "D:/visual studio/lianxi/swoop/idm" branch -r --contains 80a1db7`（输出为空）；`git -C "D:/visual studio/lianxi/swoop/idm" rev-list --left-right --count origin/main...HEAD` ⇒ `21 2` | `80a1db7 2026-09-25 license: MIT → GPL-3.0-only（含源文件 SPDX 头）`、远端无分支包含它 | ❌ **不能**——路径在本仓之外，且那个仓不是本交付物的组成部分；本行只作**本轮记录**，绝不进门禁断言 |
| L8 | 仓库对外是**公开**的 | `curl -sS https://api.github.com/repos/159357yangjun/swoop` | `"private": false`、`"visibility": "public"`、`"default_branch": "main"` | ⚠️ 要联网 |
| L9 | `browser-extension/` 自 `874427e`（2026-09-19）起就在公开仓库里 | `git log --diff-filter=A --format='%h %ad' --date=short -- browser-extension \| tail -1` | `874427e 2026-09-19`，subject 含「建立工程基线」 | ✅ 能（`--verify` 断言） |

**这三条叠在一起的实际后果，必须直说**：徽章写着 MIT，而 `master` 没有许可证文件，
`browser-extension/` 又已经在公开历史里。任何第三方读到"MIT"就有权按 MIT 的条款拿走**全部内容**，
包括所有者声明过"不开源"的那个目录——本文件的这句声明**不能撤回已经公开的东西**，也不能替代许可证。
另有一条方向性事实要登记：所有者那边（L7）已经把许可换成 **GPL-3.0-only**，但那条改动**尚未公开生效**，
所以现在读到徽章的人拿到的仍是 MIT。这两者不一致本身就是风险，不是本文件能弥合的。
要让"扩展不开源 + 换成 GPL"成为事实，只有三条路，且**都不在本轮权限内**（不 push、不在共享远端上做 force-push、不碰 `D:\visual studio\lianxi\swoop\idm` 那棵树）：

1. 给 `master` 落一份 `LICENSE` 并**在条款里明确排除范围**——条款内容与是否沿用 GPL-3.0-only 都属所有者的决定（L7 是线索，不是授权我照抄）；
2. 把 `browser-extension/` 从**公开历史**里移除（改写历史 + force-push），或与 `master` 拆成不同仓库；
3. 把 L7 那条改动推到公开默认分支（这是另一个仓、另一个会话的事）。

另注一条盘上不一致（`--verify` 会核）：`.gitignore` 声明忽略
`.gitignore:31 含「browser-extension/native-messaging-host/*.exe」`、
`.gitignore:32 含「browser-extension/native-messaging-host/*.dll」`，
但该目录下仍有 **3 个 `.dll` 被跟踪**
（规则早于文件、或文件先入库后写规则，`.gitignore` 对已跟踪文件无效）：
`networkinformation/qnetworklistmanager.dll`、`tls/qcertonlybackend.dll`、`tls/qschannelbackend.dll`。
同目录另有 **6 个**构建产物（`Qt6Core.dll` 11.3MB、`libstdc++-6.dll` 2.2MB、`Qt6Network.dll` 2.0MB、
`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`、`idm-next-host.exe`）**未入库**。
整个目录在盘上是 25 个文件 / 16509136 字节，被跟踪的是 19 个文件 / 入库数 666798 字节——
两个数都印在这里并由 `--verify` 各自核对，**不许用"约 16MB"冒充公开暴露的量**：
公开暴露的是入库数，磁盘数只是本机残留。

## 3. Vendored 第三方组件（`third_party/libcurl/`）

来源是 curl 官方维护的 Windows/MinGW 构建，出处与校验值记在 `third_party/libcurl/README.md:8-14`。
下表"版本"与"证据"两列**不是手抄**：证据列每条 `路径:行 含「…」` 都由 `--verify` 打开那一行核对。

| 组件 | 版本 | 证据（文件:行 含「原文」） | 许可证标识（随仓头文件自述） | 许可证全文是否入仓 |
|---|---|---|---|---|
| curl / libcurl | 8.22.0 | `third_party/libcurl/include/curl/curlver.h:35 含「LIBCURL_VERSION "8.22.0"」` | curl license（`include/curl/curl.h:12` 自述"licensed as described in the file COPYING"） | ❌ 否 |
| LibreSSL | 4.3.2 | `third_party/libcurl/include/openssl/opensslv.h:6 含「LIBRESSL_VERSION_NUMBER 0x4030200fL」；third_party/libcurl/README.md:20 含「libressl 4.3.2」` | BSD 风格（上游 `COPYING`） | ❌ 否 |
| nghttp2 | 1.70.0 | `third_party/libcurl/include/nghttp2/nghttp2ver.h:32 含「NGHTTP2_VERSION "1.70.0"」` | MIT（`include/nghttp2/nghttp2.h:9` permission 原文） | ❌ 否 |
| nghttp3 | 1.18.0 | `third_party/libcurl/include/nghttp3/version.h:34 含「NGHTTP3_VERSION "1.18.0"」` | MIT（`include/nghttp3/nghttp3.h:8` 同族 permission 原文） | ❌ 否 |
| ngtcp2 | 1.25.0 | `third_party/libcurl/include/ngtcp2/version.h:39 含「NGTCP2_VERSION "1.25.0"」` | MIT（`include/ngtcp2/ngtcp2.h:7` "Permission is hereby granted"） | ❌ 否 |
| libssh2 | 1.11.1 | `third_party/libcurl/include/libssh2.h:51 含「LIBSSH2_VERSION "1.11.1"」` | `libssh2.h:39` 自述 `SPDX-License-Identifier: BSD-3-Clause` | ❌ 否 |
| zlib-ng | 2.3.3 | `third_party/libcurl/include/zlib.h:52 含「ZLIBNG_VERSION "2.3.3"」` | zlib license（`include/zlib.h:10` 起 permission 原文） | ❌ 否 |
| zstd | 1.5.7 | `third_party/libcurl/include/zstd.h:112 含「ZSTD_VERSION_MAJOR 1」；third_party/libcurl/include/zstd.h:113 含「ZSTD_VERSION_MINOR 5」；third_party/libcurl/include/zstd.h:114 含「ZSTD_VERSION_RELEASE 7」` | 双许可 BSD-2-Clause 或 GPLv2（`include/zstd.h:5-6` 自述） | ❌ 否 |
| libpsl | 0.23.3 | `third_party/libcurl/include/libpsl.h:34 含「PSL_VERSION "0.23.3"」` | MIT（`include/libpsl.h:6-7`） | ❌ 否 |
| brotli | 1.2.0 | `third_party/libcurl/README.md:21 含「brotli 1.2.0」` | MIT（`include/brotli/decode.h:3` "Distributed under MIT license"） | ❌ 否 |

**这张表里最要紧的一列是最后一列。** 上游许可证全文**一份都没进仓库**，而
`.gitignore:49 含「third_party/libcurl/docs/」` 恰好把上游构建包里可能带许可证文本的那个目录整体排除了——
实测：`third_party/libcurl/` **整个目录递归**里匹配 `LICENSE|LICENCE|COPYING|UNLICENSE|NOTICE` 的文件名 = **0** 个；
被排除的 `third_party/libcurl/docs/` 递归有 **188** 个文件（顶层是 57 个文件 + 2 个子目录 `examples/`、`libcurl/`），
里面也没有许可证全文（这两个数由 `--verify` 现算并印出；我上一版在这里写的"59 个文件"是 `ls | wc -l` 数了条目、
把两个子目录算了进去，已被这道门当场驳回并改正）。
也就是说：现在的仓库分发的是**二进制 + 头文件**，附带"我们知道上游是什么许可"的一句话，
而多数许可条款要求分发时**随附许可全文**。这是待办（§4），不是本文件能替它补上的。

## 4. 待办（各归其人，标明为什么不由我这一轮做）

| # | 待办 | 归属 | 为什么不在本轮 |
|---|---|---|---|
| 1 | 给 `master` 决定并落一份 `LICENSE`（含范围条款） | 所有者 | 授权条款是法律选择，不是我能替拍的默认值 |
| 2 | 把 §3 各组件的上游许可证全文收进 `third_party/licenses/` 并让本表最后一列变 ✅ | 我可以做（需联网取上游文本，或从上游 zip 取） | 本轮未取网；且收哪些版本要与 §1 的决定一起定 |
| 3 | `browser-extension/` 移出公开历史，或与 `master` 分仓 | 所有者 + 需 force-push | 不 push；共享远端上有并行会话，改写公开历史属高风险动作 |
| 4 | `.gitignore:31-32` 与 3 个已跟踪 DLL 的矛盾：要么取消跟踪，要么删规则 | 所有者定方向 | 取消跟踪=从公开分发里删内容，属"作者权"动作 |
| 5 | Qt6 随包提供许可证与可重链接说明（组件清单：`CMakeLists.txt:18 含「find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Charts Sql)」`） | 出包环节 | 任务书 §2.1 写死本轮不出包 |

Qt6 的版本**不在仓库里固定**：组件清单见
`CMakeLists.txt:18 含「find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Charts Sql)」`、
`vcpkg.json` 只写 `qt6-base` 与 `builtin-baseline`。`6.11.1` 这个数字目前只出现在
`third_party/libcurl/README.md:17 含「Qt 6.11.1」` 的散文里，所以本文件不把它当事实登记。

## 5. 运行时外部后端（不入库）

`aria2c` / `yt-dlp` / `FFmpeg` 由程序在运行时获取或使用用户已有副本，
仓库内**不含它们的源码或二进制**。这条的精确形状（`--verify` 按此断言，不写"0 个命中"那种会被我们自己的对接层打脸的话）：
`git ls-files` 里名字含 `aria2|ffmpeg|yt-dlp` 的只有 **4 个文件**，且**全是我们自己写的对接层**
`src/protocols/aria2_daemon.{cpp,h}`、`src/protocols/ffmpeg_backend.{cpp,h}`；
其中匹配 `\.(exe|dll|a|lib)$` 的条数 = **0**；`yt-dlp`/`yt_dlp` 命中 **0** 个。
本机 `C:\Users\yyyy\idm_shots\_data\{aria2,ffmpeg,yt-dlp}` 是用户自己的缓存，
`tools/ui_probe_run.sh` 等脚本的清理逻辑**不得触碰**。
