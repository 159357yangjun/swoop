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

/* 用例⑦：队列并发槽位（纯函数） */
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
    int rc = 0;
    if (rc1) rc = rc1; else if (rc2) rc = rc2; else if (rc3) rc = rc3;
    else if (rc4) rc = rc4; else if (rc5) rc = rc5; else if (rc6) rc = rc6;
    else if (rc7) rc = rc7; else if (rc8) rc = rc8; else if (rc9) rc = rc9;

    http_cleanup();
    WaitForSingleObject(h, 1000);
    CloseHandle(h);

    char rf[MAX_PATH]; GetModuleFileNameA(NULL, rf, MAX_PATH);
    char *sl = strrchr(rf, '\\'); if (sl) *(sl + 1) = 0;
    strcat(rf, "idm_selftest_result.txt");
    FILE *f = fopen(rf, "w");
    if (f) {
        fprintf(f, "selftest %s rc=%d whole=%d resume=%d store=%d sched=%d speed=%d speeddl=%d queue=%d cat=%d torrent=%d\n",
                rc == 0 ? "PASS" : "FAIL", rc, rc1, rc2, rc3, rc4, rc5, rc6, rc7, rc8, rc9);
        fclose(f);
    }

    printf("IDM selftest: %s (whole=%d resume=%d store=%d sched=%d speed=%d speeddl=%d queue=%d cat=%d torrent=%d rc=%d)\n",
           rc == 0 ? "PASS" : "FAIL", rc1, rc2, rc3, rc4, rc5, rc6, rc7, rc8, rc9, rc);
    return rc;
}
