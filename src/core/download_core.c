/*
 * download_core.c
 * 多线程下载调度引擎实现（Windows 原生线程 + CRITICAL_SECTION）
 */
#include "download_core.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <io.h>    /* _chsize */
#include <sys/stat.h>  /* fstat, struct stat */

/* Windows 头文件 */
#ifdef _WIN32
#  include <windows.h>
#else
/* 非 Windows 平台桩定义（当前项目仅目标 Windows，此处仅为编译兼容） */
typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* LPVOID;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define INFINITE 0xFFFFFFFF
#define WINAPI
static void InitializeCriticalSection(void*c){(void)c;}
static void DeleteCriticalSection(void*c){(void)c;}
static void EnterCriticalSection(void*c){(void)c;}
static void LeaveCriticalSection(void*c){(void)c;}
static HANDLE CreateThread(void*a,size_t b,void*(*fn)(void*),void*c,int d,void*e){
    (void)a;(void)b;(void)fn;(void)c;(void)d;(void)e; return INVALID_HANDLE_VALUE;
}
static DWORD WaitForMultipleObjects(DWORD n,HANDLE*h,BOOL a,DWORD m){
    (void)n;(void)h;(void)a;(void)m; return 0;
}
static void CloseHandle(HANDLE h){(void)h;}
static void Sleep(DWORD ms){(void)ms;}
static DWORD GetTickCount64(void){return 0;}
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
#endif

/* ─────────────────────────────────────
 * 全局状态
 * ───────────────────────────────────── */
static DownloadTask    g_tasks[MAX_TASKS];
static int             g_task_count = 0;
static DownloadConfig  g_cfg;
static CRITICAL_SECTION g_lock;

/* 引擎正在拆除：置位后一律不再向 GUI 投递回调。
 * 存在的意义是关掉「worker 在锁内已读出回调指针 → 出锁调用」与
 * 「dlmgr_destroy 摘回调」之间的那个窗口。 */
static volatile int    g_shutdown = 0;

/* 全局请求选项（含代理）：每次网络请求复用，由 dlmgr_set_proxy 更新 */
static NetOptions g_net_opt;
static char       g_proxy_addr_buf[512];
/* 持久化的代理凭据 / UA 字符串缓冲：NetOptions 持有的是 const char*，
 * 需要稳定的存储地址，故用全局缓冲承载（配置变更时整体覆盖）。 */
static char       g_proxy_user_buf[256];
static char       g_proxy_pass_buf[256];
static char       g_ua_buf[512];
/* 引擎默认 UA（与 network_default_options 的 Chrome UA 不同，这里是纯下载器 UA；
 * 若用户在设置中指定了 UA 则覆盖此值）。 */
static const char *kDefaultUA = "IDM-Next/0.1 (Windows; libcurl)";
static int             g_initialized = 0;
static int             g_next_id     = 1;  /* 任务 ID 自增计数器 */

/* 全局限速器（令牌桶风格，按 100ms 滑动窗口节流总带宽） */
static CRITICAL_SECTION g_throttle_lock;
static int64_t          g_throttle_window_start = 0;
static int64_t          g_throttle_bytes       = 0;

/* ── 引擎行为追踪（默认关闭，零成本）──
 * 设 IDM_TRACE_ENGINE=1 后，把「设置是否真的下发到引擎」这类关键决策打到 stderr：
 * 匹配到的站点凭据、探测结果、分片数、分片失败原因。用于在无调试器的环境里
 * 定位「界面存了盘但引擎没用上」的问题（本项目反复踩过的一类坑）。
 * 不设该变量时只多一次 getenv，生产路径无输出。 */
static int trace_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("IDM_TRACE_ENGINE");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v;
}
#define TRACE(...) do { if (trace_on()) { fprintf(stderr, "[engine] " __VA_ARGS__); \
                         fprintf(stderr, "\n"); fflush(stderr); } } while (0)

#define DOWNLOAD_BLOCK_SIZE (1024 * 1024)  /* 每小块的下载大小（1MB），便于暂停时快速响应 */

/* ── UTF-8 路径安全文件 API ──
 * Windows 的 fopen / DeleteFileA / MoveFileA 走 ANSI 代码页，若路径含 UTF-8 编码的
 * 中文（下载文件名叫中文，或用户名是中文），会生成乱码文件名或操作失败。
 * 这里统一转 UTF-16 后调用宽字符 API，保证非 ASCII 路径正确。 */
static FILE *utf8_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024], wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return NULL;
    if (MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 16) <= 0) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static int utf8_delete(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return 0;
    return DeleteFileW(wpath) ? 1 : 0;
#else
    return remove(path) == 0 ? 1 : 0;
#endif
}

static int utf8_move(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t wsrc[1024], wdst[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, wsrc, 1024) <= 0) return 0;
    if (MultiByteToWideChar(CP_UTF8, 0, dst, -1, wdst, 1024) <= 0) return 0;
    return MoveFileW(wsrc, wdst) ? 1 : 0;
#else
    return rename(src, dst) == 0 ? 1 : 0;
#endif
}

/* ─────────────────────────────────────
 * 内部工具
 * ───────────────────────────────────── */
int64_t dl_time_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    return (int64_t)time(NULL) * 1000;
#endif
}

void dl_infer_filename(const char *url, char *out, int max_len) {
    if (!url || !out || max_len <= 0) return;
    const char *p = url;
    const char *q = strchr(p, '?');
    size_t len;
    if (q) len = (size_t)(q - p);
    else  len = strlen(p);

    const char *fname = p;
    const char *slash = strrchr(p, '/');
    if (slash) fname = slash + 1;

    if (len == 0 || (int)len >= max_len) {
        strncpy(out, "download.bin", (size_t)(max_len - 1));
        out[max_len - 1] = '\0';
        return;
    }
    strncpy(out, fname, len);
    out[len] = '\00';
}

/* 全局限速节流：在分片线程（线程池 worker）每下载一块后调用。
 * 基于 100ms 滑动窗口：若本窗口内累计下载字节超过配额，
 * 则阻塞（Sleep）剩余时间，使全局总带宽不超过 speed_limit_global。 */
static void throttle_limit(int64_t bytes) {
    if (g_cfg.speed_limit_global <= 0) return;
    EnterCriticalSection(&g_throttle_lock);
    int64_t now = dl_time_ms();
    if (g_throttle_window_start == 0) {
        g_throttle_window_start = now;
        g_throttle_bytes = 0;
    }
    int64_t elapsed = now - g_throttle_window_start;
    if (elapsed >= 100) {
        /* 新窗口：重置计数 */
        g_throttle_window_start = now;
        g_throttle_bytes = bytes;
        LeaveCriticalSection(&g_throttle_lock);
        return;
    }
    g_throttle_bytes += bytes;
    int64_t quota = g_cfg.speed_limit_global * elapsed / 1000;  /* 窗口内允许字节 */
    int64_t sleep_ms = 0;
    if (g_throttle_bytes > quota)
        sleep_ms = (g_throttle_bytes - quota) * 1000 / g_cfg.speed_limit_global;
    LeaveCriticalSection(&g_throttle_lock);
    if (sleep_ms > 0) Sleep((DWORD)sleep_ms);
}

/* 任务数组始终按 task_id 升序排列（add 追加递增 id；remove 用 memmove 保序；
 * load 完成后显式排序——文件是外部可编辑的输入，不能指望它本来就有序），
 * 故用二分查找 O(log n) 取代线性扫描 O(n)。 */
static DownloadTask *find_task(int task_id) {
    int lo = 0, hi = g_task_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int id = g_tasks[mid].task_id;
        if (id == task_id) return &g_tasks[mid];
        if (id < task_id) lo = mid + 1;
        else              hi = mid - 1;
    }
    return NULL;
}

/* 就地摘除某个槽位（memmove 保序）。**调用者必须已持有 g_lock**，
 * 且必须确认该槽位没有活动线程（worker_alive == 0）——有线程时挪动槽位会让
 * 线程手里的指针指向别的任务。 */
static int detach_slot(int task_id) {
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].task_id == task_id) {
            memmove(&g_tasks[i], &g_tasks[i + 1],
                    (size_t)(g_task_count - i - 1) * sizeof(DownloadTask));
            g_task_count--;
            return 1;
        }
    }
    return 0;
}

/* 生效的每任务并发连接数：由「请求值」与「每服务器连接数上限」取小。
 * 一个下载任务基本就是对同一台主机开 N 条连接，所以设置页的「每服务器连接数」
 * 天然就是每任务并发数的上限（与 FDM 的 max connections per server 同义）。
 * 两者中任一为 0/负数视为不限，用另一个；都无效则回落到 1。 */
