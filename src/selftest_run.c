#define WIN32_LEAN_AND_MEAN
#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0501
#endif
#include <windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/download.h"
#include "engine/http.h"
#include "engine/taskstore.h"
#include "engine/sched.h"
#include "engine/speedlimit.h"
#include "engine/queue.h"
#include "engine/category.h"
#include "engine/torrent.h"
#include "common/util.h"
#include "gui/resources.h"
#include "selftest_run.h"

/* 本地自测服务：内容为 (offset*31+7)&0xFF 的确定性模式，
   支持 Range；g_slow 打开时限速，用于制造「下载到一半」的中间态。 */
#define TEST_SIZE (1048576LL)   /* 1 MB */
#define TEST_PORT 18099

static volatile int g_slow = 0;

static DWORD WINAPI srv_thread(LPVOID p)
{
    int port = *(int *)p;
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof opt);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(s, (struct sockaddr *)&a, sizeof a);
    listen(s, 8);
    while (1) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        char req[2048]; int got = 0;
        while (got < (int)sizeof req - 1) {
            int r = recv(c, req + got, 1, 0);
            if (r <= 0) break;
            got += r;
            if (got >= 4 && memcmp(req + got - 4, "\r\n\r\n", 4) == 0) break;
        }
        req[got] = 0;

        /* 让自测能拿到非 2xx 响应：路径含 /404 或 /missing 一律回 404。
           这是 P0「错误页被当成下载完成」的验证手段。 */
        if (strstr(req, "/404") || strstr(req, "/missing")) {
            const char *body = "not found";
            char h2[160];
            int l2 = snprintf(h2, sizeof h2,
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", (int)strlen(body));
            send(c, h2, l2, 0);
            send(c, body, (int)strlen(body), 0);
            closesocket(c);
            continue;
        }

        long long start = 0, end = TEST_SIZE - 1;
        char *rg = strstr(req, "Range: bytes=");
        if (rg) {
            long long a0, a1; int n = sscanf(rg, "Range: bytes=%lld-%lld", &a0, &a1);
            if (n >= 1) {
                start = a0;
                end = a1;
                if (!(n == 2 && a1 >= a0)) end = TEST_SIZE - 1;
            }
        }
        if (end >= TEST_SIZE) end = TEST_SIZE - 1;
        long long len = end - start + 1;
        if (len < 0) len = 0;

        char hdr[256];
        int hl = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Range: bytes %lld-%lld/%lld\r\n"
            "Content-Length: %lld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Connection: close\r\n\r\n",
            start, end, (long long)TEST_SIZE, len);
        send(c, hdr, hl, 0);

        long long sent = 0;
        char chunk[65536];
        while (sent < len) {
            long long left = len - sent;
            int maxc = g_slow ? 8192 : (int)sizeof chunk;
            int cnt = (int)(left < (long long)maxc ? left : (long long)maxc);
            for (int i = 0; i < cnt; i++) {
                unsigned char b = (unsigned char)(((start + sent + i) * 31 + 7) & 0xFF);
                chunk[i] = (char)b;
            }
            int r = send(c, chunk, cnt, 0);
            if (r <= 0) break;
            sent += r;
            if (g_slow) Sleep(20);   /* 限速：让下载停在中间 */
        }
        closesocket(c);
    }
    closesocket(s);
    WSACleanup();
    return 0;
}

static void make_tmp(wchar_t *out, const wchar_t *name)
{
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    wcscpy(out, tmp); wcscat(out, name);
}

/* 校验文件字节 == 服务端确定性模式。返回 0 表示完全一致。 */
static int verify_file(const wchar_t *path)
{
    HANDLE fh = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (fh == INVALID_HANDLE_VALUE) return 4;
    LARGE_INTEGER sz; GetFileSizeEx(fh, &sz);
    if (sz.QuadPart != (LONGLONG)TEST_SIZE) { CloseHandle(fh); return 3; }
    unsigned char *buf = (unsigned char *)malloc((size_t)TEST_SIZE);
    if (!buf) { CloseHandle(fh); return 5; }
    DWORD rd = 0;
    ReadFile(fh, buf, (DWORD)TEST_SIZE, &rd, NULL);
    CloseHandle(fh);
    int ok = 1;
    for (long long k = 0; k < (long long)TEST_SIZE; k++) {
        if (buf[k] != (unsigned char)((k * 31 + 7) & 0xFF)) { ok = 0; break; }
    }
    free(buf);
    return ok ? 0 : 2;
}

