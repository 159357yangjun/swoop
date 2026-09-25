#include "queue.h"

int queue_slots(int running, int max_concurrent)
{
    if (running < 0) running = 0;
    if (max_concurrent <= 0) return 0x7fffffff;   /* 不限并发 */
    int s = max_concurrent - running;
    return (s > 0) ? s : 0;
}

int queue_pick(download_task_t *const *tasks, int n, int slots,
               int *out_idx, int maxout)
{
    if (!tasks || !out_idx || maxout <= 0 || slots <= 0) return 0;
    int picked = 0;
    for (int i = 0; i < n && picked < slots && picked < maxout; i++) {
        download_task_t *t = tasks[i];
        if (!t) continue;
        if (t->status != DL_QUEUED) continue;
        if (t->user_paused) continue;      /* 用户明确暂停的，调度不得自动拉起 */
        out_idx[picked++] = i;
    }
    return picked;
}
