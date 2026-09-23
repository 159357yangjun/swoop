#include "speedlimit.h"
#include <windows.h>

/* 虚拟时钟限速：维护「累计已放行字节 g_sent」与基准时刻 g_start。
   累计字节对应的应耗时为 g_sent/limit 秒；若实际耗时超前，就休眠补足。
   这样跨线程自然聚合、且任意大的单次请求都能正确处理（不会像令牌桶那样卡死）。 */
static CRITICAL_SECTION g_lock;
static int       g_inited = 0;
static long long g_limit = 0;    /* 字节/秒，0=不限 */
static long long g_sent = 0;     /* 自 g_start 起累计放行字节 */
static DWORD     g_start = 0;

#define IDLE_RESET_MS 5000       /* 空闲超过此时长则重置信用，避免之后无限突发 */

void dl_speed_init(void)
{
    if (g_inited) return;
    InitializeCriticalSection(&g_lock);
    g_start = GetTickCount();
    g_inited = 1;
}

void dl_set_speed_limit(long long bps)
{
    dl_speed_init();
    EnterCriticalSection(&g_lock);
    g_limit = (bps > 0) ? bps : 0;
    g_sent = 0;
    g_start = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

long long dl_get_speed_limit(void) { return g_limit; }

void dl_throttle(long long bytes)
{
    dl_speed_init();
    if (bytes <= 0) return;

    EnterCriticalSection(&g_lock);
    if (g_limit <= 0) { LeaveCriticalSection(&g_lock); return; }

    DWORD now = GetTickCount() - g_start;
    long long allowed_ms = g_sent * 1000 / g_limit;
    if ((long long)now > allowed_ms + IDLE_RESET_MS) {   /* 空闲过久：重置信用 */
        g_start = GetTickCount();
        g_sent = 0;
        now = 0;
    }
    g_sent += bytes;
    long long want_ms = g_sent * 1000 / g_limit;
    long long sleep_ms = want_ms - (long long)now;
    LeaveCriticalSection(&g_lock);

    while (sleep_ms > 0) {                 /* 分段休眠，保持对暂停的响应性 */
        DWORD s = (sleep_ms > 250) ? 250 : (DWORD)sleep_ms;
        Sleep(s);
        sleep_ms -= s;
    }
}