/* 用例①：整段多线程分片下载 */
static int test_whole(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/bigfile", TEST_PORT);
    wchar_t out[MAX_PATH]; make_tmp(out, L"idm_selftest.bin");
    DeleteFileW(out);
    g_slow = 0;

    int rc;
    download_task_t *t = task_create(curl, out, 8);
    if (t && task_start(t) == 0) {
        /* 自测无消息循环，须自己驱动 task_tick（相当于 GUI 的 250ms 定时器） */
        for (int i = 0; i < 2000 && t->status == DL_DOWNLOADING; i++) {
            task_tick(t, GetTickCount());
            Sleep(10);
        }
        rc = verify_file(out);
        log_msg("test_whole: status=%d got=%lld rc=%d", t->status, t->downloaded, rc);
        task_free(t);
    } else rc = 1;
    DeleteFileW(out);
    return rc;
}

/* 用例②：下载到中途暂停 → 续传 → 校验仍需逐字节一致 */
static int test_resume(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/bigfile", TEST_PORT);
    wchar_t out[MAX_PATH]; make_tmp(out, L"idm_resume.bin");
    DeleteFileW(out);

    download_task_t *t = task_create(curl, out, 8);
    if (!t) return 1;

    g_slow = 1;                       /* 限速，保证能停在中间 */
    if (task_start(t) != 0) { g_slow = 0; task_free(t); return 1; }

    long long lo = TEST_SIZE / 8, hi = TEST_SIZE * 3 / 4;
    int caught = 0;
    for (int i = 0; i < 2000; i++) {
        task_tick(t, GetTickCount());
        long long d = t->downloaded;
        if (d >= lo && d < hi) { task_pause(t); caught = 1; break; }
        if (t->status == DL_COMPLETE || t->status == DL_ERROR) break;
        Sleep(10);
    }
    long long partial = t->downloaded;
    log_msg("test_resume: caught=%d partial=%lld/%lld status=%d",
            caught, partial, t->total, t->status);

    int rc;
    if (!caught || partial <= 0 || partial >= TEST_SIZE) { rc = 10; goto out; }
    if (t->status != DL_PAUSED) { rc = 11; goto out; }

    g_slow = 0;                       /* 续传走全速，尽快下完 */
    if (task_start(t) != 0) { rc = 12; goto out; }
    /* 必须从断点继续；若 downloaded 被打回 0，说明其实重下了（不是续传） */
    if (t->downloaded + 1 < partial) { rc = 17; goto out; }
    for (int i = 0; i < 3000 && t->status == DL_DOWNLOADING; i++) {
        task_tick(t, GetTickCount());
        Sleep(10);
    }
    if (t->status != DL_COMPLETE) { rc = 13; goto out; }

    rc = verify_file(out);
    log_msg("test_resume: resumed got=%lld status=%d verify_rc=%d",
            t->downloaded, t->status, rc);

out:
    DeleteFileW(out);
    task_free(t);
    g_slow = 0;
    return rc;
}

/* 创建一个指定大小的文件（供持久化用例校验部分文件）。 */
static void create_sized(const wchar_t *p, long long sz)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (sz > 0) {
        LARGE_INTEGER l; l.QuadPart = sz;
        SetFilePointerEx(h, l, NULL, FILE_BEGIN);
        SetEndOfFile(h);
    }
    CloseHandle(h);
}