static int effective_threads(int requested)
{
    int a = requested;
    int b = g_cfg.max_conn_per_server;
    if (a <= 0) a = b;
    if (b <= 0) b = a;
    int v = (a < b) ? a : b;
    if (v <= 0) v = 1;
    if (v > MAX_CHUNKS_PER_TASK) v = MAX_CHUNKS_PER_TASK;
    return v;
}

/* 取 URL 的主机名（小写不转换，比较时用 _stricmp）。无主机名则写出空串。 */
static void url_host_of(const char *url, char *out, int outlen)
{
    out[0] = '\0';
    if (!url) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* 把「站点登录」凭据按 URL 匹配到任务上（设置页该项原本只存盘、引擎从不下发）。
 * 匹配规则：
 *   match 含 "://"  → 当作 URL 前缀匹配
 *   否则            → 当作域名匹配，支持子域（match="example.com" 命中 "files.example.com"）
 * 命中后把凭据指针指向任务自己的缓冲区（不能用临时/全局缓冲，否则多任务会互相覆盖）。 */
static void apply_site_auth(DownloadTask *t)
{
    t->net_opts = g_net_opt;            /* 先继承全局选项（代理/UA/超时/SSL…） */
    t->auth_user[0] = '\0';
    t->auth_pass[0] = '\0';
    t->net_opts.auth_user = NULL;
    t->net_opts.auth_pass = NULL;
    if (g_cfg.site_login_count <= 0 || !t->url[0]) {
        TRACE("auth: 未配置站点登录（条数=%d）→ 请求不带凭据  url=%s",
              g_cfg.site_login_count, t->url);
        return;
    }

    char host[256];
    url_host_of(t->url, host, sizeof(host));

    for (int i = 0; i < g_cfg.site_login_count && i < MAX_SITE_LOGINS; i++) {
        const SiteCredential *c = &g_cfg.site_logins[i];
        if (!c->match[0] || !c->user[0]) continue;

        int hit = 0;
        if (strstr(c->match, "://")) {
            size_t n = strlen(c->match);
            hit = (_strnicmp(t->url, c->match, n) == 0);
        } else if (host[0]) {
            if (_stricmp(host, c->match) == 0) hit = 1;
            else {
                size_t hl = strlen(host), ml = strlen(c->match);
                /* 子域匹配：host = xxx.<match> */
                if (hl > ml + 1 && host[hl - ml - 1] == '.'
                    && _stricmp(host + hl - ml, c->match) == 0)
                    hit = 1;
            }
        }
        if (hit) {
            strncpy(t->auth_user, c->user, sizeof(t->auth_user) - 1);
            t->auth_user[sizeof(t->auth_user) - 1] = '\0';
            strncpy(t->auth_pass, c->pass, sizeof(t->auth_pass) - 1);
            t->auth_pass[sizeof(t->auth_pass) - 1] = '\0';
            t->net_opts.auth_user = t->auth_user;
            t->net_opts.auth_pass = t->auth_pass;
            TRACE("auth: host=%s 命中站点登录 match=\"%s\" user=\"%s\" → 已下发 HTTP 认证",
                  host, c->match, c->user);
            return;   /* 首条命中即生效 */
        }
    }
    TRACE("auth: host=%s 未命中任何站点登录（共 %d 条）→ 请求不带凭据",
          host, g_cfg.site_login_count);
}

/* 计算分片 */
static void calc_chunks(DownloadTask *t) {
    if (t->file_size <= 0 || t->file_size < MIN_CHUNK_SIZE) {
        t->chunk_count = 1;
        t->chunks[0].chunk_id   = 0;
        t->chunks[0].start      = 0;
        t->chunks[0].end        = t->file_size > 0 ? t->file_size - 1 : -1;
        t->chunks[0].downloaded = 0;
        t->chunks[0].done       = 0;
        t->chunks[0].retry_count = 0;
        return;
    }
    int cnt = t->thread_count;
    if (cnt <= 0) cnt = 1;
    /* 细分：保证每片不超过默认片大小，使线程池能动态调度（work-stealing），
     * 避免“片数=线程数”时某片偏慢拖垮整体速度。 */
    int by_size = (int)(t->file_size / DEFAULT_CHUNK_SIZE);
    if (by_size > cnt) cnt = by_size;
    if (cnt > MAX_CHUNKS_PER_TASK) cnt = MAX_CHUNKS_PER_TASK;
    if (cnt <= 0) cnt = 1;
    t->chunk_count = cnt;
    int64_t chunk_sz = t->file_size / cnt;
    for (int i = 0; i < cnt; i++) {
        t->chunks[i].chunk_id   = i;
        t->chunks[i].start      = (int64_t)i * chunk_sz;
        t->chunks[i].end        = (i == cnt - 1) ? (t->file_size - 1)
                                                     : ((int64_t)(i + 1) * chunk_sz - 1);
        t->chunks[i].downloaded = 0;
        t->chunks[i].done       = 0;
        t->chunks[i].retry_count = 0;
    }
}

/* ─────────────────────────────────────
 * 分片下载线程
 * ───────────────────────────────────── */
/* 单个分片的下载逻辑（被线程池动态调度）：整个分片只打开一次文件句柄，
 * 循环下载各小块并写入，减少 open/close 开销（磁盘缓存）。 */
static int download_one_chunk(DownloadTask *t, int cid) {
    if (!t || cid < 0 || cid >= t->chunk_count) return 1;

    DownloadChunk *c = &t->chunks[cid];
    if (c->done) return 0;

    /* c->end < 0 表示「从 start 一直下到流末尾」——即服务器没给出文件大小
     * （HEAD 被拒 401/403、分块传输、动态端点等）。
     * ⚠️ 这种情况下**不能**用 end - start + 1 算长度：那会得到 0，整个分片被当成
     * 空片直接标记完成，最终产出一个 0 字节的「下载完成」文件（真实踩过的坑）。
     * 这里改为一次性流式下载（range_end = -1 → 不发 Range 头，取整个响应体）。 */
    const int unknown_len = (c->end < 0);
    int64_t total_in_chunk = unknown_len ? -1 : (c->end - c->start + 1);
    int max_retry = g_cfg.retry_max > 0 ? g_cfg.retry_max : 3;
    char last_err[256] = "";

    /* 本分片打开一次文件句柄（整个任务共享同一 tmp 文件，各片写不同偏移、互不干扰） */
    FILE *fp = utf8_fopen(t->tmp_path, "r+b");
    if (!fp) fp = utf8_fopen(t->tmp_path, "w+b");
    if (!fp) {
        EnterCriticalSection(&g_lock);
        t->status = TASK_FAILED;
        snprintf(t->error_msg, sizeof(t->error_msg), "无法打开文件: %s", t->tmp_path);
        LeaveCriticalSection(&g_lock);
        return 1;
    }

    for (int r = 0; r <= max_retry; r++) {
        int any_failure = 0;

        /* 将整个分片拆成 DOWNLOAD_BLOCK_SIZE 的小块循环下载
         * 每块之间检查暂停/取消状态，实现优雅中断。
         * 长度未知时只跑一轮（一次请求取完整个响应体）。 */
        while (unknown_len || c->downloaded < total_in_chunk) {
            /* ── 检查暂停 / 取消状态 ── */
            EnterCriticalSection(&g_lock);
            if (t->status == TASK_PAUSED || t->status == TASK_CANCELLED) {
                LeaveCriticalSection(&g_lock);
                fclose(fp);
                return 0;  /* 优雅退出，保留 c->downloaded 作为断点 */
            }
            LeaveCriticalSection(&g_lock);

            /* 计算本次下载的范围（最多 DOWNLOAD_BLOCK_SIZE） */
            int64_t block_start = c->start + c->downloaded;
            int64_t remaining   = unknown_len ? -1 : (total_in_chunk - c->downloaded);
            int64_t block_size  = unknown_len ? -1
                                 : ((remaining > DOWNLOAD_BLOCK_SIZE)
                                    ? DOWNLOAD_BLOCK_SIZE : remaining);
            int64_t block_end   = unknown_len ? -1 : (block_start + block_size - 1);

            NetDownloadTask nd = {0};
            nd.url         = t->url;
            nd.save_path   = t->tmp_path;
            nd.range_start = block_start;
            nd.range_end   = block_end;
            nd.opt         = &t->net_opts;   /* 本任务选项（含代理 + 站点认证） */

            NetDownloadResult result = network_download_range_fp(&nd, fp);
            if (!result.success && result.range_ignored && t->chunk_count > 1) {
                /* 服务器不支持 Range：继续按分片写只会产出内容错位的坏文件。
                 * 停下所有分片，交给监督线程在全部退出后改成单连接重下（见函数尾）。 */
                EnterCriticalSection(&g_lock);
                t->range_downgrade = 1;
                t->status = TASK_CANCELLED;   /* 让其余分片也在下一块边界退出 */
                LeaveCriticalSection(&g_lock);
                TRACE("chunk %d: 服务器不支持 Range，降级为单连接重下", cid);
                fclose(fp);
                return 0;
            }
            if (!result.success) {
                any_failure = 1;
                /* 记下底层原因（如「HTTP 错误: 401」），用于回填任务错误文案，
                 * 否则用户只看得到笼统的「分片下载失败」。 */
                if (result.error_msg[0]) {
                    strncpy(last_err, result.error_msg, sizeof(last_err) - 1);
                    last_err[sizeof(last_err) - 1] = '\0';
                }
                TRACE("chunk %d: 块下载失败（range=%lld..%lld http=%ld）%s",
                      cid, (long long)block_start, (long long)block_end,
                      result.http_code, result.error_msg);
                break;  /* 本块失败，跳出小块循环，触发外层重试 */
            }

            EnterCriticalSection(&g_lock);
            c->downloaded += result.bytes_written;
            t->downloaded += result.bytes_written;
            /* 记录分片下载实际协商到的协议版本（首个非空即锁定，多线程同主机一致） */
            if (!t->http_version[0] && result.http_version[0]) {
                strncpy(t->http_version, result.http_version, sizeof(t->http_version) - 1);
                t->http_version[sizeof(t->http_version) - 1] = '\0';
            }

            /* 速度估算：全程平均值作兜底初值 + 滑动窗口 EMA(α=0.3) 平滑 */
            int64_t now = dl_time_ms();
            int64_t avg = 0;
            if (t->start_time > 0 && now > t->start_time)
                avg = (int64_t)(t->downloaded * 1000 / (now - t->start_time));

            if (t->start_time > 0) {
                if (avg > 0 && t->speed_ema == 0)
                    t->speed_ema = avg;   /* 首窗用平均值播种，避免初期为 0 */
                if (t->ema_last_ms == 0)
                    t->ema_last_ms = now;
                int64_t dt = now - t->ema_last_ms;
                if (dt < 0) dt = 0;
                t->ema_bytes     += result.bytes_written;
                t->ema_window_ms += dt;
                t->ema_last_ms    = now;
                /* 每累积约 800ms 刷新一次指数滑动平均 */
                if (t->ema_window_ms >= 800 && t->ema_bytes > 0) {
                    int64_t inst = t->ema_bytes * 1000 / t->ema_window_ms;
                    t->speed_ema = (t->speed_ema * 7 + inst * 3) / 10;
                    t->ema_bytes     = 0;
                    t->ema_window_ms = 0;
                }
                t->speed_bps = t->speed_ema ? t->speed_ema : avg;
                if (t->file_size > t->downloaded && t->speed_bps > 0)
                    t->eta_sec = (int)((t->file_size - t->downloaded) / t->speed_bps);
            }

            /* 在锁外调用外部回调：避免持有全局锁时重入引擎造成死锁，并缩短锁持有时间
             * 节流：每块 1MB 写盘即回调一次会在高并发下造成跨线程信号泛滥（频繁唤醒 GUI 事件循环）。
             *       仅当距上次回调 >=100ms 或本块为分片末块（强制刷最终值）时才真正下发；
             *       被跳过的增量值由 4Hz 下行刷新路径兜底，不影响进度显示正确性。 */
            {
                int64_t cb_dl = t->downloaded;
                int64_t cb_fs = t->file_size;
                int     cb_sp = (int)t->speed_bps;
                int     cb_id = t->task_id;
                void (*cb_fn)(int,int64_t,int64_t,int,void*) =
                    g_shutdown ? NULL : t->progress_cb;
                void  *cb_ud = g_shutdown ? NULL : t->progress_ud;

                int is_last_block = (c->downloaded >= total_in_chunk) ? 1 : 0;
                if (is_last_block || (now - t->last_progress_cb_ms) >= 100) {
                    t->last_progress_cb_ms = now;
                    LeaveCriticalSection(&g_lock);
                    if (cb_fn) cb_fn(cb_id, cb_dl, cb_fs, cb_sp, cb_ud);
                } else {
                    LeaveCriticalSection(&g_lock);
                }
            }

            /* 全局限速：按 100ms 窗口节流总带宽 */
            throttle_limit(result.bytes_written);

            /* 长度未知时一次请求即取完整个响应体，必须跳出，
             * 否则 unknown_len 恒为真会无限循环 */
            if (unknown_len) break;
        }

        if (!any_failure) {
            /* 所有小块全部成功 */
            EnterCriticalSection(&g_lock);
            c->done = 1;
            LeaveCriticalSection(&g_lock);
            fclose(fp);
            return 0;
        }

        /* 小块失败后重试（从断点继续） */
        c->retry_count++;
        if (r < max_retry)
            Sleep(1000 * (DWORD)(r + 1));
    }

    /* 所有重试失败 */
    int cb_id = t->task_id;
    void (*cb_fn)(int,int,void*) = NULL;
    void *cb_ud = NULL;
    EnterCriticalSection(&g_lock);
    t->status = TASK_FAILED;
    /* 优先把底层的具体原因（HTTP 401/404、无法连接…）透出去，GUI 才能显示
     * 「HTTP 错误: 401」而不是无信息量的「分片下载失败」。 */
    if (last_err[0])
        snprintf(t->error_msg, sizeof(t->error_msg), "%s", last_err);
    else
        snprintf(t->error_msg, sizeof(t->error_msg),
                 "分片 %d 下载失败，已重试 %d 次", cid, max_retry);
    cb_fn = g_shutdown ? NULL : t->complete_cb;
    cb_ud = g_shutdown ? NULL : t->complete_ud;
    LeaveCriticalSection(&g_lock);

    if (cb_fn) cb_fn(cb_id, 0, cb_ud);

    fclose(fp);
    return 1;
}

/* 线程池工作线程：循环从 dispatch_idx 领取未完成的片并下载（动态调度 / work-stealing）。
 * 先完成的线程自动领取后续分片，负载更均衡，避免“片=线程”时慢片拖垮整体。 */
static DWORD WINAPI pool_worker(LPVOID arg) {
    DownloadTask *t = (DownloadTask*)arg;
    if (!t) return 1;
    for (;;) {
        EnterCriticalSection(&g_lock);
        int i = t->dispatch_idx++;
        LeaveCriticalSection(&g_lock);
        if (i >= t->chunk_count) break;   /* 无更多片，退出 */
        if (t->chunks[i].done) continue;  /* 续传：跳过已完成片 */
        download_one_chunk(t, i);
    }
    return 0;
}

/* ─────────────────────────────────────
 * 监督线程：启动所有分片，等待完成，重命名文件
 * ───────────────────────────────────── */
static DWORD WINAPI start_task_threads(LPVOID arg) {
    DownloadTask *t = (DownloadTask*)arg;
    if (!t) return 1;

    /* 先登记「本槽位有活动线程」，再做任何事：worker_alive > 0 期间 dlmgr_remove
     * 不会 memmove 数组，t 指针在整个函数体内始终指向同一个任务。
     * 启动瞬间就已被删除的任务直接就地摘除槽位后返回。 */
    EnterCriticalSection(&g_lock);
    if (t->removed) {
        int tid = t->task_id;
        detach_slot(tid);
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    t->worker_alive++;
    LeaveCriticalSection(&g_lock);

    /* 预分配临时文件大小 */
    FILE *fp = utf8_fopen(t->tmp_path, "r+b");
    if (!fp) fp = utf8_fopen(t->tmp_path, "w+b");
    if (fp) {
        /* ⚠️ 必须用 _chsize_s（64 位）：Windows 上 long 是 32 位，`_chsize(fd, (long)file_size)`
         * 对 >2GB 的文件会把长度截断甚至变成负数，预分配失败后各分片按错误的文件长度写入，
         * 最终产出大小不对、内容错位的文件，而状态却是「已完成」。 */
        if (t->file_size > 0)
            _chsize_s(_fileno(fp), (__int64)t->file_size);
        fclose(fp);
    }

    EnterCriticalSection(&g_lock);
    t->dispatch_idx = 0;   /* 重置动态调度计数器 */
    t->start_time = dl_time_ms();
    t->status = TASK_RUNNING;
    /* 清掉上一次失败留下的原因，否则重试成功后 GUI 仍会读到过期的错误文案 */
    t->error_msg[0] = '\0';
    LeaveCriticalSection(&g_lock);

    /* 动态线程池：固定数量工作线程从 dispatch_idx 取片，先做完的线程继续领后续分片
     * （work-stealing），负载更均衡。
     * ⚠️ 并发数必须取本任务已定好的 thread_count —— 它在 dlmgr_add / 状态恢复时由
     *    effective_threads() 算出，等于 min(全局线程池大小, 每服务器连接数上限)。
     *    这里**不能**直接用 g_cfg.thread_pool_size：那样「每服务器连接数」就只影响
     *    分片数、完全约束不住真实并发（自测实测：封顶设 2 时并发峰值照样是 7），
     *    设置页那一项等于没生效——正是本项目反复排查的「死设置」。 */
    int pool = t->thread_count;
    if (pool <= 0 || pool > g_cfg.thread_pool_size)
        pool = g_cfg.thread_pool_size > 0 ? g_cfg.thread_pool_size : 1;
    if (pool > t->chunk_count) pool = t->chunk_count;
    if (pool <= 0) pool = 1;
    HANDLE handles[MAX_CHUNKS_PER_TASK];
    int launched = 0;

#ifdef _WIN32
    for (int i = 0; i < pool; i++) {
        HANDLE h = CreateThread(NULL, 0, pool_worker, t, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            handles[launched++] = h;
    }

    if (launched > 0) {
        WaitForMultipleObjects((DWORD)launched, handles, TRUE, INFINITE);
        for (int i = 0; i < launched; i++) CloseHandle(handles[i]);
    }
#endif

    /* 检查是否全部完成 */
    EnterCriticalSection(&g_lock);
    int all_done = 1;
    for (int i = 0; i < t->chunk_count; i++) {
        if (!t->chunks[i].done) { all_done = 0; break; }
    }
    if (all_done && t->status == TASK_RUNNING) {
        /* 兜底闸：一个字节都没收到、服务器也没声明大小 —— 绝不能算成功。
         * 否则会把「0 字节的空文件」重命名成正式文件、在界面上显示「已完成」，
         * 用户打开才发现是空的（比直接报错更难排查）。 */
        if (t->downloaded <= 0 && t->file_size <= 0) {
            t->status = TASK_FAILED;
            snprintf(t->error_msg, sizeof(t->error_msg),
                     "服务器未返回任何内容（可能被拒绝、需登录或链接无效）");
        } else {
            char final_path[768];
            snprintf(final_path, sizeof(final_path), "%s\\%s",
                     t->save_dir, t->filename);
            utf8_delete(final_path);
            if (utf8_move(t->tmp_path, final_path)) {
                t->status     = TASK_COMPLETED;
                t->downloaded = t->file_size > 0 ? t->file_size : t->downloaded;
            } else {
                t->status = TASK_FAILED;
                snprintf(t->error_msg, sizeof(t->error_msg), "文件重命名失败");
            }
        }
    }
    int st  = t->status;
    int tid = t->task_id;
    int relaunch_single = 0;
    int success = (st == TASK_COMPLETED);
    void (*cb)(int,int,void*) = g_shutdown ? NULL : t->complete_cb;
    void *ud = g_shutdown ? NULL : t->complete_ud;

    /* 被取消/被删除的任务：到这里所有工作线程都已退出、文件句柄已关闭，
     * 此时删 .idmtmp 才真的删得掉（dlmgr_cancel 里删会因文件被占用而失败，
     * 于是「取消」之后残留一个 .idmtmp 到下次启动）。 */
    if (st == TASK_CANCELLED)
        utf8_delete(t->tmp_path);

    t->worker_alive--;
    int gone = t->removed;
    int relaunch = (!gone && t->restart_pending);
    if (relaunch)
        t->restart_pending = 0;
    /* 不支持 Range 的降级重下：同样等线程清空后再动分片表 */
    if (!gone && !relaunch && t->range_downgrade && st == TASK_CANCELLED)
        relaunch_single = 1;
    if (gone) {
        /* 槽位现在可以安全挪动了。摘除后 t 已失效，不得再解引用。 */
        detach_slot(tid);
        cb = NULL;   /* 已被删除的任务不再回调：GUI 侧早已把它移出列表 */
    } else if (relaunch || relaunch_single) {
        /* 马上会自动重来，别先报一次「失败」——那会让任务行闪一下红色、
         * 还会弹一条完成通知，用户以为下载挂了。 */
        cb = NULL;
    }
    LeaveCriticalSection(&g_lock);

    if (cb) cb(tid, success, ud);

    /* 两种「线程清空后重来」：下载中收到的重新开始、以及不支持 Range 的降级重下。
     * 都走 dlmgr_restart 的「无活动线程」分支（删临时文件 + 重置 + 重新探测启动）。
     * 必须放在锁外调用——dlmgr_restart 自己会拿锁，并且会再创建线程。 */
    if (relaunch || relaunch_single)
        dlmgr_restart(tid);
    return 0;
}

/* ─────────────────────────────────────
 * 公开 API
 * ───────────────────────────────────── */

int dlmgr_init(const DownloadConfig *cfg) {
    if (g_initialized) return 0;
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = 0;
    g_shutdown = 0;   /* 允许 init → destroy → init 循环复用 */
    if (cfg) g_cfg = *cfg;
    else {
        g_cfg.thread_pool_size   = THREAD_POOL_SIZE;
        g_cfg.max_concurrent     = 3;
        g_cfg.retry_max          = 3;
        g_cfg.speed_limit_global = 0;
        /* 0 = 不额外封顶（GUI 启动后会经 applyToEngine 下发设置页的真实值） */
        g_cfg.max_conn_per_server = 0;
        g_cfg.site_login_count    = 0;
        strncpy(g_cfg.default_save_dir, "C:\\Users\\Public\\Downloads",
                sizeof(g_cfg.default_save_dir) - 1);
    }
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_throttle_lock);
    /* 初始化全局请求选项（默认不含代理，沿用系统代理） */
    g_net_opt = network_default_options();
    g_net_opt.proxy_addr = g_proxy_addr_buf;
    network_init();
    g_initialized = 1;
    return 0;
}

void dlmgr_destroy(void) {
    if (!g_initialized) return;

    /* ① 先摘掉所有回调并请求取消。
     * 目的是让「引擎已开始拆、worker 还在跑」这段时间里，不再有任何一次
     * 跨线程回调被投递给正在析构的 GUI 对象（跳板是 static_cast<DownloadManager*>(ud)
     * 后直接解引用，指针非空但可能已悬空，`if (self)` 挡不住）。
     * 回调是成对读出的（函数指针 + ud），所以置空时也必须成对置空。 */
    EnterCriticalSection(&g_lock);
    g_shutdown = 1;
    for (int i = 0; i < g_task_count; i++) {
        g_tasks[i].progress_cb = NULL;
        g_tasks[i].progress_ud = NULL;
        g_tasks[i].complete_cb = NULL;
        g_tasks[i].complete_ud = NULL;
        if (g_tasks[i].status == TASK_RUNNING)
            g_tasks[i].status = TASK_CANCELLED;
    }
    LeaveCriticalSection(&g_lock);

    /* ② 等 worker 真的退出。
     * 取消检查发生在每个 1 MiB 小块之间，所以线程会很快在下一个块边界退出；
     * 之前是「置位后立刻 network_cleanup + DeleteCriticalSection」，等于在活线程
     * 脚下抽掉锁 —— 线程随后 EnterCriticalSection 就是 UB（实测表现为偶发退出崩溃）。 */
    int alive = 0;
    for (int waited = 0; waited < 3000; waited += 10) {
        alive = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_task_count; i++)
            alive += g_tasks[i].worker_alive;
        LeaveCriticalSection(&g_lock);
        if (alive == 0) break;
        Sleep(10);
    }

    /* 如果网络卡住导致 3 秒内没退干净：宁可不删临界区（进程马上退出，泄漏无意义），
     * 也不要在活线程还在取锁的时候把它删掉。 */
    const int clean = (alive == 0);
    if (!clean)
        TRACE("destroy: 仍有 %d 个 worker 未退出，保留临界区不删（避免删锁后线程再取锁）",
              alive);
    else
        TRACE("destroy: 所有 worker 已退出");

    network_cleanup();
    if (clean) {
        DeleteCriticalSection(&g_lock);
        DeleteCriticalSection(&g_throttle_lock);
    }
    g_initialized = 0;
}

int dlmgr_add(const char *url,
               const char *save_dir,
               const char *filename,
               int thread_count,
               void (*progress_cb)(int,int64_t,int64_t,int,void*),
               void *progress_ud,
               void (*complete_cb)(int,int,void*),
               void *complete_ud) {
    if (!g_initialized || !url || *url == '\00') return -1;

    EnterCriticalSection(&g_lock);
    if (g_task_count >= MAX_TASKS) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }

    DownloadTask *t = &g_tasks[g_task_count];
    memset(t, 0, sizeof(DownloadTask));
    t->task_id = g_next_id++;

    strncpy(t->url, url, sizeof(t->url) - 1);

    const char *dir = (save_dir && save_dir[0])
                     ? save_dir : g_cfg.default_save_dir;
    strncpy(t->save_dir, dir, sizeof(t->save_dir) - 1);

    if (filename && filename[0]) {
        strncpy(t->filename, filename, sizeof(t->filename) - 1);
        t->filename_from_user = 1;
    } else {
        dl_infer_filename(url, t->filename, sizeof(t->filename));
        t->filename_from_user = 0;
    }

    snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
             t->save_dir, t->filename);

    t->status       = TASK_PENDING;
    /* 并发数受「每服务器连接数」上限约束，设置页该项才真正生效 */
    t->thread_count = effective_threads(thread_count > 0 ? thread_count
                                                         : g_cfg.thread_pool_size);
    /* 建立本任务的网络选项（含按 URL 命中「站点登录」的 HTTP 认证） */
    apply_site_auth(t);
    t->speed_ema    = 0;
    t->ema_bytes    = 0;
    t->ema_window_ms = 0;
    t->ema_last_ms  = 0;
    t->last_progress_cb_ms = 0;
    t->progress_cb  = progress_cb;
    t->progress_ud  = progress_ud;
    t->complete_cb  = complete_cb;
    t->complete_ud  = complete_ud;

    int id = t->task_id;
    g_task_count++;
    LeaveCriticalSection(&g_lock);
    return id;
}

