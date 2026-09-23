#include "queue.h"

int queue_slots(int running, int max_concurrent)
{
    if (running < 0) running = 0;
    if (max_concurrent <= 0) return 0x7fffffff;   /* 不限并发 */
    int s = max_concurrent - running;
    return (s > 0) ? s : 0;
}
