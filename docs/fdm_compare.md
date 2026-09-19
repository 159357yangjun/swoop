# IDM Next × Free Download Manager (FDM) 源码对比分析

> 对标对象：FDM **3.9.7 (build 1327, GPLv3)**，开源镜像 `github.com/LFYG/FDM`。
> 目标：从源码层面找功能/性能差异，好的直接借鉴，需优化的就地适配。
> 分析方法：下载 FDM 源码逐模块对照 IDM Next 现有实现（`src/core`、`src/protocols`、`browser-extension` 等）。

## 一、结论速览

| 维度 | FDM 3.9.7 做法 | IDM Next 现状 | 判定 | 动作 |
|---|---|---|---|---|
| HTTP 分段引擎 | 静态分片（每片 1 线程） | **动态 work-stealing**（先完成线程领取后续分片） | IDM Next 更优 | 不抄 |
| Range / 断点续传 | `IsResumeSupported()` 探测 `Accept-Ranges` | `network_probe()` 已探测并保守降级 | 持平/IDM Next 略优 | 不抄 |
| **每服务器并发连接数** | WinInet 上限 **500**（`AdjustWinInetConnectionLimit`） | WinHTTP **默认 2**（未设置上限） | **IDM Next 有瓶颈** | ✅ **已优化**（提到 100） |
| 限速 | 全局 + 按流量模式（轻/中/重）+ 每段限速 | 全局令牌桶（100ms 窗口） | 基本持平，FDM 更细 | 暂不改（够用） |
| 写入策略 | 分片内聚合写缓存（减少 WriteFile 次数） | 每块 1MB 直写（粒度已合适） | 持平 | 不抄 |
| 浏览器集成 | COM/NSAPI 注入（已过时） | MV3 Native Messaging（更现代） | IDM Next 更优 | 不抄 |
| BT/磁力 | libtorrent（独立进程式） | aria2c 托管（单守护进程） | 架构不同，IDM Next 更轻 | 不抄 |
| 计划任务/自动关机 | ScheduleMgr | 已有 schedule_dialog + #31 关机 | 持平 | 不抄 |

## 二、重点：已落地的优化 —— WinHTTP 并发连接上限

**问题**：IDM Next 的纯 C 引擎用的是 WinHTTP（`src/core/network.c`），而 `network_init()` 没有设置 `WINHTTP_OPTION_MAX_CONNS_PER_SERVER`。WinHTTP 对同一服务器默认只允许 **2 条并发连接**。多线程分片下载时，一个文件的多个分片线程会互相排队，多线程加速几乎失效。

**FDM 的做法**（`InetFile/fsInternetSession.cpp`）：
```cpp
ULONG ul = 500;
InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &ul, sizeof(ul));
InternetSetOption(NULL, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &ul, sizeof(ul));
```

**已修改**（`src/core/network.c`，对标但取稳健值 100）：
```c
DWORD max_conns = 100;
WinHttpSetOption(g_hSession, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &max_conns, sizeof(max_conns));
WinHttpSetOption(g_hSession, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER, &max_conns, sizeof(max_conns));
```
编译已通过。多线程下载小文件/大文件均可真正并行，加速效果恢复。

## 三、IDM Next 优于 FDM 的点（无需抄）

1. **动态 work-stealing 分片**：`download_core.c` 的 `pool_worker` 把 chunk 数放大（`file_size / DEFAULT_CHUNK_SIZE`），先完成的线程自动领取后续分片，负载更均衡；FDM 是静态"片数=线程数"，慢片会拖垮整体。
2. **Range 探测稳健**：`network_probe()` 既看 `Accept-Ranges` 头，又对"无头但有长度"的情况保守允许尝试，比 FDM 的硬判定更稳。
3. **浏览器集成现代化**：MV3 Native Messaging（`browser-extension/`）+ 共享 aria2c 守护进程，比 FDM 的 COM/NSAPI 注入更安全、跨浏览器。
4. **资源占用更低**：BT/磁力走 aria2c 单守护进程托管，不内嵌 libtorrent，编译体积与常驻内存更可控。

## 四、可选后续（按需再做）

- **流量模式（轻/中/重）**：FDM 的 `vmsTrafficUsageModeMgr` 让用户按场景切换限速档位；IDM Next 目前只有单一全局限速，可加档位 UI（中等工作量）。
- **写入聚合缓存**：若后续遇到大量小分片场景，可参考 FDM 的分片内写缓存减少 WriteFile 系统调用（当前 1MB 块其实已基本够用）。
- **HTTP/2**：WinHTTP 可开启 `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL` 协商 h2，对高延迟链路有增益（需评估兼容性）。

## 五、参考源码位置

FDM 源码已克隆至项目内 `fdm-ref/`（GPLv3，仅供对照，不影响构建）。核心文件：
- `fsInternetDownloader.cpp` —— HTTP/FTP 下载器与分段（`_threadDownload`、`CreateSection`）
- `InetFile/fsInternetSession.cpp` —— WinInet 会话与并发上限（`AdjustWinInetConnectionLimit`）
- `vmsMaximumSpeedMeter.cpp` —— 限速/速度计量
- `Bittorrent/vmsBtDownloadManager.cpp` —— BT 后端（libtorrent）