int dlmgr_start(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (!t) { LeaveCriticalSection(&g_lock); return -1; }
    if (t->status == TASK_RUNNING) { LeaveCriticalSection(&g_lock); return 0; }

    /* 是否需要先探测文件大小并计算分片。
     * ⚠️ 判据必须是「本任务还没分过段」而**不是**「状态是 PENDING」：
     *   ① 新建后立刻被暂停的任务是 PAUSED，用状态判断会跳过探测，chunk_count 停在 0，
     *      接着 start_task_threads 拿着 0 个分片空转（状态显示下载中，实际一动不动）；
     *   ② 从状态文件恢复、chunks 为空数组的任务同理。
     * 分片一旦算过（含"长度未知"的单流兜底 chunk_count=1）就保持，续传靠 c->downloaded。 */
    int need_size = (t->chunk_count == 0) ? 1 : 0;
    /* 每次启动都重算一次本任务的网络选项：设置可能在任务创建之后才改（代理/UA/站点登录），
     * 而且从状态文件恢复的任务 net_opts 是空的，必须补上。 */
    apply_site_auth(t);
    TRACE("start: id=%d need_size=%d threads=%d（上限 %d）url=%s",
          task_id, need_size, t->thread_count, g_cfg.max_conn_per_server, t->url);
    LeaveCriticalSection(&g_lock);

    if (need_size) {
        /* 单请求探测：文件大小 + Range 支持（2 RTT → 1 RTT） */
        NetworkProbe probe = network_probe(t->url, &t->net_opts);
        int64_t fsize = probe.file_size;
        int     supports = probe.supports_range;
        TRACE("probe: success=%d http=%ld size=%lld range=%d ver=%s "
              "（auth_user=%s）",
              probe.success, probe.http_code, (long long)fsize, supports,
              probe.http_version,
              t->net_opts.auth_user ? t->net_opts.auth_user : "(null)");

        EnterCriticalSection(&g_lock);
        t = find_task(task_id);
        if (!t) { LeaveCriticalSection(&g_lock); return -1; }
        t->file_size = fsize;
        /* 记录 HEAD 探测实际协商到的协议版本（HTTP/2 仅 HTTPS 出现） */
        if (probe.http_version[0]) {
            strncpy(t->http_version, probe.http_version, sizeof(t->http_version) - 1);
            t->http_version[sizeof(t->http_version) - 1] = '\0';
        }

        /* 若用户未显式指定文件名，采用服务器建议的文件名（Content-Disposition）。
         * 这能修正大量动态下载链接 / API 下载 / 无扩展名 URL 的文件名错误问题。 */
        if (!t->filename_from_user && probe.suggested_filename[0]) {
            strncpy(t->filename, probe.suggested_filename, sizeof(t->filename) - 1);
            t->filename[sizeof(t->filename) - 1] = '\0';
            snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
                     t->save_dir, t->filename);
        }

        /* range_downgrade：已经实测过这台服务器不吃 Range（回 200+整份文件），
         * HEAD 的 Accept-Ranges 不可信，直接按单连接处理，不再分片。 */
        if (supports && fsize >= MIN_CHUNK_SIZE && !t->range_downgrade)
            calc_chunks(t);
        else {
            t->chunk_count = 1;
            t->chunks[0].chunk_id   = 0;
            t->chunks[0].start      = 0;
            /* end = -1 → 连 Range 头都不发（下载范围未知 = 取整个响应体），
             * 就当一个普通单连接下载，不会再被服务器回一次 200。 */
            t->chunks[0].end        = t->range_downgrade ? -1
                                                         : (fsize > 0 ? fsize - 1 : -1);
            t->chunks[0].downloaded = 0;
            t->chunks[0].done       = 0;
            t->chunks[0].retry_count = 0;
        }
        TRACE("chunks: 任务 %d → %d 片（size=%lld range=%d threads=%d）",
              task_id, t->chunk_count, (long long)fsize, supports, t->thread_count);
        LeaveCriticalSection(&g_lock);
    }

