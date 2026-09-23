#ifndef IDM_DOWNLOAD_H
#define IDM_DOWNLOAD_H
#include <windows.h>

typedef enum {
    DL_QUEUED, DL_DOWNLOADING, DL_PAUSED, DL_COMPLETE, DL_ERROR
} dl_status;

typedef struct {
    char        url[2048];
    wchar_t     outfile[MAX_PATH];
    long long   total;
    volatile LONGLONG downloaded;   /* 各段已写字节之和，供 UI 显示 */
    dl_status   status;
    int         num_conn;
    double      speed;        /* 字节/秒，由 UI 计时器刷新 */
    volatile LONG threads_done;     /* 本轮已完成的分片线程数 */
    volatile int  threads_expected; /* 本轮启动的分片线程数 */
    HANDLE      threads[16];        /* 按段号索引 */
    volatile int running;
    DWORD       last_tick;
    LONGLONG    last_downloaded;
    /* 分段状态：每段起点/长度/已写字节。多线程分片是乱序写入的，
       续传必须按段各自补齐（用单一 downloaded 当起点会留下空洞）。 */
    int         seg_count;
    long long   seg_start[16];
    long long   seg_len[16];
    volatile LONGLONG seg_written[16];
    /* BT/磁力：kind = IDM_KIND_TORRENT 时交给 aria2c 子进程；proc 为其进程句柄 */
    int         kind;
    void       *proc;
} download_task_t;

download_task_t *task_create(const char *url, const wchar_t *outfile, int num_conn);
void  task_free(download_task_t *t);
int   task_start(download_task_t *t);   /* 全新下载，或 DL_PAUSED 时续传 */
void  task_stop(download_task_t *t);    /* 仅发停止信号（running=0），不回收线程 */
void  task_pause(download_task_t *t);   /* 停止并等待分片线程退出，标记 DL_PAUSED */
void  task_join(download_task_t *t);    /* 等待所有分片线程结束并回收句柄 */
int   task_tick(download_task_t *t, DWORD now_ms); /* 返回 1 表示状态有变化 */

#endif
