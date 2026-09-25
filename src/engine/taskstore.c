#include "taskstore.h"
#include "torrent.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAXN 8192

/* 定稿一个刚解析出来的任务：补段数/连接数、把「正在下载」降级为暂停、清运行态、校验部分文件。 */
static void finalize_task(download_task_t *t, int nseg)
{
    t->seg_count = (nseg > 16) ? 16 : (nseg < 0 ? 0 : nseg);
    if (t->seg_count == 0 && t->total <= 0) {
        /* 未知长度：单段，从 downloaded 处下到结束 */
        t->seg_count = 1;
        t->seg_start[0] = 0;
        t->seg_len[0] = 0;
        t->seg_written[0] = t->downloaded;
    }
    if (t->num_conn < 1) t->num_conn = (t->seg_count > 0) ? t->seg_count : 1;
    if (t->num_conn > 16) t->num_conn = 16;
    if (t->status == DL_DOWNLOADING) t->status = DL_PAUSED;
    t->running = 0;
    t->threads_done = 0;
    t->threads_expected = 0;
    t->speed = 0.0;
    for (int s = 0; s < 16; s++) t->threads[s] = NULL;
}

/* 校验部分文件：不在或大小对不上就重置进度（从头下）。已完成的不动。 */
static void verify_partial(download_task_t *t)
{
    if (t->status == DL_COMPLETE) return;
    if (t->kind == IDM_KIND_TORRENT) return;   /* BT 由 aria2 管自己的文件 */

    HANDLE fh = CreateFileW(t->outfile, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    int reset = 0;
    if (fh == INVALID_HANDLE_VALUE) {
        reset = 1;
    } else {
        LARGE_INTEGER sz; GetFileSizeEx(fh, &sz);
        CloseHandle(fh);
        if (t->total <= 0 || sz.QuadPart != (LONGLONG)t->total) reset = 1;
    }
    if (reset) {
        t->downloaded = 0;
        for (int i = 0; i < 16; i++) t->seg_written[i] = 0;
        t->status = DL_QUEUED;
    }
}

int taskstore_save(const wchar_t *path, download_task_t *const *tasks, int n)
{
    if (!path || !tasks) return -1;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return -1;

    fputs("SWOOP|1\n", f);
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        download_task_t *t = tasks[i];
        if (!t) continue;
        char u8[MAX_PATH * 4];
        u8[0] = 0;
        WideCharToMultiByte(CP_UTF8, 0, t->outfile, -1, u8, sizeof u8, NULL, NULL);

        char ref8[4096];
        ref8[0] = 0;
        if (t->referer[0])
            WideCharToMultiByte(CP_UTF8, 0, t->referer, -1, ref8, sizeof ref8, NULL, NULL);

        fputs("T\n", f);
        fprintf(f, "url=%s\n", t->url);
        fprintf(f, "file=%s\n", u8);
        fprintf(f, "referer=%s\n", ref8);
        fprintf(f, "total=%lld\n", t->total);
        fprintf(f, "downloaded=%lld\n", (long long)t->downloaded);
        fprintf(f, "status=%d\n", (int)t->status);
        fprintf(f, "kind=%d\n", t->kind);
        fprintf(f, "upaused=%d\n", t->user_paused ? 1 : 0);
        fprintf(f, "conn=%d\n", t->num_conn);
        fprintf(f, "segs=%d\n", t->seg_count);
        for (int s = 0; s < t->seg_count && s < 16; s++)
            fprintf(f, "s=%lld,%lld,%lld\n",
                    t->seg_start[s], t->seg_len[s], (long long)t->seg_written[s]);
        cnt++;
    }
    fclose(f);
    return cnt;
}

int taskstore_load(const wchar_t *path, download_task_t **tasks, int maxn, int *outn)
{
    if (!path || !tasks || !outn) return -1;
    *outn = 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) return -1;

    char line[LINE_MAXN];
    int n = 0, seidx = 0;
    download_task_t *cur = NULL;

    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!line[0]) continue;

        if (strcmp(line, "T") == 0) {
            if (cur) { finalize_task(cur, seidx); verify_partial(cur); }
            if (n >= maxn) { cur = NULL; break; }
            cur = (download_task_t *)calloc(1, sizeof *cur);
            if (!cur) break;
            tasks[n++] = cur;
            seidx = 0;
            continue;
        }
        if (!cur) continue;   /* 表头行 */

        if      (strncmp(line, "url=", 4) == 0) {
            strncpy(cur->url, line + 4, sizeof cur->url - 1);
            cur->url[sizeof cur->url - 1] = 0;
        } else if (strncmp(line, "file=", 5) == 0) {
            MultiByteToWideChar(CP_UTF8, 0, line + 5, -1, cur->outfile, MAX_PATH);
        } else if (strncmp(line, "referer=", 8) == 0) {
            MultiByteToWideChar(CP_UTF8, 0, line + 8, -1, cur->referer, 2048);
        } else if (strncmp(line, "upaused=", 8) == 0) {
            cur->user_paused = atoi(line + 8) ? 1 : 0;
        } else if (strncmp(line, "total=", 6) == 0) {
            cur->total = strtoll(line + 6, NULL, 10);
        } else if (strncmp(line, "downloaded=", 11) == 0) {
            cur->downloaded = strtoll(line + 11, NULL, 10);
        } else if (strncmp(line, "status=", 7) == 0) {
            cur->status = (dl_status)atoi(line + 7);
        } else if (strncmp(line, "kind=", 5) == 0) {
            cur->kind = atoi(line + 5);
        } else if (strncmp(line, "conn=", 5) == 0) {
            cur->num_conn = atoi(line + 5);
        } else if (strncmp(line, "segs=", 5) == 0) {
            /* 段数由 s= 行实际决定 */
        } else if (strncmp(line, "s=", 2) == 0) {
            long long a = 0, b = 0, c = 0;
            if (sscanf(line + 2, "%lld,%lld,%lld", &a, &b, &c) == 3 && seidx < 16) {
                cur->seg_start[seidx] = a;
                cur->seg_len[seidx] = b;
                cur->seg_written[seidx] = c;
                seidx++;
            }
        }
    }
    fclose(f);
    if (cur) { finalize_task(cur, seidx); verify_partial(cur); }

    *outn = n;
    return 0;
}