/* 用例③：任务列表存盘/读回（含逐段进度） */
static int test_taskstore(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/a", TEST_PORT);
    wchar_t f1[MAX_PATH], f2[MAX_PATH], path[MAX_PATH];
    make_tmp(f1, L"idm_ts1.bin");
    make_tmp(f2, L"idm_ts2.bin");
    make_tmp(path, L"idm_ts_store.dat");
    create_sized(f1, 1048576);      /* 与 total 一致，verify_partial 才不会重置 */
    create_sized(f2, 500);

    download_task_t *t1 = task_create(curl, f1, 8);
    t1->total = 1048576; t1->downloaded = 300000; t1->status = DL_PAUSED;
    t1->seg_count = 8;
    for (int i = 0; i < 8; i++) {
        t1->seg_start[i] = (long long)i * 131072;
        t1->seg_len[i] = 131072;
        t1->seg_written[i] = (i < 3) ? 131072 : 1000;
    }
    download_task_t *t2 = task_create(curl, f2, 4);
    t2->total = 500; t2->downloaded = 500; t2->status = DL_COMPLETE;
    t2->seg_count = 1; t2->seg_start[0] = 0; t2->seg_len[0] = 500; t2->seg_written[0] = 500;

    download_task_t *arr[2] = { t1, t2 };
    int rc = 0;
    if (taskstore_save(path, arr, 2) != 2) rc = 1;
    else {
        download_task_t *out[8]; int n = 0;
        if (taskstore_load(path, out, 8, &n) != 0 || n != 2) rc = 2;
        else if (strcmp(out[0]->url, t1->url) != 0) rc = 3;
        else if (wcscmp(out[0]->outfile, t1->outfile) != 0) rc = 4;
        else if (out[0]->total != t1->total || out[0]->downloaded != t1->downloaded) rc = 5;
        else if (out[0]->seg_count != 8) rc = 6;
        else if (out[0]->seg_written[2] != 131072 || out[0]->seg_written[3] != 1000) rc = 7;
        else if (out[0]->status != DL_PAUSED) rc = 9;
        else if (out[1]->seg_count != 1 || out[1]->seg_written[0] != 500
                 || out[1]->status != DL_COMPLETE) rc = 8;
        for (int i = 0; i < n; i++) free(out[i]);
    }
    task_free(t1); task_free(t2);
    DeleteFileW(path); DeleteFileW(f1); DeleteFileW(f2);
    log_msg("test_taskstore rc=%d", rc);
    return rc;
}

/* 用例④：调度判定（纯函数） */
static int test_sched(void)
{
    struct { int en, s, st, c; sched_action want; } t[] = {
        { 1, 100, 800, 100, SCHED_START },
        { 1, 100, 800, 800, SCHED_STOP },
        { 1, 100, 800, 500, SCHED_NONE },
        { 0, 100, 800, 100, SCHED_NONE },   /* 未使能 */
        { 1, 500, 500, 500, SCHED_NONE },   /* 起止相同 */
        { 1, 2600, 800, 2600, SCHED_NONE }, /* 非法时刻 */
        { 1, 100, 800, -1, SCHED_NONE },    /* 非法当前 */
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        sched_action got = sched_decide(t[i].en, t[i].s, t[i].st, t[i].c);
        if (got != t[i].want) { rc = i + 1; break; }
    }
    log_msg("test_sched rc=%d", rc);
    return rc;
}

/* 用例⑤：全局限速（虚拟时钟）。3 倍量 @ limit → 应耗时 ≈ 3 秒。 */
static int test_speedlimit(void)
{
    const long long lim = 200000;
    dl_set_speed_limit(lim);
    DWORD t0 = GetTickCount();
    dl_throttle(lim * 3);
    DWORD dt = GetTickCount() - t0;
    dl_set_speed_limit(0);
    if (dt < 2500 || dt > 4500) { log_msg("test_speedlimit limited dt=%lu", dt); return 1; }

    t0 = GetTickCount();
    dl_throttle(10 * 1024 * 1024);      /* 不限速应几乎立即 */
    dt = GetTickCount() - t0;
    if (dt > 200) { log_msg("test_speedlimit unlimited dt=%lu", dt); return 2; }

    log_msg("test_speedlimit ok");
    return 0;
}

/* 用例⑥：限速真的作用于下载 —— 1MB @ 512KB/s ≈ 2 秒，且数据仍逐字节正确。
   （若 write_cb 没调 dl_throttle，这里会秒完 → 变红） */
static int test_speedlimit_download(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/bigfile", TEST_PORT);
    wchar_t out[MAX_PATH]; make_tmp(out, L"idm_sl.bin");
    DeleteFileW(out);
    g_slow = 0;
    dl_set_speed_limit(524288);        /* 512 KB/s */
    int rc = 0;
    download_task_t *t = task_create(curl, out, 8);
    DWORD t0 = GetTickCount();
    if (t && task_start(t) == 0) {
        for (int i = 0; i < 3000 && t->status == DL_DOWNLOADING; i++) {
            task_tick(t, GetTickCount());
            Sleep(10);
        }
        DWORD dt = GetTickCount() - t0;
        if (t->status != DL_COMPLETE) rc = 1;
        else if (verify_file(out) != 0) rc = 2;
        else if (dt < 1500 || dt > 6000) rc = 3;
        log_msg("test_speedlimit_download dt=%lu status=%d rc=%d", dt, t->status, rc);
        task_free(t);
    } else rc = 4;
    dl_set_speed_limit(0);
    DeleteFileW(out);
    return rc;
}

