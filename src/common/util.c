#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include <stdio.h>
#include <shlobj.h>      /* SHCreateDirectoryExW */
#include <shellapi.h>    /* ShellExecuteW */

static FILE *g_log = NULL;

/* load_dll_func：每次 LoadLibrary（系统会去重，不会重复加载），
   这样 winhttp 等网络 DLL 不出现在静态导入里 —— 这正是 IDMan.exe 的瘦身手法。 */
FARPROC load_dll_func(const char *dll, const char *func)
{
    HMODULE h = LoadLibraryA(dll);
    if (!h) return NULL;
    return GetProcAddress(h, func);
}

char *str_dup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

wchar_t *str_dup_w(const wchar_t *s)
{
    if (!s) return NULL;
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *p = (wchar_t *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int str_ends_with_i(const char *s, const char *suf)
{
    if (!s || !suf) return 0;
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    return strcasecmp(s + ls - lf, suf) == 0;
}

void format_bytes(long long bytes, char *out, size_t n)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0; double v = (double)bytes;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) _snprintf(out, n, "%lld %s", bytes, u[0]);
    else        _snprintf(out, n, "%.2f %s", v, u[i]);
}

void format_speed(double bps, char *out, size_t n)
{
    if (bps <= 0) { _snprintf(out, n, "0 B/s"); return; }
    const char *u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0; double v = bps;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    _snprintf(out, n, "%.2f %s", v, u[i]);
}

void log_init(const wchar_t *exe_dir)
{
    wchar_t path[MAX_PATH];
    wcscpy(path, exe_dir);
    wcscat(path, L"\\swoop.log");
    g_log = _wfopen(path, L"a");
}

void log_msg(const char *fmt, ...)
{
    if (!g_log) return;
    time_t t = time(NULL);
    struct tm lt; localtime_s(&lt, &t);
    fprintf(g_log, "[%02d:%02d:%02d] ", lt.tm_hour, lt.tm_min, lt.tm_sec);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

int settings_get_int(const char *key, int def)
{
    HKEY h; LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\YangJun\\Swoop", 0, KEY_READ, &h);
    if (r != ERROR_SUCCESS) return def;
    DWORD v = 0, sz = sizeof(v);
    r = RegQueryValueExA(h, key, NULL, NULL, (LPBYTE)&v, &sz);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS) ? (int)v : def;
}

void settings_set_int(const char *key, int val)
{
    HKEY h; DWORD disp;
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\YangJun\\Swoop", 0, NULL,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp);
    if (r != ERROR_SUCCESS) return;
    RegSetValueExA(h, key, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
    RegCloseKey(h);
}

int settings_get_str(const char *key, wchar_t *out, int n)
{
    if (!out || n <= 0) return 0;
    out[0] = 0;
    HKEY h;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\YangJun\\Swoop", 0,
                      KEY_READ, &h) != ERROR_SUCCESS) return 0;
    wchar_t wkey[128];
    if (!MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 128)) { RegCloseKey(h); return 0; }
    DWORD type = 0, sz = (DWORD)(n * sizeof(wchar_t));
    LONG r = RegQueryValueExW(h, wkey, NULL, &type, (LPBYTE)out, &sz);
    RegCloseKey(h);
    if (r != ERROR_SUCCESS || type != REG_SZ) { out[0] = 0; return 0; }
    out[n - 1] = 0;
    return 1;
}

void settings_set_str(const char *key, const wchar_t *val)
{
    if (!val) return;
    HKEY h; DWORD disp;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\YangJun\\Swoop", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return;
    wchar_t wkey[128];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 128))
        RegSetValueExW(h, wkey, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

/* ---- 路径工具 ---- */

/* 逐级创建 filepath 的父目录。SHCreateDirectoryExW 会一路把中间层建齐，
   并正确处理盘符 / UNC。返回 0 表示目录已存在或创建成功。 */
int ensure_dir_for_file(const wchar_t *filepath)
{
    if (!filepath || !filepath[0]) return -1;
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, filepath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (!sl) return 0;          /* 没有目录成分 → 当前目录 */
    if (sl == dir) return 0;    /* 形如 "\file"，根目录必存在 */
    *sl = 0;

    int r = SHCreateDirectoryExW(NULL, dir, NULL);
    if (r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS) return 0;
    return -1;
}

void exe_dir(wchar_t *out, int n)
{
    if (!out || n <= 0) return;
    GetModuleFileNameW(NULL, out, n);
    wchar_t *sl = wcsrchr(out, L'\\');
    if (sl) *(sl + 1) = 0;
    else out[0] = 0;
}

/* exe 目录可写就用它（便携），否则退 %LOCALAPPDATA%\Swoop。 */
int data_dir(wchar_t *out, int n)
{
    if (!out || n <= 0) return -1;
    wchar_t d[MAX_PATH];
    exe_dir(d, MAX_PATH);

    /* 探针文件：能建就能删，说明目录可写 */
    wchar_t probe[MAX_PATH];
    _snwprintf(probe, MAX_PATH, L"%s.swoopwrite", d);
    HANDLE h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        DeleteFileW(probe);
        _snwprintf(out, n, L"%s", d);
        return 0;
    }

    wchar_t base[MAX_PATH];
    DWORD r = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (r == 0 || r >= MAX_PATH) return -1;
    _snwprintf(out, n, L"%s\\Swoop", base);
    SHCreateDirectoryExW(NULL, out, NULL);
    if (out[0] && out[wcslen(out) - 1] != L'\\') {
        size_t L = wcslen(out);
        if (L + 1 < (size_t)n) { out[L] = L'\\'; out[L + 1] = 0; }
    }
    return 0;
}

int open_path(const wchar_t *path)
{
    if (!path || !path[0]) return -1;
    /* shell32 已在静态导入里（托盘图标），不新增依赖 */
    HINSTANCE r = ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)r > 32) ? 0 : -1;
}
