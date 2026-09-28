#include "selftest_run.h"
#include "engine/category.h"
#include "engine/download.h"
#include "engine/torrent.h"
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_safe_derived_filenames(void)
{
    wchar_t out[MAX_PATH];
    category_filename_from_url(L"http://host/a%2Fb%5Cc.txt", out, MAX_PATH);
    if (wcscmp(out, L"a_b_c.txt") != 0) return 101;
    category_filename_from_url(L"http://host/CON.txt", out, MAX_PATH);
    if (wcscmp(out, L"_CON.txt") != 0) return 102;
    category_filename_from_url(L"http://host/file.txt.%20", out, MAX_PATH);
    if (wcscmp(out, L"file.txt") != 0) return 103;
    category_filename_from_url(L"http://host/%00evil.txt", out, MAX_PATH);
    if (wcscmp(out, L"_evil.txt") != 0) return 104;
    category_build_path(L"C:\\DL", L"http://host/%2e%2e%5csecret.txt", 0, out, MAX_PATH);
    if (wcscmp(out, L"C:\\DL\\.._secret.txt") != 0) return 105;
    return 0;
}

/* 新任务不能因为 task_start() 的 CREATE_ALWAYS 静默截断用户已有文件。
   还要覆盖两个“都没落盘”的排队任务：它们必须分别预留 (1)/(2)，不能等到
   CreateFileW 时才发现冲突。task_free 后预留必须释放。 */
