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

/* 确保文件路径的父目录存在（逐级创建）。返回 0 成功。
   下载前必须调用：分类子目录（Downloads\Videos 等）默认并不存在。 */
int ensure_dir_for_file(const wchar_t *filepath);

/* 取 exe 所在目录（含结尾反斜杠）。 */
void exe_dir(wchar_t *out, int n);

/* 取可写数据目录（数据/日志用）。
   优先 exe 目录（便携）；不可写（如装在 Program Files）则退到
   %LOCALAPPDATA%\Swoop，避免任务列表静默丢失。成功返回 0。 */
int  data_dir(wchar_t *out, int n);

/* 用系统默认程序打开文件或目录（资源管理器）。成功返回 0。 */
int open_path(const wchar_t *path);

#endif