/* 用例⑦：队列并发槽位 + 挑选（纯函数） */
static int test_queue(void)
{
    struct { int running, max, want; } t[] = {
        { 0, 3, 3 }, { 2, 3, 1 }, { 3, 3, 0 }, { 5, 3, 0 },
        { -1, 3, 3 },   /* 负数运行数按 0 处理 */
        { 9, 0, 0x7fffffff }, { 0, 0, 0x7fffffff },   /* 0 表示不限并发 */
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        int got = queue_slots(t[i].running, t[i].max);
        if (got != t[i].want) { rc = i + 1; break; }
    }
    if (rc) { log_msg("test_queue slots rc=%d", rc); return rc; }

    /* queue_pick：只挑「排队中且用户没暂停」的。
       必须跳过 user_paused，否则用户点过暂停的任务会被调度器反复拉起来。 */
    download_task_t a, b, c, d, e;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    memset(&d, 0, sizeof d); memset(&e, 0, sizeof e);
    a.status = DL_QUEUED;
    b.status = DL_QUEUED;  b.user_paused = 1;   /* 用户暂停 → 不许拉起 */
    c.status = DL_DOWNLOADING;                  /* 已在跑 → 不重复启动 */
    d.status = DL_QUEUED;
    e.status = DL_COMPLETE;                     /* 已完成 → 不重下 */
    download_task_t *arr[5] = { &a, &b, &c, &d, &e };
    int idx[8];

    int n = queue_pick(arr, 5, 5, idx, 8);
    if (n != 2 || idx[0] != 0 || idx[1] != 3) rc = 20;
    else if (queue_pick(arr, 5, 1, idx, 8) != 1 || idx[0] != 0) rc = 21;
    else if (queue_pick(arr, 5, 0, idx, 8) != 0) rc = 22;      /* 没空位 → 一个都不挑 */
    else if (queue_pick(arr, 5, 5, idx, 0) != 0) rc = 23;      /* maxout 保护 */
    else if (queue_pick(NULL, 5, 5, idx, 8) != 0) rc = 24;
    log_msg("test_queue rc=%d", rc);
    return rc;
}

