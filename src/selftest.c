#include "selftest_run.h"
#include "engine/category.h"
#include "engine/download.h"
#include "engine/torrent.h"
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 仅供 selftest_run.c 的旧持久化夹具（Makefile 用宏把它的 task_create 映射到这里）。
   夹具经常先创建“部分下载文件”再构造任务，因此既不能自动改名，也不能占用
   生产预留表；真正应用恢复由 taskstore_load() 负责登记。 */
download_task_t *task_create_test_fixture(const char *url, const wchar_t *outfile, int num_conn)
{
    if (!url || !url[0] || !outfile || !outfile[0]) return NULL;
    download_task_t *t = (download_task_t *)calloc(1, sizeof *t);
    if (!t) return NULL;
    strncpy(t->url, url, sizeof t->url - 1);
    t->url[sizeof t->url - 1] = 0;
    wcsncpy(t->outfile, outfile, MAX_PATH - 1);
    t->outfile[MAX_PATH - 1] = 0;
    t->kind = torrent_kind_for_url(url);
    t->num_conn = num_conn;
    if (t->num_conn < 1) t->num_conn = 1;
    if (t->num_conn > 16) t->num_conn = 16;
    t->status = DL_QUEUED;
    return t;
}

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
