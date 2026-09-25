#include "download.h"
#include "http.h"
#include "speedlimit.h"
#include "torrent.h"
#include "common/util.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    download_task_t *task;
    int       seg;       /* 段号 */
    HANDLE    fh;
    long long start;     /* 本段本轮起始绝对偏移 */
    long long len;       /* 本段本轮待下字节数（0 = 下到结束） */
    long long written;   /* 本段本轮已写字节 */
} conn_arg_t;

/* 每收到一块就写入文件（文件指针已按偏移定位），累加总量；running=0 时中止。 */
static int task_write_cb(void *ctx, const void *data, long long n)
{
    conn_arg_t *a = (conn_arg_t *)ctx;
    if (!a->task->running) return 0;
    DWORD wr = 0;
    if (!WriteFile(a->fh, data, (DWORD)n, &wr, NULL)) return 0;
    a->written += wr;
    InterlockedAdd64(&a->task->downloaded, (LONGLONG)wr);
    dl_throttle(wr);   /* 全局限速（不限时立即返回） */
    return 1;
}

static DWORD WINAPI conn_thread(LPVOID p)
{
    conn_arg_t *a = (conn_arg_t *)p;
    download_task_t *t = a->task;
    a->fh = CreateFileW(t->outfile, FILE_WRITE_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (a->fh == INVALID_HANDLE_VALUE) {
        log_msg("task: open out file failed");
        InterlockedIncrement(&t->threads_done);
        free(a);
        return 0;
    }
    LARGE_INTEGER off; off.QuadPart = a->start;
    SetFilePointerEx(a->fh, off, NULL, FILE_BEGIN);

    long long got = http_download(t->url, t->referer, a->start, a->len,
                                  task_write_cb, NULL, a);
    if (got < 0) InterlockedIncrement(&t->io_errors);   /* 传输失败（含非 2xx）→ 整任务判错 */

    /* 先落盘本段进度，再报告线程结束 —— 保证 join 返回时 seg_written 一定准确 */
    t->seg_written[a->seg] = a->written;
    CloseHandle(a->fh);
    InterlockedIncrement(&t->threads_done);
    free(a);
    return 0;
}

download_task_t *task_create(const char *url, const wchar_t *outfile, int num_conn)
{
    download_task_t *t = (download_task_t *)calloc(1, sizeof *t);
    if (!t) return NULL;
    strncpy(t->url, url, sizeof t->url - 1);
    wcsncpy(t->outfile, outfile, MAX_PATH - 1);
    t->num_conn = num_conn;
    if (t->num_conn < 1) t->num_conn = 1;
    if (t->num_conn > 16) t->num_conn = 16;
    t->status = DL_QUEUED;
    return t;
}

void task_join(download_task_t *t)
{
    if (!t) return;
    HANDLE h[16]; int cnt = 0;
    for (int i = 0; i < 16; i++) {
        if (t->threads[i]) { h[cnt++] = t->threads[i]; t->threads[i] = NULL; }
    }
    if (cnt) {
        WaitForMultipleObjects(cnt, h, TRUE, INFINITE);
        for (int i = 0; i < cnt; i++) CloseHandle(h[i]);
    }
    t->threads_done = 0;
}

void task_stop(download_task_t *t)
{
    if (!t) return;
    t->running = 0;
}

void task_pause(download_task_t *t)
{
    if (!t) return;

    /* 排队中的任务还没起线程 —— 必须显式标成暂停。
       以前这里直接 return，于是「暂停」对排队任务完全无效：
       调度器下一个 250ms tick 又会把它拉起来（用户看到的是「点了暂停还在下」）。
       定时调度的「暂停全部」同样会漏掉排队任务。 */
    if (t->status == DL_QUEUED) {
        t->status = DL_PAUSED;
        t->speed = 0.0;
        return;
    }
    if (t->status != DL_DOWNLOADING) return;

    if (t->kind == IDM_KIND_TORRENT) {
        if (t->proc) {
            TerminateProcess((HANDLE)t->proc, 1);
            CloseHandle((HANDLE)t->proc);
            t->proc = NULL;
        }
        t->status = DL_PAUSED;
        t->speed = 0.0;
        log_msg("task_pause(torrent): %s", t->url);
        return;
    }
    t->running = 0;      /* 通知各分片线程中止 */
    task_join(t);        /* 等它们真正退出，避免恢复时新旧线程串写 */
    t->status = DL_PAUSED;
    t->speed = 0.0;
    log_msg("task_pause: %s got=%lld", t->url, t->downloaded);
}

void task_free(download_task_t *t)
{
    if (!t) return;
    t->running = 0;
    task_join(t);        /* 线程退出后才能释放它引用的 task 结构 */
    if (t->proc) {       /* BT：终止 aria2c 子进程 */
        TerminateProcess((HANDLE)t->proc, 1);
        CloseHandle((HANDLE)t->proc);
        t->proc = NULL;
    }
    free(t);
}

/* 按 num_conn 把 [0,total) 均分为 n 段；total<=0（未知长度）退化为单段。 */
static void split_segments(download_task_t *t, int supports_range)
{
    int n = (supports_range && t->total > 0) ? t->num_conn : 1;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    t->num_conn = n;
    t->seg_count = n;

    long long chunk = (t->total > 0) ? t->total / n : 0;
    for (int i = 0; i < n; i++) {
        t->seg_start[i] = (t->total > 0) ? (long long)i * chunk : 0;
        t->seg_len[i]   = (t->total > 0)
                          ? ((i == n - 1) ? (t->total - (long long)i * chunk) : chunk)
                          : 0;   /* 0 表示下到结束 */
        t->seg_written[i] = 0;
        t->threads[i] = NULL;
    }
}

int task_seg_plan(long long seg_start, long long seg_len, long long already,
                  long long *out_start, long long *out_len, long long *out_written0)
{
    if (already < 0) already = 0;
    if (seg_len > 0 && already >= seg_len) return 0;   /* 本段已补齐 */
    if (out_start)    *out_start    = seg_start + already;
    if (out_len)      *out_len      = (seg_len > 0) ? (seg_len - already) : 0;
    if (out_written0) *out_written0 = already;
    return 1;
}

int task_start(download_task_t *t)
{
    /* BT/磁力：交给同目录的 aria2c.exe 处理（outfile 作为下载目录） */
    if (t->kind == IDM_KIND_TORRENT) {
        wchar_t dir[MAX_PATH]; GetModuleFileNameW(NULL, dir, MAX_PATH);
        wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *(sl + 1) = 0;
        t->total = -1;
        t->downloaded = 0;
        t->running = 1;
        t->threads_done = 0;
        t->threads_expected = 0;
        t->io_errors = 0;
        t->status = DL_DOWNLOADING;
        t->proc = torrent_start(t->url, t->outfile, dir);
        if (!t->proc) { t->status = DL_ERROR; return -1; }
        log_msg("task_start(torrent): %s -> aria2c", t->url);
        return 0;
    }

    /* 已暂停/排队且有进度 → 续传；否则全新下载（DL_COMPLETE 重下也走全新） */
    int resuming = (t->status == DL_PAUSED || t->status == DL_QUEUED)
                   && t->downloaded > 0 && t->total > 0;

    /* http_probe 会返回 -2 表示服务端非 2xx（404/403/500…）。
       这种情况必须直接失败，绝不能把错误页当内容下下来。 */
    http_resource_t res;
    int pr = http_probe(t->url, &res);
    if (pr != 0) {
        t->status = DL_ERROR;
        log_msg("task_start: probe failed rc=%d http=%d %s", pr, res.status, t->url);
        return -1;
    }

    /* 续传前提：服务端仍支持 Range 且总大小未变；否则安全起见重下 */
    if (resuming && (!res.supports_range || res.content_length != t->total)) {
        log_msg("task_start: resume aborted (range=%d len=%lld old=%lld)",
                res.supports_range, res.content_length, t->total);
        resuming = 0;
        t->downloaded = 0;
    }
    t->total = res.content_length;

    /* 目标目录可能不存在（默认「按类型分类」会落到 Downloads\Videos 等子目录，
       而标准 Windows 上这些子目录并不存在）→ 必须先建，否则 CreateFileW
       报 ERROR_PATH_NOT_FOUND，用户看到的是「一新建就失败」。 */
    if (ensure_dir_for_file(t->outfile) != 0) {
        t->status = DL_ERROR;
        log_msg("task_start: cannot create dir for %ls", t->outfile);
        return -1;
    }

    HANDLE fh;
    if (resuming) {
        /* 复用已有文件；段划分沿用暂停前的，只补各段缺口 */
        fh = CreateFileW(t->outfile, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) { t->status = DL_ERROR; return -1; }
        CloseHandle(fh);
    } else {
        split_segments(t, res.supports_range);
        fh = CreateFileW(t->outfile, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) { t->status = DL_ERROR; return -1; }
        if (t->total > 0) {                 /* 预分配，保证各段可乱序写入 */
            LARGE_INTEGER sz; sz.QuadPart = t->total;
            SetFilePointerEx(fh, sz, NULL, FILE_BEGIN);
            SetEndOfFile(fh);
        }
        CloseHandle(fh);
        t->downloaded = 0;
    }

    t->running = 1;
    t->threads_done = 0;
    t->io_errors = 0;
    t->last_downloaded = t->downloaded;
    t->last_tick = GetTickCount();
    t->status = DL_DOWNLOADING;

    int expected = 0;
    for (int i = 0; i < t->seg_count; i++) {
        long long a_start = 0, a_len = 0, a_written0 = 0;
        if (!task_seg_plan(t->seg_start[i], t->seg_len[i],
                           (long long)t->seg_written[i],
                           &a_start, &a_len, &a_written0))
            continue;                        /* 本段已补齐，不用起线程 */
        conn_arg_t *a = (conn_arg_t *)malloc(sizeof *a);
        a->task = t;
        a->seg = i;
        a->start = a_start;
        a->len = a_len;
        a->written = a_written0;
        /* CreateThread 可能失败：失败的段不计入 expected，
           否则 threads_done 永远追不上 expected，任务会永久卡在「下载中」。 */
        HANDLE th = CreateThread(NULL, 0, conn_thread, a, 0, NULL);
        if (th) { t->threads[i] = th; expected++; }
        else {
            t->threads[i] = NULL;
            free(a);
            log_msg("task_start: CreateThread failed for seg %d", i);
        }
    }
    t->threads_expected = expected;

    if (expected == 0) { t->status = DL_COMPLETE; return 0; }
    log_msg("task_start: %s segs=%d expected=%d total=%lld resume=%d",
            t->url, t->seg_count, expected, t->total, resuming);
    return 0;
}

int dl_verdict(long long total, long long downloaded, int io_errors)
{
    if (io_errors > 0)    return 0;                    /* 有段传输出错（含非 2xx） */
    if (total > 0)        return downloaded >= total;  /* 已知大小：必须补齐 */
    if (total == 0)       return 1;                    /* 服务端明确说这是空文件 */
    return downloaded > 0;                             /* 长度未知：收到过数据才算成功 */
}

int task_tick(download_task_t *t, DWORD now_ms)
{
    if (t->status != DL_DOWNLOADING) return 0;

    /* BT：进度由 aria2c 自己管，这里只等子进程退出判完成 */
    if (t->kind == IDM_KIND_TORRENT) {
        if (!t->proc) { t->status = DL_ERROR; return 1; }
        if (WaitForSingleObject((HANDLE)t->proc, 0) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess((HANDLE)t->proc, &ec);
            CloseHandle((HANDLE)t->proc);
            t->proc = NULL;
            t->status = (ec == 0) ? DL_COMPLETE : DL_ERROR;
            log_msg("torrent_done: %s exit=%lu", t->url, ec);
            return 1;
        }
        return 0;   /* aria2 仍在运行 */
    }

    DWORD dt = now_ms - t->last_tick;
    if (dt == 0) dt = 1;
    t->speed = (double)(t->downloaded - t->last_downloaded) * 1000.0 / (double)dt;
    t->last_downloaded = t->downloaded;
    t->last_tick = now_ms;

    if (t->threads_done >= t->threads_expected) {
        int done = dl_verdict(t->total, t->downloaded, (int)t->io_errors);
        t->status = done ? DL_COMPLETE : DL_ERROR;
        log_msg("task_done: status=%d got=%lld total=%lld err=%ld",
                t->status, t->downloaded, t->total, (long)t->io_errors);
        return 1;
    }
    return 1; /* 速度变化也算变化，UI 重画 */
}
