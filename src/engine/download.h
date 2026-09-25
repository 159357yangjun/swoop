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
    /* 浏览器给的来源页，防盗链站点需要；空串表示不发 */
    wchar_t     referer[2048];
    /* 1 = 用户手动暂停，定时调度不得自动恢复（否则覆盖用户意图） */
    int         user_paused;
    /* 本轮分片线程里传输失败的个数（非 2xx / 连接断）——>0 则整任务判错 */
    volatile LONG io_errors;
} download_task_t;

download_task_t *task_create(const char *url, const wchar_t *outfile, int num_conn);
void  task_free(download_task_t *t);
int   task_start(download_task_t *t);   /* 全新下载，或 DL_PAUSED 时续传 */
void  task_stop(download_task_t *t);    /* 仅发停止信号（running=0），不回收线程 */
void  task_pause(download_task_t *t);   /* 停止并等待分片线程退出，标记 DL_PAUSED */
void  task_join(download_task_t *t);    /* 等待所有分片线程结束并回收句柄 */
int   task_tick(download_task_t *t, DWORD now_ms); /* 返回 1 表示状态有变化 */

/* 分片线程全部结束后的完成判定（纯函数，便于自测）。
   返回 1 = 完成，0 = 失败。
   total < 0 表示长度未知（服务端没给 Content-Length）——此时必须「收到过数据」
   才算成功，否则 404/403 错误页会被判成下载完成。 */
int   dl_verdict(long long total, long long downloaded, int io_errors);

/* 单段本轮的下发计划（纯函数，便于自测）。
   already = 该段已写字节数。返回 1 = 本段还需下载、要起线程；0 = 已补齐。
   out_start   本轮请求的起始绝对偏移
   out_len     本轮请求的字节数（0 = 下到结束）
   out_written0 分段线程的计数起点，必须 = already —— 线程退出时会把累计值写回
                seg_written，若从 0 起算就会覆盖掉暂停前的进度（多次续传后进度 >100%）。 */
int   task_seg_plan(long long seg_start, long long seg_len, long long already,
                    long long *out_start, long long *out_len, long long *out_written0);

#endif