#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, start_task_threads,
                            find_task(task_id), 0, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    else return -1;
#endif
    return 0;
}

int dlmgr_pause(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        /* 排队中（PENDING）也要真的置为 PAUSED。
         * 原来的实现只在 RUNNING 时改状态、对 PENDING 静默返回 0，于是 GUI 把行标成
         * 「已暂停」、引擎里却还是 PENDING：一旦退出就会以 PENDING 落盘，下次启动
         * 队列调度器照样把它拉起来下载 —— 用户的「暂停」等于没按。
         * 终态（已完成/已失败/已取消）不动，避免把结束的任务改回可续状态。 */
        if (t->status == TASK_PENDING || t->status == TASK_RUNNING)
            t->status = TASK_PAUSED;
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_cancel(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        t->status = TASK_CANCELLED;
        /* 临时文件只能在没有活动线程时删：有线程还开着这个文件时 DeleteFileW 会因
         * 占用而失败（fopen 不共享删除权限），于是「取消」看着成功、.idmtmp 却留着。
         * 有线程的情况交给 start_task_threads 收尾时删。 */
        if (t->worker_alive == 0)
            utf8_delete(t->tmp_path);
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_restart(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (!t) { LeaveCriticalSection(&g_lock); return -1; }

    /* 还在下载中就直接重置分片、并让 dlmgr_start 再起一组线程，
     * 会变成两组线程同时写同一个 .idmtmp（旧线程的指针还盯着 chunks[]，
     * 而 chunks 已经被 memset 了）——产出文件必然损坏。
     * 这里只置位并让它停下，等最后一个线程退出时由监督线程重新发起。 */
    if (t->worker_alive > 0) {
        t->restart_pending = 1;
        t->status = TASK_CANCELLED;
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    /* 删除旧临时文件 */
    utf8_delete(t->tmp_path);

    /* 重置任务为初始状态，保留 URL / 路径 / 文件名 / 回调 */
    t->status     = TASK_PENDING;
    t->downloaded = 0;
    t->file_size  = -1;      /* 强制重新探测文件大小 */
    t->speed_bps  = 0;
    t->speed_ema  = 0;
    t->ema_bytes     = 0;
    t->ema_window_ms = 0;
    t->ema_last_ms   = 0;
    t->eta_sec    = 0;
    t->start_time = 0;
    memset(t->chunks, 0, sizeof(t->chunks));
    /* chunk_count 必须跟着清零：dlmgr_start 现在用「chunk_count == 0」判断是否需要
     * 重新探测分段（不再看状态）。只清数组不清计数，等于对外宣称"已经分好段了"，
     * 而 chunks[] 全是 0 值 —— 会静默产生一个从头覆盖的坏文件。 */
    t->chunk_count = 0;
    LeaveCriticalSection(&g_lock);

    /* dlmgr_start 检测到「尚未分段」会重新获取文件大小并计算分片 */
    return dlmgr_start(task_id);
}

int dlmgr_remove(int task_id) {
    int found = 0;
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        found = 1;
        if (t->worker_alive > 0) {
            /* 还有线程在跑（删除一个正在下载的任务就会走到这里）。
             * 此刻 memmove 把整个结构体挪一格，线程手里的指针就会指向**另一个任务**：
             * 它会把自己的进度、分片数据、乃至「下载完成」状态写到那个无辜任务身上，
             * 而那个任务的文件会被重命名成当前这个任务的名字。跨任务污染，且无法事后察觉。
             * 正确做法：只登记删除意图并置为「已取消」（线程据此优雅退出），
             * 等最后一个线程退出时由它就地摘除槽位。*/
            t->removed = 1;
            t->status  = TASK_CANCELLED;
        } else {
            detach_slot(task_id);
        }
    }
    LeaveCriticalSection(&g_lock);
    return found ? 0 : -1;
}

/* 重新绑定任务的进度/完成回调（dlmgr_load_state 恢复的任务用） */
int dlmgr_set_callbacks(int task_id,
                        void (*progress_cb)(int,int64_t,int64_t,int,void*), void* progress_ud,
                        void (*complete_cb)(int,int,void*), void* complete_ud)
{
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        t->progress_cb = progress_cb;
        t->progress_ud = progress_ud;
        t->complete_cb = complete_cb;
        t->complete_ud = complete_ud;
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_get_task_info(int task_id, TaskInfo *info) {
    if (!info) return -1;
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        info->task_id    = t->task_id;
        info->downloaded = t->downloaded;
        info->file_size  = t->file_size;
        info->speed_bps  = t->speed_bps;
        info->eta_sec    = t->eta_sec;
        info->status     = (int)t->status;
        strncpy(info->filename, t->filename, sizeof(info->filename) - 1);
        info->filename[sizeof(info->filename) - 1] = '\0';
        strncpy(info->http_version, t->http_version, sizeof(info->http_version) - 1);
        info->http_version[sizeof(info->http_version) - 1] = '\0';
        /* 分片概况：clamp 到数组上限，避免越界读 */
        int cc = t->chunk_count;
        if (cc < 0) cc = 0;
        if (cc > MAX_CHUNKS_PER_TASK) cc = MAX_CHUNKS_PER_TASK;
        info->chunk_count = cc;
        int done = 0;
        for (int i = 0; i < cc; i++) {
            if (t->chunks[i].done) done++;
        }
        info->chunks_done = done;
        strncpy(info->error_msg, t->error_msg, sizeof(info->error_msg) - 1);
        info->error_msg[sizeof(info->error_msg) - 1] = '\0';
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

/* 批量获取所有任务详情：单次持锁遍历 g_tasks[]，填充顺序与 dlmgr_list 返回的 ids 完全一致，
 * 调用方可按索引对齐直接使用，省去每任务一次 EnterCriticalSection + find_task 二分查找。
 * 这是 GUI 4Hz 刷新热路径（refreshFromEngine）的针对性优化——原来每 tick 为每个任务单独
 * 加锁并做二分查找，n 个任务即 n 次锁竞争，会与下载线程的进度回调抢锁。 */
int dlmgr_get_task_infos(TaskInfo *out, int max_count) {
    if (!out || !g_initialized) return 0;
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    for (int i = 0; i < cnt; i++) {
        DownloadTask *t = &g_tasks[i];
        out[i].task_id    = t->task_id;
        out[i].downloaded = t->downloaded;
        out[i].file_size  = t->file_size;
        out[i].speed_bps  = t->speed_bps;
        out[i].eta_sec    = t->eta_sec;
        out[i].status     = (int)t->status;
        strncpy(out[i].filename, t->filename, sizeof(out[i].filename) - 1);
        out[i].filename[sizeof(out[i].filename) - 1] = '\0';
        strncpy(out[i].http_version, t->http_version, sizeof(out[i].http_version) - 1);
        out[i].http_version[sizeof(out[i].http_version) - 1] = '\0';
        /* 与 dlmgr_get_task_info 保持一致：这两个字段必须一并填充，
         * 否则调用方读到的是未初始化内存（TaskInfo 是栈上分配的） */
        int cc = t->chunk_count;
        if (cc < 0) cc = 0;
        if (cc > MAX_CHUNKS_PER_TASK) cc = MAX_CHUNKS_PER_TASK;
        out[i].chunk_count = cc;
        int done = 0;
        for (int c = 0; c < cc; c++) {
            if (t->chunks[c].done) done++;
        }
        out[i].chunks_done = done;
        strncpy(out[i].error_msg, t->error_msg, sizeof(out[i].error_msg) - 1);
        out[i].error_msg[sizeof(out[i].error_msg) - 1] = '\0';
    }
    LeaveCriticalSection(&g_lock);
    return cnt;
}

int dlmgr_list(int *ids, int max_count) {
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    for (int i = 0; i < cnt; i++) ids[i] = g_tasks[i].task_id;
    LeaveCriticalSection(&g_lock);
    return cnt;
}

void dlmgr_set_speed_limit(int bytes_per_sec) {
    EnterCriticalSection(&g_lock);
    g_cfg.speed_limit_global = bytes_per_sec;
    LeaveCriticalSection(&g_lock);
}

void dlmgr_set_proxy(int type, const char *host, int port,
                     const char *user, const char *pass) {
    /* 关闭代理：恢复默认选项，沿用系统代理 */
    if (type == PROXY_NONE || !host || !host[0] || port <= 0) {
        g_net_opt = network_default_options();
        g_net_opt.proxy_addr = g_proxy_addr_buf;
        g_net_opt.proxy_user = NULL;
        g_net_opt.proxy_pass = NULL;
        g_net_opt.proxy_type = PROXY_NONE;
        g_proxy_addr_buf[0] = '\0';
        g_proxy_user_buf[0] = '\0';
        g_proxy_pass_buf[0] = '\0';
        return;
    }
    g_net_opt.proxy_type = (ProxyType)type;
    /* libcurl 语法：地址统一为 "host:port"，由 PROXYTYPE 区分 HTTP/SOCKS。
     * （旧版 WinHTTP 的 "socks=host:port" 前缀在 libcurl 下无法解析，会导致 SOCKS 代理失效） */
    snprintf(g_proxy_addr_buf, sizeof(g_proxy_addr_buf),
             "%s:%d", host, port);
    g_net_opt.proxy_addr = g_proxy_addr_buf;
    g_net_opt.proxy_user = NULL;
    g_net_opt.proxy_pass = NULL;
    if (user && user[0]) {
        strncpy(g_proxy_user_buf, user, sizeof(g_proxy_user_buf) - 1);
        g_proxy_user_buf[sizeof(g_proxy_user_buf) - 1] = '\0';
        g_net_opt.proxy_user = g_proxy_user_buf;
    }
    if (pass && pass[0]) {
        strncpy(g_proxy_pass_buf, pass, sizeof(g_proxy_pass_buf) - 1);
        g_proxy_pass_buf[sizeof(g_proxy_pass_buf) - 1] = '\0';
        g_net_opt.proxy_pass = g_proxy_pass_buf;
    }
}

/* ─────────────────────────────────────
 * 持久化（JSON 格式，cJSON 就绪前用简化格式）
 * ───────────────────────────────────── */
/* ─────────────────────────────────────
 * 状态文件（tasks.json）的字符串读写
 *
 * 为什么要成对处理：写的时候必须转义，读的时候必须还原，否则往返不闭环。
 * 而「读」还要兼容**旧版写出的未转义文件**（旧版直接把 Windows 路径原样写出，
 * 形如 "dir":"C:\Users\me"），所以用文件头的 "idmFormat":2 做版本判别：
 * 有标记才按 JSON 转义语义还原，没有就沿用原来的逐字符扫描。
 * 不做这个区分的话，"C:\temp" 里的 \t 会被还原成制表符，路径就悄悄坏了。
 * ───────────────────────────────────── */

/* 按 JSON 规则转义写入。必须处理 '\\' 和 '"'：
 * Windows 目录里全是反斜杠，原样写出去会得到 "\Users" 这种非法转义序列，
 * 严格解析器（QJsonDocument）会因一个字符读不出整份文件 —— 所有任务一起丢。
 * 控制字符转成 \u00XX，保证输出永远是合法 JSON。 */
static void json_write_escaped(FILE *fp, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            /* UTF-8 多字节原样透传（0x80+），本身就是合法 JSON 字符 */
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", (unsigned)*p);
                else           fputc((int)*p, fp);
        }
    }
}

/* 读一个 JSON 字符串字面量：p 指向开头的引号之后，结果写进 out。
 * 返回下一个待处理位置（越过收尾引号）。
 * 未知转义序列按「原样保留反斜杠」处理——宽松是关键，宁可少还原也不能吃掉字符。 */
static const char *json_read_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            char dec = 0;
            switch (c) {
                case '"':  dec = '"';  break;
                case '\\': dec = '\\'; break;
                case '/':  dec = '/';  break;
                case 'n':  dec = '\n'; break;
                case 'r':  dec = '\r'; break;
                case 't':  dec = '\t'; break;
                case 'b':  dec = '\b'; break;
                case 'f':  dec = '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (sscanf(p + 2, "%4x", &cp) == 1 && cp > 0 && cp < 0x80) {
                        if (n + 1 < cap) out[n++] = (char)cp;
                        p += 6;
                        continue;
                    }
                    break;   /* \uXXXX 超出 ASCII：走下面的按未知转义保留 */
                }
                default: break;
            }
            if (dec) {
                if (n + 1 < cap) out[n++] = dec;
                p += 2;
                continue;
            }
            if (n + 2 < cap) { out[n++] = '\\'; out[n++] = c; }
            p += 2;
            continue;
        }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[(n < cap) ? n : (cap ? cap - 1 : 0)] = '\0';
    return (*p == '"') ? p + 1 : p;
}

