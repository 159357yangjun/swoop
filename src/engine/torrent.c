#include "torrent.h"
#include <wchar.h>
#include <string.h>
#include <stdio.h>

int torrent_kind_for_url(const char *url)
{
    if (!url) return IDM_KIND_HTTP;
    if (_strnicmp(url, "magnet:", 7) == 0) return IDM_KIND_TORRENT;

    size_t len = strcspn(url, "?#");    /* 去掉 query / fragment */
    if (len >= 8 && _strnicmp(url + len - 8, ".torrent", 8) == 0)
        return IDM_KIND_TORRENT;
    return IDM_KIND_HTTP;
}

int torrent_build_args(const char *url, const wchar_t *dir, wchar_t *out, int n)
{
    wchar_t wurl[2048];
    if (!url || !dir || !out || n <= 0) return -1;
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) return -1;

    int r = _snwprintf(out, n,
        L"--dir=\"%s\" --seed-time=0 --bt-stop-timeout=0 "
        L"--console-log-level=warn --summary-interval=0 \"%s\"",
        dir, wurl);
    return (r > 0 && r < n) ? 0 : -1;
}

HANDLE torrent_start(const char *url, const wchar_t *dir, const wchar_t *exe_dir)
{
    if (!url || !dir) return NULL;

    wchar_t exe[MAX_PATH];
    _snwprintf(exe, MAX_PATH, L"%saria2c.exe", exe_dir ? exe_dir : L"");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
        wcscpy(exe, L"aria2c.exe");        /* 退回 PATH 查找 */

    wchar_t argv[4096];
    if (torrent_build_args(url, dir, argv, 4096) != 0) return NULL;

    /* 命令行首 token 必须是程序名（否则 aria2 会把第一个选项当 argv[0]） */
    wchar_t cmd[4600];
    _snwprintf(cmd, 4600, L"\"%s\" %s", exe, argv);

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return NULL;

    CloseHandle(pi.hThread);
    return pi.hProcess;
}
