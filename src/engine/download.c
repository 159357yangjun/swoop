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
    int       seg;
    HANDLE    fh;
    long long start;
    long long len;
    long long written;
} conn_arg_t;

static int task_write_cb(void *ctx, const void *data, long long n)
{
    conn_arg_t *a = (conn_arg_t *)ctx;
    if (!a->task->running) return 0;
    DWORD wr = 0;
    if (!WriteFile(a->fh, data, (DWORD)n, &wr, NULL)) return 0;
    a->written += wr;
    InterlockedAdd64(&a->task->downloaded, (LONGLONG)wr);
    dl_throttle(wr);
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
    if (got < 0) InterlockedIncrement(&t->io_errors);
    t->seg_written[a->seg] = a->written;
    CloseHandle(a->fh);
    InterlockedIncrement(&t->threads_done);
    free(a);
    return 0;
}

static int path_is_taken(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a != INVALID_FILE_ATTRIBUTES) return 1;
    DWORD e = GetLastError();
    return !(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND);
}

/* 新 HTTP 任务若目标已存在，自动保留原文件并选择 file (1).ext / (2)...。
   目录部分和扩展名保持不变；BT 的 outfile 是目录，不使用本函数。 */
static int choose_available_outfile(const wchar_t *desired, wchar_t *out, int n)
{
    if (!desired || !desired[0] || !out || n <= 1) return 0;
    wcsncpy(out, desired, n - 1);
    out[n - 1] = 0;
    if (!path_is_taken(out)) return 1;

    const wchar_t *end = desired + wcslen(desired);
    const wchar_t *slash1 = wcsrchr(desired, L'\\');
    const wchar_t *slash2 = wcsrchr(desired, L'/');
    const wchar_t *slash = slash1;
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    const wchar_t *base = slash ? slash + 1 : desired;
    const wchar_t *dot = wcsrchr(base, L'.');
    if (dot == base) dot = NULL;

    size_t prefix_len = (size_t)(base - desired);
    size_t stem_len = (size_t)((dot ? dot : end) - base);
    const wchar_t *ext = dot ? dot : L"";
    size_t ext_len = wcslen(ext);

    for (int i = 1; i <= 9999; i++) {
        wchar_t suffix[32];
        _snwprintf(suffix, 32, L" (%d)", i);
        suffix[31] = 0;
        size_t suffix_len = wcslen(suffix);
        if (prefix_len + suffix_len + ext_len + 2 > (size_t)n) return 0;
        size_t max_stem = (size_t)n - 1 - prefix_len - suffix_len - ext_len;
        size_t use_stem = stem_len < max_stem ? stem_len : max_stem;
        if (use_stem == 0) return 0;

        size_t w = 0;
        if (prefix_len) { wmemcpy(out + w, desired, prefix_len); w += prefix_len; }
        wmemcpy(out + w, base, use_stem); w += use_stem;
        wmemcpy(out + w, suffix, suffix_len); w += suffix_len;
        if (ext_len) { wmemcpy(out + w, ext, ext_len); w += ext_len; }
        out[w] = 0;
        if (!path_is_taken(out)) return 1;
    }
    return 0;
}

download_task_t *task_create(const char *url, const wchar_t *outfile, int num_conn)
{
    if (!url || !url[0] || !outfile || !outfile[0]) return NULL;
    download_task_t *t = (download_task_t *)calloc(1, sizeof *t);
    if (!t) return NULL;
    strncpy(t->url, url, sizeof t->url - 1);
    t->url[sizeof t->url - 1] = 0;
    t->kind = torrent_kind_for_url(url);
    if (t->kind == IDM_KIND_TORRENT) {
        wcsncpy(t->outfile, outfile, MAX_PATH - 1);
        t->outfile[MAX_PATH - 1] = 0;
    } else {
        if (!choose_available_outfile(outfile, t->outfile, MAX_PATH)) {
            free(t);
            return NULL;
        }
        if (_wcsicmp(t->outfile, outfile) != 0)
            log_msg("task_create: output exists, renamed to %ls", t->outfile);
    }
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
    t->running = 0;
    task_join(t);
    t->status = DL_PAUSED;
    t->speed = 0.0;
    log_msg("task_pause: %s got=%lld", t->url, t->downloaded);
}

void task_free(download_task_t *t)
{
    if (!t) return;
    t->running = 0;
    task_join(t);
    if (t->proc) {
        TerminateProcess((HANDLE)t->proc, 1);
        CloseHandle((HANDLE)t->proc);
        t->proc = NULL;
    }
    free(t);
}

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
        t->seg_len[i] = (t->total > 0)
                        ? ((i == n - 1) ? (t->total - (long long)i * chunk) : chunk)
                        : 0;
        t->seg_written[i] = 0;
        t->threads[i] = NULL;
    }
}

int task_seg_plan(long long seg_start, long long seg_len, long long already,
                  long long *out_start, long long *out_len, long long *out_written0)
{
    if (already < 0) already = 0;
    if (seg_len > 0 && already >= seg_len) return 0;
    if (out_start) *out_start = seg_start + already;
    if (out_len) *out_len = (seg_len > 0) ? (seg_len - already) : 0;
    if (out_written0) *out_written0 = already;
    return 1;
}

int task_start(download_task_t *t)
{
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

    int resuming = (t->status == DL_PAUSED || t->status == DL_QUEUED)
                   && t->downloaded > 0 && t->total > 0;
    http_resource_t res;
    int pr = http_probe(t->url, &res);
    if (pr != 0) {
        t->status = DL_ERROR;
        log_msg("task_start: probe failed rc=%d http=%d %s", pr, res.status, t->url);
        return -1;
    }
    if (resuming && (!res.supports_range || res.content_length != t->total)) {
        log_msg("task_start: resume aborted (range=%d len=%lld old=%lld)",
                res.supports_range, res.content_length, t->total);
        resuming = 0;
        t->downloaded = 0;
    }
    t->total = res.content_length;
    if (ensure_dir_for_file(t->outfile) != 0) {
        t->status = DL_ERROR;
        log_msg("task_start: cannot create dir for %ls", t->outfile);
        return -1;
    }

    HANDLE fh;
    if (resuming) {
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
        if (t->total > 0) {
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
            continue;
        conn_arg_t *a = (conn_arg_t *)malloc(sizeof *a);
        a->task = t;
        a->seg = i;
        a->start = a_start;
        a->len = a_len;
        a->written = a_written0;
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
    if (io_errors > 0) return 0;
    if (total > 0) return downloaded >= total;
    if (total == 0) return 1;
    return downloaded > 0;
}

int task_tick(download_task_t *t, DWORD now_ms)
{
    if (t->status != DL_DOWNLOADING) return 0;
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
        return 0;
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
    return 1;
}