/* 读出 "key":"值为字符串" 到 out；找不到返回 0。
 * 调用方需保证 p 落在本任务对象的范围内（沿用原来的 strstr 定位习惯）。 */
static int json_read_field(const char *p, const char *key, int escaped,
                           char *out, size_t cap) {
    const char *fld = strstr(p, key);
    if (!fld) return 0;
    fld += strlen(key);
    if (*fld != '"') return 0;
    fld++;
    if (escaped) {
        json_read_string(fld, out, cap);
    } else {
        const char *end = strchr(fld, '"');
        size_t len = end ? (size_t)(end - fld) : strlen(fld);
        if (len >= cap) len = cap - 1;
        memcpy(out, fld, len);
        out[len] = '\0';
    }
    return 1;
}

int dlmgr_save_state(const char *path) {
    if (!path) return -1;
    FILE *fp = utf8_fopen(path, "w");
    if (!fp) return -1;

    /* idmFormat 标记：读的时候据此判断该不该按 JSON 转义语义还原。
     * 老版本文件没有这一行，仍然走宽松扫描，不受影响。 */
    fprintf(fp, "{\n  \"idmFormat\": 2,\n  \"tasks\": [\n");
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_task_count; i++) {
        DownloadTask *t = &g_tasks[i];
        fprintf(fp, "    {\"id\":%d, \"url\":\"", t->task_id);
        json_write_escaped(fp, t->url);
        fprintf(fp, "\", \"file\":\"");
        json_write_escaped(fp, t->filename);
        fprintf(fp, "\", \"dir\":\"");
        json_write_escaped(fp, t->save_dir);
        fprintf(fp, "\", ");
        fprintf(fp,
            "\"size\":%lld, \"downloaded\":%lld, \"status\":%d,\n",
            (long long)t->file_size,
            (long long)t->downloaded,
            (int)t->status);

        fprintf(fp, "     \"chunks\":[");
        for (int j = 0; j < t->chunk_count; j++) {
            if (j > 0) fprintf(fp, ",");
            fprintf(fp,
                "{\"start\":%lld, \"end\":%lld, \"downloaded\":%lld, \"done\":%d}",
                (long long)t->chunks[j].start,
                (long long)t->chunks[j].end,
                (long long)t->chunks[j].downloaded,
                t->chunks[j].done);
        }
        fprintf(fp, "] }%s\n",
                (i < g_task_count - 1) ? "," : "");
    }
    LeaveCriticalSection(&g_lock);
    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    return 0;
}

