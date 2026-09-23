#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

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
    wcscat(path, L"\\idm.log");
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
    HKEY h; LONG r = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\IDMNextNative", 0, KEY_READ, &h);
    if (r != ERROR_SUCCESS) return def;
    DWORD v = 0, sz = sizeof(v);
    r = RegQueryValueExA(h, key, NULL, NULL, (LPBYTE)&v, &sz);
    RegCloseKey(h);
    return (r == ERROR_SUCCESS) ? (int)v : def;
}

void settings_set_int(const char *key, int val)
{
    HKEY h; DWORD disp;
    LONG r = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\IDMNextNative", 0, NULL,
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
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\IDMNextNative", 0,
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
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\IDMNextNative", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp) != ERROR_SUCCESS)
        return;
    wchar_t wkey[128];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 128))
        RegSetValueExW(h, wkey, 0, REG_SZ, (const BYTE *)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}
