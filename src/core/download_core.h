/*
 * download_core.h
 * 多线程下载调度引擎 — 对标 IDM 核心
 *
 * 架构：
 *   DownloadManager（全局单例）管理任务队列
 *   每个 DownloadTask 拆分为若干 DownloadChunk（分片）
 *   线程池并发执行分片下载，完成后合并进度
 */

#ifndef DOWNLOAD_CORE_H
#define DOWNLOAD_CORE_H

#include <stdint.h>
#include "network.h"   /* NetOptions：每个任务需要独立的网络选项（站点认证按任务生效） */

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────────────────────────────
 * 常量
 * ───────────────────────────────────── */
#define MAX_CHUNKS_PER_TASK  32    /* 每个任务最多分片数 */
#define MAX_TASKS            128   /* 任务队列最大容量 */
#define THREAD_POOL_SIZE     8     /* 默认线程池大小 */
#define MIN_CHUNK_SIZE       (512 * 1024)      /* 512 KB，小于此不分片 */
#define DEFAULT_CHUNK_SIZE   (4 * 1024 * 1024) /* 4 MB 每片 */
#define MAX_SITE_LOGINS      16    /* 「站点登录」凭据条数上限 */

/* 站点登录凭据（设置页「站点登录」）：命中 match 的请求自动带上 HTTP 认证 */
typedef struct {
    char match[256];   /* 站点匹配规则：域名（如 files.example.com）或 URL 前缀 */
    char user[128];
    char pass[128];
} SiteCredential;

/* ─────────────────────────────────────
 * 任务状态
 * ───────────────────────────────────── */
typedef enum {
    TASK_PENDING   = 0,  /* 等待下载 */
    TASK_RUNNING   = 1,  /* 下载中 */
    TASK_PAUSED    = 2,  /* 已暂停 */
    TASK_COMPLETED = 3,  /* 已完成 */
    TASK_FAILED    = 4,  /* 失败 */
    TASK_CANCELLED = 5   /* 已取消 */
} TaskStatus;

/* ─────────────────────────────────────
 * 分片结构
 * ───────────────────────────────────── */
typedef struct {
    int     chunk_id;
    int64_t start;           /* 分片起始字节 */
    int64_t end;             /* 分片结束字节（含） */
    int64_t downloaded;      /* 已下载字节数 */
    int     done;            /* 1 = 此分片完成 */
    int     retry_count;     /* 已重试次数 */
} DownloadChunk;

/* ─────────────────────────────────────
 * 单个下载任务
 * ───────────────────────────────────── */