/* 清空 g_tasks[]，供 dlmgr_load_state() 在载入前调用。
 *
 * 存在的理由：load 的语义是「用文件内容重建队列」，不是「往现有队列尾部追加」。
 * 旧实现直接往下追加，于是同一进程内 load 两次就会把任务翻倍。这不是理论问题：
 * 走查工具里连开两个 MainWindow（各自构造一次），真实用户目录里的 tasks.json
 * 就从 56 条变成 112 条，并在退出时被写回磁盘 —— 任务列表凭空空了一半的「重复项」，
 * 而且重复的 task_id 会让 find_task() 的二分查找在同一 id 上反复命中同一条。
 * 与其要求调用方「一辈子只 load 一次」，不如让 load 自己保证幂等。
 *
 * 清空的顺序有讲究：先把 RUNNING 标成 CANCELLED 让 worker 在下一个块边界退出，
 * 等它们真的退场后再 memset —— 否则线程还在往 g_tasks[i] 里写，抹完它继续写，
 * 下一个 dlmgr_add() 复用同一槽位就成了两个写入者。 */
static void reset_all_tasks(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_RUNNING)
            g_tasks[i].status = TASK_CANCELLED;
    }
    LeaveCriticalSection(&g_lock);

    /* 与 dlmgr_destroy() 同样的等待窗口：取消失效（网络卡住）时不无限等，
     * 3 秒后照常清空 —— 此时那些任务已被标记取消，不会再被当成有效任务。 */
    for (int waited = 0; waited < 3000; waited += 10) {
        int alive = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_task_count; i++)
            alive += g_tasks[i].worker_alive;
        LeaveCriticalSection(&g_lock);
        if (alive == 0) break;
        Sleep(10);
    }

    EnterCriticalSection(&g_lock);
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = 0;
    g_next_id = 1;      /* 新任务从 1 重新编号，与清空后的空队列保持一致 */
    LeaveCriticalSection(&g_lock);
}

