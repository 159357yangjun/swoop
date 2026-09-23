#ifndef IDM_TASKSTORE_H
#define IDM_TASKSTORE_H
#include "engine/download.h"

/* 任务列表持久化（UTF-8 文本，含每段进度，重启后可正确续传）。
   save：返回写入条数，-1 失败。
   load：最多 maxn 条，*outn 为条数；返回 0 成功，-1 失败/不存在。
   载入时会把「上次正在下载」的任务降级为 DL_PAUSED，并校验部分文件；
   文件缺失/大小不符则重置为从头下载。 */
int taskstore_save(const wchar_t *path, download_task_t *const *tasks, int n);
int taskstore_load(const wchar_t *path, download_task_t **tasks, int maxn, int *outn);

#endif