typedef struct {
    int         task_id;
    char        url[2048];
    char        save_dir[ 512];   /* 保存目录 */
    char        filename[256];   /* 文件名（含扩展名） */
    int         filename_from_user; /* 1=用户显式指定，0=从URL自动推断（可被服务器 Content-Disposition 覆盖） */
    char        tmp_path[768];   /* 临时下载路径 */

    int64_t     file_size;       /* 远程文件大小，-1=未知 */
    int64_t     downloaded;      /* 已下载总字节 */
    int         chunk_count;     /* 实际分片数 */
    DownloadChunk chunks[MAX_CHUNKS_PER_TASK];

    TaskStatus  status;
    int         thread_count;    /* 并发线程数 */
    int         speed_limit;     /* Bytes/s，0=不限速 */
    int         priority;        /* 0=普通，1=高优先级 */
    int         dispatch_idx;    /* 动态调度：下一个待分配的片索引（线程池 work-stealing 用） */

    /* 槽位生命周期（见 dlmgr_remove 注释）：
     * worker_alive > 0 表示还有线程持有本槽位的指针，此时**绝不能** memmove 数组挪动它，
     * 否则线程会把状态和分片数据写到隔壁任务的槽位上（跨任务污染）。
     * removed = 1 表示调用方已请求删除，等最后一个线程退出时再由它就地摘除本槽位。 */
    int         worker_alive;
    int         removed;
    /* 有线程在跑时收到「重新开始」→ 置位。等线程全部退出后再走一遍重启流程，
     * 避免出现两组线程同时写同一个 .idmtmp 的场面。 */
    int         restart_pending;
    /* 服务器无视 Range（HEAD 报 Accept-Ranges，取 Range 时却回 200+整份文件）。
     * 置位后：监督线程在分片全部退出后丢弃分片、改用单连接从头重下，
     * 且 dlmgr_start 重新探测时也不再分片。置位即长期有效，不随单次下载结束复位。 */
    int         range_downgrade;

    /* 时间统计 */
    int64_t     start_time;      /* Unix 时间戳（ms） */
    int64_t     speed_bps;       /* 当前速度（Bytes/s，计算值） */
    int         eta_sec;         /* 预估剩余时间（秒） */

    /* 速度平滑（滑动窗口 EMA，α=0.3）：比全程平均值更稳更准 */
    int64_t     speed_ema;       /* 平滑后的瞬时速度（Bytes/s） */
    int64_t     ema_bytes;       /* EMA 窗口内累计下载字节 */
    int64_t     ema_window_ms;   /* EMA 窗口已累计时间(ms) */
    int64_t     ema_last_ms;     /* 上次 EMA 采样时间(ms) */

    int64_t     last_progress_cb_ms; /* 上次进度回调时间戳(ms)，用于回调节流：默认每块1MB，>=100ms 或分片末块才真正下发 */

    char        error_msg[256];

    char        http_version[16]; /* 实际协商到的协议版本："HTTP/1.1" / "HTTP/2"（仅 HTTPS 可能 HTTP/2） */

    /* 本任务生效的网络选项：= 全局 g_net_opt 的副本，再按 URL 命中「站点登录」时
     * 补上 HTTP 认证。必须是每任务一份——不同任务可能指向不同站点、用不同凭据。
     * auth_user/auth_pass 是本结构体内的缓冲区，net_opts 的这两个指针指向它们，
     * 因此任务在 g_tasks[] 里移动/复制后指针不会失效（apply_site_auth 会重新指向）。 */
    NetOptions  net_opts;
    char        auth_user[128];
    char        auth_pass[128];

    /* 回调：GUI 刷新进度条用 */
    void (*progress_cb)(int task_id, int64_t downloaded, int64_t total, int speed, void *ud);
    void *progress_ud;
    void (*complete_cb)(int task_id, int success, void *ud);
    void *complete_ud;
} DownloadTask;

/* ─────────────────────────────────────
 * 管理器配置
 * ───────────────────────────────────── */
typedef struct {
    int thread_pool_size;   /* 全局线程池大小 */
    int max_concurrent;     /* 同时下载的任务数 */
    int retry_max;          /* 每片最大重试次数 */
    int speed_limit_global; /* 全局限速 Bytes/s，0 不限 */
    char default_save_dir[512];
    /* 代理（type 取值见 network.h 的 ProxyType：0=无 1=HTTP 2=SOCKS4 3=SOCKS5） */
    int  proxy_type;
    char proxy_host[256];
    int  proxy_port;
    char proxy_user[256];    /* 代理认证用户名（空=无需认证） */
    char proxy_pass[256];    /* 代理认证密码（空=无需认证） */
    char user_agent[256];    /* 自定义 User-Agent（空=引擎默认） */
    int  connect_timeout_sec;/* 连接超时秒数（0=引擎默认 15s） */
    /* 每服务器连接数上限：对单个任务并发到同一主机的连接数封顶。
     * 与「每任务最大线程数」取较小值生效（任一为 0/负数表示不限，用另一个）。 */
    int  max_conn_per_server;
    /* 站点登录凭据：命中 URL 的任务自动带 HTTP 认证（libcurl CURLAUTH_ANY） */
    SiteCredential site_logins[MAX_SITE_LOGINS];
    int  site_login_count;
} DownloadConfig;

/* ─────────────────────────────────────
 * 管理器 API
 * ───────────────────────────────────── */

/* 初始化（程序启动时调用一次） */
int  dlmgr_init(const DownloadConfig *cfg);  /* 0=成功 */

/* 销毁（程序退出前调用）*/
void dlmgr_destroy(void);

/* 添加新任务，返回 task_id（>=0），失败返回 -1 */
int  dlmgr_add(const char *url,
               const char *save_dir,
               const char *filename,     /* NULL=自动从URL推断 */
               int         thread_count, /* 0=使用配置默认值 */
               void (*progress_cb)(int,int64_t,int64_t,int,void*),
               void *progress_ud,
               void (*complete_cb)(int,int,void*),
               void *complete_ud);

