#include "selftest_run.h"
#include "engine/category.h"
#include "engine/download.h"
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
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
   task_create() 必须在任务进列表前挑好新名字；BT 的 outfile 是目录，不能被改名。 */
static int test_existing_file_guard(void)
{
    wchar_t temp[MAX_PATH], original[MAX_PATH], expected[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return 111;
    _snwprintf(original, MAX_PATH, L"%sswoop-existing-test.bin", temp);
    _snwprintf(expected, MAX_PATH, L"%sswoop-existing-test (1).bin", temp);
    original[MAX_PATH - 1] = expected[MAX_PATH - 1] = 0;
    DeleteFileW(original);
    DeleteFileW(expected);

    HANDLE h = CreateFileW(original, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 112;
    const char marker[] = "keep";
    DWORD wr = 0;
    WriteFile(h, marker, sizeof marker, &wr, NULL);
    CloseHandle(h);

    download_task_t *t = task_create("http://127.0.0.1/file.bin", original, 4);
    if (!t) { DeleteFileW(original); return 113; }
    int rc = 0;
    if (wcscmp(t->outfile, expected) != 0) rc = 114;
    else {
        HANDLE r = CreateFileW(original, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (r == INVALID_HANDLE_VALUE) rc = 115;
        else {
            char buf[8] = {0}; DWORD got = 0;
            ReadFile(r, buf, sizeof buf, &got, NULL);
            CloseHandle(r);
            if (got < sizeof marker || memcmp(buf, marker, sizeof marker) != 0) rc = 116;
        }
    }
    task_free(t);

    /* magnet 的 outfile 是现有下载目录，绝不能变成 "Downloads (1)"。 */
    download_task_t *bt = task_create("magnet:?xt=urn:btih:ABC", temp, 1);
    if (!bt) rc = rc ? rc : 117;
    else {
        wchar_t want[MAX_PATH]; wcsncpy(want, temp, MAX_PATH - 1); want[MAX_PATH - 1] = 0;
        if (wcscmp(bt->outfile, want) != 0 && !rc) rc = 118;
        task_free(bt);
    }

    DeleteFileW(original);
    DeleteFileW(expected);
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
