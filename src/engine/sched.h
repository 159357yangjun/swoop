#ifndef IDM_SCHED_H
#define IDM_SCHED_H

/* 定时调度：到点「开始全部」或「暂停全部」。时刻用 HHMM 表示（如 100=01:00，800=08:00）。 */
typedef enum { SCHED_NONE = 0, SCHED_START = 1, SCHED_STOP = 2 } sched_action;

/* 纯函数（便于自测）：给定使能与开始/停止/当前时刻，决定该动作。
   未使能、时刻非法、或 start==stop 时返回 SCHED_NONE。 */
sched_action sched_decide(int enabled, int start_hhmm, int stop_hhmm, int cur_hhmm);

#endif
