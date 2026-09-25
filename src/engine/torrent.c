#include "torrent.h"
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <shlobj.h>      /* SHCreateDirectoryExW */

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

    /* 保存目录不存在就建（aria2 一般也会建，但用户手填错路径时先建更可靠） */
    SHCreateDirectoryExW(NULL, dir, NULL);

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

/* ---- 任务显示名 ---- */

/* %XX 解码（按 UTF-8 解释），'+' 视作空格。结果写进 out。 */
static void pct_decode_to_wide(const char *in, wchar_t *out, int n)
{
    char bytes[1024];
    int j = 0;
    for (const char *p = in; *p && j < (int)sizeof bytes - 1; ) {
        if (*p == '%' && p[1] && p[2]) {
            int hi = -1, lo = -1, k;
            for (k = 0; k < 2; k++) {
                char ch = p[1 + k];
                int v = -1;
                if (ch >= '0' && ch <= '9') v = ch - '0';
                else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
                else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
                if (v < 0) break;
                if (k == 0) hi = v; else lo = v;
            }
            if (k == 2) { bytes[j++] = (char)((hi << 4) | lo); p += 3; continue; }
        }
        bytes[j++] = (*p == '+') ? ' ' : *p;
        p++;
    }
    bytes[j] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, bytes, -1, out, n)) out[0] = 0;
}

void torrent_display_name(const char *url, wchar_t *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (!url || !url[0]) return;

    if (_strnicmp(url, "magnet:", 7) == 0) {
        const char *p = strstr(url, "dn=");
        if (p) {
            p += 3;
            char enc[512]; int j = 0;
            for (; p[j] && p[j] != '&' && j < 511; j++) enc[j] = p[j];
            enc[j] = 0;
            pct_decode_to_wide(enc, out, n);
        }
        if (!out[0]) wcsncpy(out, L"磁力任务", n - 1);
        out[n - 1] = 0;
        return;
    }

    /* .torrent：取末段文件名并去掉 .torrent 后缀 */
    wchar_t wu[2048];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wu, 2048)) {
        wchar_t *q = wcschr(wu, L'?'); if (q) *q = 0;
        wchar_t *f = wcsrchr(wu, L'/'); f = f ? f + 1 : wu;
        size_t L = wcslen(f);
        if (L > 8 && _wcsicmp(f + L - 8, L".torrent") == 0) f[L - 8] = 0;
        if (f[0]) { wcsncpy(out, f, n - 1); out[n - 1] = 0; return; }
    }
    wcsncpy(out, L"BT 任务", n - 1);
    out[n - 1] = 0;
}
