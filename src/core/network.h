/*
 * network.h
 * WinHTTP 原生网络请求模块 — 完整接口定义
 *
 * 提供：网页源码下载、文件分片下载、通用 GET/POST/HEAD 请求
 * 支持：HTTPS / Range 分片 / 重定向跟随 / 代理 / Cookie
 */
#ifndef NETWORK_H
#define NETWORK_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   /* FILE*（network_download_range_fp 复用调用方句柄） */

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────────────────────────────────────
 * 代理类型枚举
 * ───────────────────────────────────────────── */
typedef enum {
    PROXY_NONE = 0,     /* 不使用代理（默认） */
    PROXY_HTTP = 1,     /* HTTP 代理 */
    PROXY_SOCKS4 = 2,   /* SOCKS4 代理 */
    PROXY_SOCKS5 = 3    /* SOCKS5 代理 */
} ProxyType;

/* ─────────────────────────────────────────────
 * 初始化 / 清理（程序启动 / 退出各调用一次）
 * ───────────────────────────────────────────── */
int  network_init(void);   /* 返回 0 成功，-1 失败 */
void network_cleanup(void);

/* 运行时开关：启用/禁用 HTTP/2 协商（HTTPS/ALPN）。
 * 若全局 session 已创建则立即重新应用，后续新建连接按新策略协商。 */
void network_set_http2_enabled(int enabled);

/* ─────────────────────────────────────────────
 * 请求选项
 * ───────────────────────────────────────────── */
typedef struct {
    const char *user_agent;        /* NULL 时使用默认 Chrome UA */
    const char *referer;           /* 防盗链 Referer */
    const char *cookie;            /* Cookie 字符串 "name=value; name2=val2" */
    const char *proxy_addr;        /* 代理地址 "127.0.0.1:7890"，NULL=不使用 */
    ProxyType   proxy_type;        /* 代理类型 */
    const char *proxy_user;        /* 代理认证用户名（可为 NULL） */
    const char *proxy_pass;        /* 代理认证密码（可为 NULL） */
    const char *auth_user;         /* 目标站点 HTTP 认证用户名（可为 NULL = 不认证） */
    const char *auth_pass;         /* 目标站点 HTTP 认证密码 */
    int         timeout_sec;       /* 请求超时秒数，0=默认30s */
    int         connect_timeout_sec;/* 连接超时秒数，0=默认15s */
    int         follow_redirect;   /* 最大重定向次数，0=不跟随 */
    int         ssl_verify;        /* 1=验证证书(默认) 0=忽略证书错误 */
    int         max_retry;         /* 失败最大重试次数，0=不重试 */
    int         retry_delay_ms;    /* 重试间隔毫秒，默认1000 */
} NetOptions;

/* 返回填充好默认值的选项结构体 */
NetOptions network_default_options(void);

/* ─────────────────────────────────────────────
 * 内存响应体（用于下载网页源码）
 * ───────────────────────────────────────────── */
typedef struct {
    char  *data;      /* malloc 的缓冲区，调用方 free() */
    size_t size;
    long   http_code;
    char   content_type[128];
    char   http_version[16]; /* 实际协商到的协议版本："HTTP/1.1" / "HTTP/2"（仅 HTTPS 可能 HTTP/2） */
} NetResponse;

/*
 * network_get_html()
 *   下载 url 对应的网页，返回完整 HTML 字符串。
 *   自动处理 gzip 解压和重定向跟随。
 *   失败返回 .data = NULL。调用方 free(resp.data)。
 */
NetResponse network_get_html(const char *url, const NetOptions *opt);

/*
 * network_post()
 *   发起 POST 请求，post_data 为 URL-encoded 或 JSON 字符串。
 *   调用方 free(resp.data)。
 */
NetResponse network_post(const char *url, const char *post_data,
                         size_t post_len, const NetOptions *opt);

/* ─────────────────────────────────────────────
 * 文件分片下载（多线程调度引擎调用此接口）
 * ───────────────────────────────────────────── */

/* 进度回调：已下字节、总字节、userdata，返回非0中断下载 */
typedef int (*NetProgressCb)(int64_t downloaded, int64_t total, void *userdata);

typedef struct {
    const char    *url;
    const char    *save_path;      /* 写入的本地文件路径 */
    int64_t        range_start;    /* HTTP Range 起始字节 */
    int64_t        range_end;      /* -1 表示下载到文件末尾 */
    NetProgressCb  progress_cb;    /* 可为 NULL */
    void          *progress_data;
    const NetOptions *opt;
} NetDownloadTask;

typedef struct {
    int     success;               /* 1 = 成功 */
    int64_t bytes_written;
    long    http_code;
    int     range_ignored;         /* 1 = 服务器无视 Range（要中段却回 200+整份），
                                    * 分段引擎据此降级为单连接续下，而不是产出错位文件 */
    int     no_retry;              /* 1 = 确定性失败，重试无意义（磁盘满/权限/文件被占用）。
                                    * 调用方应直接把 error_msg 抛给用户，不要白等重试间隔 */
    char    error_msg[256];
    char    http_version[16];      /* 实际协商到的协议版本："HTTP/1.1" / "HTTP/2" */
} NetDownloadResult;

/*
 * network_download_range()
 *   下载 url 的 [range_start, range_end] 字节范围，
 *   追加写入 save_path 文件的对应偏移位置。
 *   线程安全：每次调用内部独立创建 WinHTTP handle。
 *   内置指数退避重试逻辑。
 */
NetDownloadResult network_download_range(const NetDownloadTask *task);

/*
 * network_download_range_fp()
 *   与 network_download_range 等价，但写入到调用方已经打开的 FILE*（不负责 open/close）。
 *   供分段引擎在“一个分片只打开一次文件句柄”的磁盘缓存模式下复用句柄，避免每块重复开关文件。
 */
NetDownloadResult network_download_range_fp(const NetDownloadTask *task, FILE *fp);

/*
 * network_probe()
 *   一次性 HEAD 探测：单请求同时获取文件大小与 Range 支持情况，
 *   将原来的 2 次 RTT 合并为 1 次（2× HEAD → 1× HEAD）。
 *   内部仅发起一次 do_http_request，同时回写 content_length 与响应头。
 */
typedef struct {
    int     success;        /* 1 = 探测成功 */
    int64_t file_size;      /* 字节，未知为 -1 */
    int     supports_range; /* 1 = 支持分片，0 = 不支持 */
    long    http_code;
    char    suggested_filename[256]; /* 服务器 Content-Disposition 建议的文件名（可能为空白） */
    char    http_version[16];      /* 实际协商到的协议版本："HTTP/1.1" / "HTTP/2"（HEAD 探测） */
} NetworkProbe;

NetworkProbe network_probe(const char *url, const NetOptions *opt);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_H */