/* 用例⑧：分类目录（后缀表 / 路径组装 / %XX 解码） */
static int test_category(void)
{
    struct { const wchar_t *name; dl_category want; } c[] = {
        { L"a.mp4", CAT_VIDEO }, { L"A.MP4", CAT_VIDEO }, { L"b.mkv", CAT_VIDEO },
        { L"c.mp3", CAT_MUSIC }, { L"d.flac", CAT_MUSIC },
        { L"e.zip", CAT_ARCHIVE }, { L"f.7z", CAT_ARCHIVE },
        { L"g.pdf", CAT_DOC }, { L"h.docx", CAT_DOC },
        { L"i.exe", CAT_PROGRAM }, { L"j.msi", CAT_PROGRAM },
        { L"k.xyz", CAT_OTHER }, { L"noext", CAT_OTHER }, { L"", CAT_OTHER },
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof c / sizeof c[0]); i++) {
        if (category_for_filename(c[i].name) != c[i].want) { rc = i + 1; break; }
    }
    if (rc) { log_msg("test_category ext rc=%d", rc); return rc; }

    wchar_t out[MAX_PATH];
    /* 分类开启：base\Videos\video.mp4 */
    category_build_path(L"C:\\DL", L"http://host/path/video.mp4", 1, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\Videos\\video.mp4") != 0) { rc = 20; goto done; }
    /* 分类关闭：base\video.mp4 */
    category_build_path(L"C:\\DL", L"http://host/path/video.mp4", 0, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\video.mp4") != 0) { rc = 21; goto done; }
    /* query 去掉 + %XX 解码 */
    category_build_path(L"C:\\DL", L"http://host/a%20b.mp4?token=x#frag", 1, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\Videos\\a b.mp4") != 0) { rc = 22; goto done; }
    /* 后缀大小写 + 未知后类落 Other */
    category_build_path(L"C:\\DL", L"http://host/Img.PNG", 1, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\Other\\Img.PNG") != 0) { rc = 23; goto done; }
    /* 无 '/' 的裸名 */
    category_build_path(L"C:\\DL", L"movie.mkv", 1, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\Videos\\movie.mkv") != 0) { rc = 24; goto done; }

    /* URL 里的 UTF-8 %XX 必须**整段**解回宽字符。
       以前一个 %XX 塞进一个 wchar_t，%E4%B8%AD 变成 3 个乱码字符 → 中文文件名全乱。 */
    category_filename_from_url(L"http://host/%E4%B8%AD%E6%96%87.mp4", out, MAX_PATH);
    if (wcscmp(out, L"\u4E2D\u6587.mp4") != 0) { rc = 25; goto done; }
    /* 中文名要能正确归类（后缀表依赖解出来的宽字符） */
    category_build_path(L"C:\\DL", L"http://host/%E4%B8%AD%E6%96%87.mp4", 1, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\Videos\\\u4E2D\u6587.mp4") != 0) { rc = 26; goto done; }
    /* %20 空格 + 空名兜底 */
    category_filename_from_url(L"http://host/a%20b.mp4", out, MAX_PATH);
    if (wcscmp(out, L"a b.mp4") != 0) { rc = 27; goto done; }
    category_filename_from_url(L"http://host/", out, MAX_PATH);
    if (wcscmp(out, L"download") != 0) { rc = 28; goto done; }

done:
    log_msg("test_category rc=%d out=%ls", rc, out);
    return rc;
}

/* 用例⑨：BT/磁力判别（纯函数） */
static int test_torrent_kind(void)
{
    struct { const char *url; int want; } t[] = {
        { "magnet:?xt=urn:btih:ABC", IDM_KIND_TORRENT },
        { "MAGNET:?xt=urn:btih:ABC", IDM_KIND_TORRENT },   /* 大小写不敏感 */
        { "http://host/a.torrent", IDM_KIND_TORRENT },
        { "http://host/a.TORRENT", IDM_KIND_TORRENT },
        { "http://host/a.torrent?x=1", IDM_KIND_TORRENT }, /* query 不影响 */
        { "http://host/a.torrent#frag", IDM_KIND_TORRENT },
        { "http://host/a.mp4", IDM_KIND_HTTP },
        { "https://host/bigfile", IDM_KIND_HTTP },
        { "http://host/torrent", IDM_KIND_HTTP },          /* 前缀像但不以 .torrent 结尾 */
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        if (torrent_kind_for_url(t[i].url) != t[i].want) { rc = i + 1; break; }
    }
    if (torrent_kind_for_url(NULL) != IDM_KIND_HTTP) rc = 90;
    log_msg("test_torrent_kind rc=%d", rc);
    return rc;
}

/* 用例⑩：完成判定（纯函数）。
   重点防「404 错误页被当成下载完成」以及「传输出错仍报成功」。 */
static int test_verdict(void)
{
    struct { long long total, got; int err, want; } t[] = {
        { 1000, 1000, 0, 1 },   /* 补齐 → 完成 */
        { 1000,  999, 0, 0 },   /* 差一字节 → 不算完成 */
        { 1000, 1000, 1, 0 },   /* 有段传输失败 → 即使字节数够也判失败 */
        { 0,       0, 0, 1 },   /* 服务端明确返回空文件 → 完成 */
        { -1,    100, 0, 1 },   /* 长度未知但收到数据 → 完成 */
        { -1,      0, 0, 0 },   /* 长度未知且一字节没收到（典型 404 错误页）→ 失败 */
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        int got = dl_verdict(t[i].total, t[i].got, t[i].err);
        if (got != t[i].want) { rc = i + 1; break; }
    }
    log_msg("test_verdict rc=%d", rc);
    return rc;
}

/* 用例⑪：目标父目录不存在时仍要能下成。
   默认「按类型分类」会写到 Downloads\Videos\... 而该子目录并不存在，
   以前 CreateFileW 直接 ERROR_PATH_NOT_FOUND → 新建任务即失败。 */
static int test_dir_missing(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/bigfile", TEST_PORT);
    wchar_t tmp[MAX_PATH], root[MAX_PATH], sub1[MAX_PATH], sub2[MAX_PATH], out[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(root, MAX_PATH, L"%sidm_dirprobe_%lu", tmp, (unsigned long)GetTickCount());
    _snwprintf(sub1, MAX_PATH, L"%s\\Videos", root);
    _snwprintf(sub2, MAX_PATH, L"%s\\sub", sub1);
    _snwprintf(out,  MAX_PATH, L"%s\\deep.bin", sub2);
    DeleteFileW(out);
    RemoveDirectoryW(sub2); RemoveDirectoryW(sub1); RemoveDirectoryW(root);

    g_slow = 0;
    int rc = 0;
    download_task_t *t = task_create(curl, out, 4);
    if (!t) return 1;
    if (task_start(t) != 0) rc = 2;              /* 建目录失败 → 这里就会红 */
    else {
        for (int i = 0; i < 2000 && t->status == DL_DOWNLOADING; i++) {
            task_tick(t, GetTickCount());
            Sleep(10);
        }
        if (t->status != DL_COMPLETE) rc = 3;
        else if (verify_file(out) != 0) rc = 4;
    }
    log_msg("test_dir_missing rc=%d status=%d path=%ls", rc, t->status, out);
    task_free(t);

    DeleteFileW(out);
    RemoveDirectoryW(sub2); RemoveDirectoryW(sub1); RemoveDirectoryW(root);
    return rc;
}

/* 用例⑫：服务端 404 必须判失败，且不能把错误页写成文件。
   以前 http_probe 不看状态码，content_length=-1 → 任务显示「完成」。 */
static int test_http_404(void)
{
    char curl[256]; snprintf(curl, sizeof curl, "http://127.0.0.1:%d/404", TEST_PORT);
    wchar_t out[MAX_PATH]; make_tmp(out, L"idm_404.bin");
    DeleteFileW(out);
    g_slow = 0;

    int rc = 0;
    download_task_t *t = task_create(curl, out, 4);
    if (!t) return 1;
    if (task_start(t) == 0) rc = 2;                                  /* 404 竟然启动成功 */
    else if (t->status != DL_ERROR) rc = 3;
    else if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) rc = 4; /* 不该留下文件 */
    log_msg("test_http_404 rc=%d status=%d", rc, t->status);
    task_free(t);
    DeleteFileW(out);
    return rc;
}

/* 用例⑬：分段下发计划（纯函数）。
   重点：续传时线程的计数起点必须接上「暂停前已写字节」（out_written0 = already），
   否则线程退出时会把 seg_written 覆盖成只剩本轮的字节数 → 再续传时起点偏小、
   downloaded 被重复累加，进度条出现 >100%。 */
static int test_seg_plan(void)
{
    struct { long long st, len, already; int want; long long ws, wl, ww; } t[] = {
        { 0, 1000,    0, 1,   0, 1000,   0 },   /* 全新单段 */
        { 0, 1000,  400, 1, 400,  600, 400 },   /* 续传一半：计数起点必须 = 400 */
        { 0, 1000, 1000, 0,   0,    0,   0 },   /* 已补齐 → 不起线程 */
        { 500, 500,  300, 1, 800,  200, 300 },  /* 多段中的尾段续传 */
        { 0,    0,  500, 1, 500,    0, 500 },   /* 长度未知：从已写处下到结束 */
        { 0,  100,   -5, 1,   0,  100,   0 },   /* 负数已写按 0 */
    };
    int rc = 0;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        long long s = -1, l = -1, w = -1;
        int got = task_seg_plan(t[i].st, t[i].len, t[i].already, &s, &l, &w);
        if (got != t[i].want) { rc = i + 1; break; }
        if (got == 1 && (s != t[i].ws || l != t[i].wl || w != t[i].ww)) { rc = 100 + i; break; }
    }
    /* NULL 出参不应崩 */
    task_seg_plan(0, 100, 0, NULL, NULL, NULL);
    log_msg("test_seg_plan rc=%d", rc);
    return rc;
}

/* 用例⑭：排队中的任务点「暂停」必须真的停下。
   以前 task_pause 对非 DL_DOWNLOADING 直接 return，于是排队任务状态不变，
   调度器下一个 250ms tick 又把它拉起来 —— 用户看到「点了暂停还在下」。
   定时调度的「暂停全部」同样漏掉排队任务。 */
static int test_pause_queued(void)
{
    download_task_t *t = task_create("http://127.0.0.1:1/never", L"never.bin", 4);
    if (!t) return 1;
    int rc = 0;
    if (t->status != DL_QUEUED) rc = 2;          /* task_create 出厂即排队 */
    else {
        task_pause(t);
        if (t->status != DL_PAUSED) rc = 3;      /* 必须变成已暂停 */
    }
    /* 暂停后再调一次不应改变状态（幂等） */
    if (!rc) { task_pause(t); if (t->status != DL_PAUSED) rc = 4; }
    log_msg("test_pause_queued rc=%d status=%d", rc, (int)t->status);
    task_free(t);
    return rc;
}

/* 用例⑮：资源表真的可用。
   菜单上写着「新建任务 Ctrl+N」，就必须能 LoadAccelerators 拿到那张快捷键表；
   两个对话框模板、两个菜单、图标也必须在。
   （写错资源 ID 或 .rc 语法问题，windres 不报错，只在运行时静默失效。） */
static int test_resources(void)
{
    HINSTANCE h = GetModuleHandleW(NULL);
    int rc = 0;
    if (!LoadAcceleratorsW(h, MAKEINTRESOURCEW(IDR_ACCEL)))       rc = 1;
    else if (!LoadMenuW(h, MAKEINTRESOURCEW(IDR_MAIN)))           rc = 2;
    else if (!LoadMenuW(h, MAKEINTRESOURCEW(IDR_TRAY)))           rc = 3;
    else if (!FindResourceW(h, MAKEINTRESOURCEW(IDD_NEW_TASK), RT_DIALOG)) rc = 4;
    else if (!FindResourceW(h, MAKEINTRESOURCEW(IDD_SETTINGS), RT_DIALOG)) rc = 5;
    else if (!LoadIconW(h, MAKEINTRESOURCEW(IDI_APP)))            rc = 6;
    log_msg("test_resources rc=%d", rc);
    return rc;
}

int run_selftest(void)
{
    int port = TEST_PORT;
    g_slow = 0;
    DWORD tid; HANDLE h = CreateThread(NULL, 0, srv_thread, &port, 0, &tid);
    Sleep(300);

    if (http_init(WINHTTP_ACCESS_TYPE_NO_PROXY) != 0) {
        printf("selftest: fail load winhttp\n");
        return 1;
    }

    int rc1 = test_whole();
    int rc2 = test_resume();
    int rc3 = test_taskstore();
    int rc4 = test_sched();
    int rc5 = test_speedlimit();
    int rc6 = test_speedlimit_download();
    int rc7 = test_queue();
    int rc8 = test_category();
    int rc9 = test_torrent_kind();
    int rc10 = test_verdict();
    int rc11 = test_dir_missing();
    int rc12 = test_http_404();
    int rc13 = test_seg_plan();
    int rc14 = test_pause_queued();
    int rc15 = test_resources();
    int rc = 0;
    if (rc1) rc = rc1; else if (rc2) rc = rc2; else if (rc3) rc = rc3;
    else if (rc4) rc = rc4; else if (rc5) rc = rc5; else if (rc6) rc = rc6;
    else if (rc7) rc = rc7; else if (rc8) rc = rc8; else if (rc9) rc = rc9;
    else if (rc10) rc = rc10; else if (rc11) rc = rc11; else if (rc12) rc = rc12;
    else if (rc13) rc = rc13; else if (rc14) rc = rc14; else if (rc15) rc = rc15;

    http_cleanup();
    WaitForSingleObject(h, 1000);
    CloseHandle(h);

    char rf[MAX_PATH]; GetModuleFileNameA(NULL, rf, MAX_PATH);
    char *sl = strrchr(rf, '\\'); if (sl) *(sl + 1) = 0;
    strcat(rf, "idm_selftest_result.txt");
    FILE *f = fopen(rf, "w");
    if (f) {
        fprintf(f, "selftest %s rc=%d whole=%d resume=%d store=%d sched=%d speed=%d "
                   "speeddl=%d queue=%d cat=%d torrent=%d verdict=%d dirmiss=%d http404=%d "
                   "segplan=%d paused=%d res=%d\n",
                rc == 0 ? "PASS" : "FAIL", rc, rc1, rc2, rc3, rc4, rc5, rc6,
                rc7, rc8, rc9, rc10, rc11, rc12, rc13, rc14, rc15);
        fclose(f);
    }

    printf("IDM selftest: %s (whole=%d resume=%d store=%d sched=%d speed=%d speeddl=%d "
           "queue=%d cat=%d torrent=%d verdict=%d dirmiss=%d http404=%d segplan=%d paused=%d res=%d rc=%d)\n",
           rc == 0 ? "PASS" : "FAIL", rc1, rc2, rc3, rc4, rc5, rc6,
           rc7, rc8, rc9, rc10, rc11, rc12, rc13, rc14, rc15, rc);
    return rc;
}