/* 控制任务 */
int  dlmgr_start(int task_id);    /* 开始 / 从暂停恢复 */
int  dlmgr_pause(int task_id);    /* 暂停（保留断点） */
int  dlmgr_cancel(int task_id);   /* 取消并删除临时文件 */
int  dlmgr_restart(int task_id);  /* 重新开始：重置分片、删临时文件、重新探测 */
int  dlmgr_remove(int task_id);   /* 从队列移除（已完成的任务） */
/* 重新绑定任务的进度/完成回调（dlmgr_load_state 恢复的任务用） */
int  dlmgr_set_callbacks(int task_id,
                         void (*progress_cb)(int,int64_t,int64_t,int,void*), void* progress_ud,
                         void (*complete_cb)(int,int,void*), void* complete_ud);

/* 获取全部任务 ID 列表（写入 ids 数组），返回数量 */
int  dlmgr_list(int *ids, int max_count);

/* 全局限速（运行时修改） */
void dlmgr_set_speed_limit(int bytes_per_sec);

/* 设置下载代理（运行时生效）；type 见 ProxyType 枚举，host 可为 NULL/空 表示不使用。
 * user/pass 为代理认证凭据（空或 NULL 表示无需认证）。 */
void dlmgr_set_proxy(int type, const char *host, int port,
                     const char *user, const char *pass);

/* 持久化：保存当前任务队列到文件 */
int  dlmgr_save_state(const char *path);

/* 持久化：从文件恢复任务队列（程序重启时调用）*/
int  dlmgr_load_state(const char *path);

/* ─────────────────────────────────────
 * GUI 需要的轻量摘要结构
 * ───────────────────────────────────── */
typedef struct {
    int     task_id;
    char    filename[256];
    char    save_dir[512];    /* 保存目录（供校验/打开文件用时构造完整路径） */
    char    url[2048];        /* 截断版 URL（仅用于显示） */
    int64_t size;
    int     status;
    int64_t speed;            /* 当前速度 B/s */
} TaskSummary;

/* 单个任务的实时详细信息 */
typedef struct {
    int     task_id;
    int64_t downloaded;       /* 已下载字节数 */
    int64_t file_size;        /* 总大小 */
    int64_t speed_bps;        /* 当前速度 B/s */
    int     eta_sec;          /* 剩余秒数 */
    int     status;
    char    filename[256];    /* 文件名（用于 GUI 显示）*/
    char    http_version[16]; /* 实际协商到的协议版本："HTTP/1.1" / "HTTP/2" */
    /* 分片概况：供任务详情面板展示真实分段进度，
     * 避免 GUI 只能显示「详见引擎日志」这类无信息量的占位文案 */
    int     chunk_count;      /* 实际分片数，0 = 尚未分段（未探测/单流） */
    int     chunks_done;      /* 已完成的分片数 */
    /* 最近一次失败原因（引擎写入的中文描述，如「HTTP 404」「无法解析 URL」）。
     * 为空表示无错误记录。GUI 据此给出具体原因，而不是笼统的「请检查网络」。 */
    char    error_msg[256];
} TaskInfo;

/* ─────────────────────────────────────
 * GUI 专用 API
 * ───────────────────────────────────── */

/* 获取任务摘要列表（GUI ListView 刷新用），返回实际数量 */
int  dlmgr_get_task_summary(TaskSummary *out, int max_count);

/* 获取单个任务的实时详细信息 */
int  dlmgr_get_task_info(int task_id, TaskInfo *info);

/* 批量获取所有任务的实时详细信息：单次持锁遍历 g_tasks[]，填充顺序与 dlmgr_list
 * 返回的 ids 完全一致，调用方可按索引对齐直接使用。省去 GUI 4Hz 刷新热路径上每任务
 * 一次的 EnterCriticalSection + find_task 二分查找（原来每 tick 要加锁 n 次、二分 n 次）。 */
int  dlmgr_get_task_infos(TaskInfo *out, int max_count);

/* 获取当前配置副本 */
DownloadConfig dlmgr_get_config(void);

/* 更新配置（运行时生效） */
void dlmgr_set_config(const DownloadConfig *cfg);

/* 开始所有排队/暂停中的任务，返回已启动数量 */
int  dlmgr_start_all(void);

/* 暂停所有正在运行的任务，返回已暂停数量 */
int  dlmgr_stop_all(void);

/* 从队列中删除所有已完成的任务，返回已删除数量 */
int  dlmgr_remove_completed(void);

/* ─────────────────────────────────────
 * 内部工具（供实现文件使用）
 * ───────────────────────────────────── */
/* 从 URL 推断文件名 */
void dl_infer_filename(const char *url, char *out, int max_len);

/* 当前时间戳（毫秒） */
int64_t dl_time_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* DOWNLOAD_CORE_H */