int dlmgr_load_state(const char *json_path) {
    if (!json_path) return -1;

    /* 载入前先清空既有任务（见 reset_all_tasks 上的说明）。
     * 放在读文件成功之后再调用会更省事，但那样「文件打不开」就会留下半新半旧的
     * 队列；这里是启动路径，清空后失败也只是回到空列表，语义更好预期。 */
    reset_all_tasks();

    /* 读取整个 JSON 文件 */
    FILE *fp = utf8_fopen(json_path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return -1; }
    fseek(fp, 0, SEEK_SET);

    char *buf = (char*)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    if (nread > (size_t)fsize) nread = (size_t)fsize;  /* 防止 tainted index 越界 */
    buf[nread] = '\0';
    fclose(fp);

    EnterCriticalSection(&g_lock);

    /* 本次文件是否由「转义版」写入器产出（见 json_write_escaped 上的说明）。
     * 旧文件没有这个标记 → 沿用原来的逐字符扫描，避免把 "C:\temp" 的 \t 还原成制表符。 */
    const int escaped = (strstr(buf, "\"idmFormat\"") != NULL);
    if (escaped) TRACE("load_state: 检测到转义版状态文件，按 JSON 转义语义还原");

    /* 定位到 "tasks" 数组 */
    char *p = strstr(buf, "\"tasks\"");
    if (!p) goto done;
    p = strchr(p, '[');
    if (!p) goto done;
    p++;  /* 跳过 '[' */

    /* 循环解析每个任务对象 */
    while (*p) {
        /* 跳过空白和逗号，找 '{' 或 ']' */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        if (g_task_count >= MAX_TASKS) break;
        DownloadTask *t = &g_tasks[g_task_count];
        memset(t, 0, sizeof(DownloadTask));

        /* ── 解析 "id":数字 ── */
        char *fld = strstr(p, "\"id\":");
        if (!fld) break;
        fld += 5; while (*fld == ' ' || *fld == '\t') fld++;
        t->task_id = atoi(fld);
        if (t->task_id >= g_next_id) g_next_id = t->task_id + 1;

        /* ── 解析 "url"/"file"/"dir" ──
         * key 只给到冒号（不含值那侧的引号）：json_read_field 内部会校验并跳过它。
         * 若把引号也写进 key，函数会越过它再去要求一个引号，结果三个字段全读空。 */
        json_read_field(p, "\"url\":",  escaped, t->url,      sizeof(t->url));
        json_read_field(p, "\"file\":", escaped, t->filename, sizeof(t->filename));
        json_read_field(p, "\"dir\":",  escaped, t->save_dir, sizeof(t->save_dir));

        /* ── 解析 "size":数字 ── */
        fld = strstr(p, "\"size\":");
        if (fld) { fld += 7; while (*fld == ' ' || *fld == '\t') fld++; t->file_size = (int64_t)atoll(fld); }

        /* ── 解析 "downloaded":数字 ── */
        fld = strstr(p, "\"downloaded\":");
        if (fld) { fld += 13; while (*fld == ' ' || *fld == '\t') fld++; t->downloaded = (int64_t)atoll(fld); }

        /* ── 解析 "status":数字 ── */
        fld = strstr(p, "\"status\":");
        if (fld) {
            fld += 9; while (*fld == ' ' || *fld == '\t') fld++;
            t->status = (TaskStatus)atoi(fld);
            /* 重启后，RUNNING 任务变为 PAUSED（需要手动恢复） */
            if (t->status == TASK_RUNNING) t->status = TASK_PAUSED;
        }

        /* ── 解析 "chunks":[...] ── */
        char *chunk_arr = strstr(p, "\"chunks\":[");
        if (chunk_arr) {
            char *cp = strchr(chunk_arr, '[');
            if (cp) {
                cp++;  /* 跳过 '[' */
                t->chunk_count = 0;
                while (*cp && t->chunk_count < MAX_CHUNKS_PER_TASK) {
                    while (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r' || *cp == ',') cp++;
                    if (*cp == ']') break;
                    if (*cp != '{') break;

                    DownloadChunk *c = &t->chunks[t->chunk_count];
                    char *cf;

                    cf = strstr(cp, "\"start\":");
                    if (cf) { cf += 8; while (*cf == ' ' || *cf == '\t') cf++; c->start = atoll(cf); }

                    cf = strstr(cp, "\"end\":");
                    if (cf) { cf += 6; while (*cf == ' ' || *cf == '\t') cf++; c->end = atoll(cf); }

                    cf = strstr(cp, "\"downloaded\":");
                    if (cf) { cf += 13; while (*cf == ' ' || *cf == '\t') cf++; c->downloaded = atoll(cf); }

                    cf = strstr(cp, "\"done\":");
                    if (cf) { cf += 7; while (*cf == ' ' || *cf == '\t') cf++; c->done = atoi(cf); }

                    c->chunk_id = t->chunk_count;
                    c->retry_count = 0;
                    t->chunk_count++;

                    /* 跳到下一个 chunk 或结束 */
                    cp = strchr(cp, '}');
                    if (cp) cp++;
                }
                /* chunks 数组解析完成，将 p 更新到 ']' 之后 */
                char *bracket_close = strchr(cp, ']');
                if (bracket_close) p = bracket_close + 1;
            }
        }

        /* 恢复其他字段 */
        snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
                 t->save_dir[0] ? t->save_dir : g_cfg.default_save_dir,
                 t->filename);
        t->progress_cb  = NULL;
        t->progress_ud  = NULL;
        t->complete_cb  = NULL;
        t->complete_ud  = NULL;
        t->speed_bps    = 0;
        t->eta_sec      = 0;
        t->start_time   = 0;
        t->last_progress_cb_ms = 0;
        /* 从状态文件恢复的任务也要过一遍并发上限（状态里存的是历史值） */
        t->thread_count = effective_threads(g_cfg.thread_pool_size);

        g_task_count++;
        p = strchr(p, '}');
        if (p) p++;
    }

done:
    /* 按 task_id 升序排序后再交出去（放在 done 之后：解析中途放弃也要建立不变量）。
     * find_task() 是二分查找，依赖「g_tasks[] 按 task_id 升序」这条隐式约定；
     * 新建/摘除路径都保序，但状态文件是外部可编辑的输入 —— 顺序一旦不是升序，
     * 二分就会「找不到明明存在的任务」，表现为任务凭空消失、暂停/删除全部失效。
     * 与其要求文件永远有序，不如在入口处把不变量建立起来。 */
    for (int i = 1; i < g_task_count; i++) {
        DownloadTask tmp = g_tasks[i];
        int j = i - 1;
        while (j >= 0 && g_tasks[j].task_id > tmp.task_id) {
            g_tasks[j + 1] = g_tasks[j];
            j--;
        }
        g_tasks[j + 1] = tmp;
    }
    LeaveCriticalSection(&g_lock);
    free(buf);
    return 0;
}