static int test_existing_file_guard(void)
{
    wchar_t temp[MAX_PATH], original[MAX_PATH], expected1[MAX_PATH], expected2[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 111;
    _snwprintf(original, MAX_PATH, L"%sswoop-existing-test.bin", temp);
    _snwprintf(expected1, MAX_PATH, L"%sswoop-existing-test (1).bin", temp);
    _snwprintf(expected2, MAX_PATH, L"%sswoop-existing-test (2).bin", temp);
    original[MAX_PATH - 1] = expected1[MAX_PATH - 1] = expected2[MAX_PATH - 1] = 0;
    DeleteFileW(original);
    DeleteFileW(expected1);
    DeleteFileW(expected2);

    HANDLE h = CreateFileW(original, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 112;
    const char marker[] = "keep";
    DWORD wr = 0;
    WriteFile(h, marker, sizeof marker, &wr, NULL);
    CloseHandle(h);

    download_task_t *t1 = task_create("http://127.0.0.1/file-a.bin", original, 4);
    if (!t1) { DeleteFileW(original); return 113; }
    int rc = 0;
    if (wcscmp(t1->outfile, expected1) != 0) rc = 114;

    download_task_t *t2 = task_create("http://127.0.0.1/file-b.bin", original, 4);
    if (!t2) rc = rc ? rc : 115;
    else if (wcscmp(t2->outfile, expected2) != 0 && !rc) rc = 116;

    /* 选择候选路径本身不能碰原文件。 */
    if (!rc) {
        HANDLE r = CreateFileW(original, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (r == INVALID_HANDLE_VALUE) rc = 117;
        else {
            char buf[8] = {0}; DWORD got = 0;
            ReadFile(r, buf, sizeof buf, &got, NULL);
            CloseHandle(r);
            if (got < sizeof marker || memcmp(buf, marker, sizeof marker) != 0) rc = 118;
        }
    }

    if (t2) task_free(t2);
    task_free(t1);

    /* 两个任务释放后，(1) 应重新可用；预留表不能泄漏。 */
    download_task_t *t3 = task_create("http://127.0.0.1/file-c.bin", original, 4);
    if (!t3) rc = rc ? rc : 119;
    else {
        if (wcscmp(t3->outfile, expected1) != 0 && !rc) rc = 120;
        task_free(t3);
    }

    /* magnet 的 outfile 是现有下载目录，绝不能变成 "Downloads (1)"。 */
    download_task_t *bt = task_create("magnet:?xt=urn:btih:ABC", temp, 1);
    if (!bt) rc = rc ? rc : 121;
    else {
        wchar_t want[MAX_PATH]; wcsncpy(want, temp, MAX_PATH - 1); want[MAX_PATH - 1] = 0;
        if (wcscmp(bt->outfile, want) != 0 && !rc) rc = 122;
        task_free(bt);
    }

    DeleteFileW(original);
    DeleteFileW(expected1);
    DeleteFileW(expected2);
    return rc;
}

/* 预留是在「排队挑名字」那一刻做的，真正开文件却是之后（队列放行、另一个进程
   抢先落盘、浏览器已经存了一份同名文件）。这个用例专门踩这个时间窗：
   先让 task_create 拿到一个空闲名字 → 外部用同名文件写入 marker → task_start。
   要求：别人的 marker 必须原样还在（不许被截断），任务必须自己换到 " (1)" 后缀。
   由 run_selftest() 调用，因为需要自测本地 HTTP 服务在跑（端口与其 TEST_PORT 一致）。
   放在本文件而不是 selftest_run.c：那边 task_create 被 Makefile 的 -D 映射成夹具，
   走的不是预留路径，测不到这件事。 */
int test_race_at_start(void)
{
    wchar_t base[MAX_PATH], alt[MAX_PATH], used[MAX_PATH];
    used[0] = 0;
    if (!GetTempPathW(MAX_PATH, base)) return 161;
    wcsncat(base, L"swoop-race-test.bin", MAX_PATH - wcslen(base) - 1);
    base[MAX_PATH - 1] = 0;
    wcsncpy(alt, base, MAX_PATH - 1);
    alt[MAX_PATH - 1] = 0;
    wchar_t *dot = wcsrchr(alt, L'.');
    if (!dot) return 162;
    wchar_t suffix[MAX_PATH];
    _snwprintf(suffix, MAX_PATH, L" (1)%s", dot);
    suffix[MAX_PATH - 1] = 0;
    size_t stem = (size_t)(dot - alt);
    wchar_t alt2[MAX_PATH];
    wcsncpy(alt2, alt, stem); alt2[stem] = 0;
    _snwprintf(alt, MAX_PATH, L"%ls%ls", alt2, suffix);
    alt[MAX_PATH - 1] = 0;
    DeleteFileW(base);
    DeleteFileW(alt);

    char curl[256];
    snprintf(curl, sizeof curl, "http://127.0.0.1:%d/bigfile", 18099);
    download_task_t *t = task_create(curl, base, 4);
    if (!t) return 163;
    int rc = 0;
    if (wcscmp(t->outfile, base) != 0) { rc = 164; task_free(t); goto done; }  /* 起点必须空闲 */

    /* 抢占：模拟"挑完名字之后、开文件之前"有别的东西把这个名字用了 */
    HANDLE h = CreateFileW(base, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { rc = 165; task_free(t); goto done; }
    const char marker[] = "keep";
    DWORD wr = 0;
    WriteFile(h, marker, sizeof marker, &wr, NULL);
    CloseHandle(h);

    if (task_start(t) != 0) { rc = 166; goto bail; }
    /* 开文件这一步必须换名，不能把别人的文件截断 */
    if (wcscmp(t->outfile, base) == 0) { rc = 167; goto bail; }

    wchar_t probe[MAX_PATH];
    wcsncpy(probe, base, MAX_PATH - 1); probe[MAX_PATH - 1] = 0;
    char buf[8] = {0}; DWORD got = 0;
    HANDLE r = CreateFileW(probe, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (r == INVALID_HANDLE_VALUE) { rc = 168; goto bail; }
    ReadFile(r, buf, sizeof buf, &got, NULL);
    CloseHandle(r);
    if (strncmp(buf, "keep", 4) != 0) { rc = 169; goto bail; }   /* 被截断就是这里 */

    /* 换名之后任务本身仍要能正常跑完，别留下半截文件 */
    wcsncpy(used, t->outfile, MAX_PATH - 1); used[MAX_PATH - 1] = 0;
    for (int i = 0; i < 2000 && t->status == DL_DOWNLOADING; i++) {
        task_tick(t, GetTickCount());
        Sleep(10);
    }
    if (t->status != DL_COMPLETE) rc = 170;

bail:
    task_free(t);
    if (used[0]) DeleteFileW(used);
done:
    DeleteFileW(base);
    DeleteFileW(alt);
    return rc;
}

int main(void)
{
    int safe = test_safe_derived_filenames();
    if (safe != 0) {
        printf("Swoop filename safety selftest: FAIL rc=%d\n", safe);
        return safe;
    }
    int guard = test_existing_file_guard();
    if (guard != 0) {
        printf("Swoop overwrite guard selftest: FAIL rc=%d\n", guard);
        return guard;
    }
    return run_selftest();
}
