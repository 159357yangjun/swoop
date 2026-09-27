#include "selftest_run.h"
#include "engine/category.h"
#include <windows.h>
#include <wchar.h>
#include <stdio.h>

/* category_filename_from_url() 处理的是不可信 URL/站点建议名。
   这些用例专门防止 percent-decode 之后重新出现路径分隔符、设备名或非法尾字符。 */
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

int main(void)
{
    int safe = test_safe_derived_filenames();
    if (safe != 0) {
        printf("Swoop filename safety selftest: FAIL rc=%d\n", safe);
        return safe;
    }
    return run_selftest();
}
