#ifndef IDM_QUEUE_H
#define IDM_QUEUE_H

/* 下载队列并发控制：给定当前正在下载数与并发上限，返回还能再启动几个。
   max_concurrent <= 0 视为"不限"（返回一个很大的数）。 */
int queue_slots(int running, int max_concurrent);

#endif
