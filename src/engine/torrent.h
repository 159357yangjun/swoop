#ifndef IDM_TORRENT_H
#define IDM_TORRENT_H
#include <windows.h>

#define IDM_KIND_HTTP    0
#define IDM_KIND_TORRENT 1

/* 由 URL 判别任务类型：magnet: 或 *.torrent → TORRENT，否则 HTTP。 */
int torrent_kind_for_url(const char *url);

/* 组装 aria2c 参数（不含 exe 路径）。成功返回 0。 */
int torrent_build_args(const char *url, const wchar_t *dir, wchar_t *out, int n);

/* 启动 aria2c（先在 exe_dir 下找 aria2c.exe，再退回 PATH）。成功返回进程句柄，失败 NULL。 */
HANDLE torrent_start(const char *url, const wchar_t *dir, const wchar_t *exe_dir);

#endif
