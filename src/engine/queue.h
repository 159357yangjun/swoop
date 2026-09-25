#ifndef IDM_QUEUE_H
#define IDM_QUEUE_H

#include "download.h"

/* 下载队列并发控制：给定当前正在下载数与并发上限，返回还能再启动几个。
   max_concurrent <= 0 视为"不限"（返回一个很大的数）。 */
int queue_slots(int running, int max_concurrent);

/* 挑出本轮应当启动的任务下标（纯函数，便于自测）：
   只挑「排队中且用户没手动暂停」的任务，最多 slots 个，按列表顺序先到先得。
   返回挑中的个数（<= maxout）。
   —— 必须跳过 user_paused：否则用户点过「暂停」的任务会被调度器反复拉起来。 */
int queue_pick(download_task_t *const *tasks, int n, int slots,
               int *out_idx, int maxout);

#endif