/* ─────────────────────────────────────
 * GUI 需要的额外 API（main_window / task_detail_dialog 调用）
 * ───────────────────────────────────── */

/* 获取任务摘要列表（用于刷新 ListView） */
int dlmgr_get_task_summary(TaskSummary *out, int max_count) {
    if (!out || !g_initialized) return 0;
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    for (int i = 0; i < cnt; i++) {
        DownloadTask *t = &g_tasks[i];
        out[i].task_id     = t->task_id;
        strncpy(out[i].filename, t->filename, sizeof(out[i].filename)-1);
        strncpy(out[i].save_dir, t->save_dir, sizeof(out[i].save_dir)-1);
        strncpy(out[i].url, t->url, sizeof(out[i].url)-1);
        out[i].size       = t->file_size;
        out[i].status      = (int)t->status;
        out[i].speed       = t->speed_bps;
    }
    LeaveCriticalSection(&g_lock);
    return cnt;
}

/* 获取当前配置 */
DownloadConfig dlmgr_get_config(void) {
    return g_cfg;
}

/* 更新配置（运行时生效）*/
void dlmgr_set_config(const DownloadConfig *cfg) {
    if (!cfg || !g_initialized) return;
    EnterCriticalSection(&g_lock);
    g_cfg.thread_pool_size = cfg->thread_pool_size;
    g_cfg.max_concurrent  = cfg->max_concurrent;
    g_cfg.retry_max       = cfg->retry_max;
    g_cfg.speed_limit_global = cfg->speed_limit_global;
    g_cfg.proxy_type = cfg->proxy_type;
    strncpy(g_cfg.proxy_host, cfg->proxy_host, sizeof(g_cfg.proxy_host) - 1);
    g_cfg.proxy_port = cfg->proxy_port;
    strncpy(g_cfg.proxy_user, cfg->proxy_user, sizeof(g_cfg.proxy_user) - 1);
    strncpy(g_cfg.proxy_pass, cfg->proxy_pass, sizeof(g_cfg.proxy_pass) - 1);
    strncpy(g_cfg.user_agent, cfg->user_agent, sizeof(g_cfg.user_agent) - 1);
    g_cfg.user_agent[sizeof(g_cfg.user_agent) - 1] = '\0';
    g_cfg.connect_timeout_sec = cfg->connect_timeout_sec;
    /* 每服务器连接数上限：负数视为无效（当 0 处理 = 不封顶） */
    g_cfg.max_conn_per_server = cfg->max_conn_per_server > 0
                                    ? cfg->max_conn_per_server : 0;
    /* 站点登录凭据：整表下发，超上限的条目丢弃 */
    int sc = cfg->site_login_count;
    if (sc < 0) sc = 0;
    if (sc > MAX_SITE_LOGINS) sc = MAX_SITE_LOGINS;
    for (int i = 0; i < sc; i++) g_cfg.site_logins[i] = cfg->site_logins[i];
    g_cfg.site_login_count = sc;
    if (cfg->default_save_dir[0])
        strncpy(g_cfg.default_save_dir, cfg->default_save_dir,
                sizeof(g_cfg.default_save_dir) - 1);
    LeaveCriticalSection(&g_lock);
    /* 代理同步到全局请求选项 */
    dlmgr_set_proxy(g_cfg.proxy_type, g_cfg.proxy_host, g_cfg.proxy_port,
                    g_cfg.proxy_user, g_cfg.proxy_pass);
    /* dlmgr_set_proxy 会重置 g_net_opt（含 UA / 超时回默认），此处把用户在设置中
     * 配置的 UA 与连接超时重新套用，使「用户代理」「连接超时」两项设置真正生效。 */
    g_net_opt.user_agent = g_ua_buf;
    if (g_cfg.user_agent[0]) {
        strncpy(g_ua_buf, g_cfg.user_agent, sizeof(g_ua_buf) - 1);
        g_ua_buf[sizeof(g_ua_buf) - 1] = '\0';
    } else {
        strncpy(g_ua_buf, kDefaultUA, sizeof(g_ua_buf) - 1);
        g_ua_buf[sizeof(g_ua_buf) - 1] = '\0';
    }
    g_net_opt.connect_timeout_sec = (g_cfg.connect_timeout_sec > 0)
                                      ? g_cfg.connect_timeout_sec : 15;
}

/* 开始所有排队中的任务 */
int dlmgr_start_all(void) {
    if (!g_initialized) return -1;
    int started = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_PENDING || g_tasks[i].status == TASK_PAUSED) {
            dlmgr_start(g_tasks[i].task_id);
            started++;
        }
    }
    return started;
}

/* 停止所有运行中的任务 */
int dlmgr_stop_all(void) {
    if (!g_initialized) return -1;
    int stopped = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_RUNNING) {
            g_tasks[i].status = TASK_PAUSED;
            stopped++;
        }
    }
    return stopped;
}

/* 删除所有已完成的任务 */
int dlmgr_remove_completed(void) {
    if (!g_initialized) return -1;
    int removed = 0;
    EnterCriticalSection(&g_lock);
    for (int i = g_task_count - 1; i >= 0; i--) {
        if (g_tasks[i].status == TASK_COMPLETED || g_tasks[i].status == TASK_FAILED) {
            memmove(&g_tasks[i], &g_tasks[i+1],
                    (size_t)(g_task_count - i - 1) * sizeof(DownloadTask));
            g_task_count--;
            removed++;
        }
    }
    LeaveCriticalSection(&g_lock);
    return removed;
}

