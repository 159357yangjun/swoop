#ifndef IDM_UTIL_H
#define IDM_UTIL_H
#include <windows.h>
#include <stddef.h>

/* IDM 风格：网络 DLL 运行时动态加载，绝不写进静态导入表，主 exe 才轻。 */
FARPROC load_dll_func(const char *dll, const char *func);

char    *str_dup(const char *s);
wchar_t *str_dup_w(const wchar_t *s);
int      str_ends_with_i(const char *s, const char *suf);

void format_bytes(long long bytes, char *out, size_t n);
void format_speed(double bps, char *out, size_t n);

void log_init(const wchar_t *exe_dir);
void log_msg(const char *fmt, ...);

int  settings_get_int(const char *key, int def);
void settings_set_int(const char *key, int val);

/* 字符串设置（REG_SZ，宽字符）。get 成功返回 1。 */
int  settings_get_str(const char *key, wchar_t *out, int n);
void settings_set_str(const char *key, const wchar_t *val);

#endif
