#ifndef IDM_HTTP_H
#define IDM_HTTP_H
#include <windows.h>

typedef struct {
    char      url[2048];
    long long content_length; /* -1 = 未知 */
    int       supports_range;  /* 0/1 */
    int       status;          /* HTTP 状态码，0 = 未知 */
} http_resource_t;

/* 初始化 WinHTTP（动态加载 winhttp.dll）。access_type 传 WINHTTP_ACCESS_TYPE_NO_PROXY
   可绕过代理（自测本地服务用）。返回 0 成功，-1 失败。 */
int  http_init(int access_type);
void http_cleanup(void);

/* 探测资源：大小 + 是否支持 Range 分片。
   返回 0 成功（且状态码为 2xx）；-1 网络失败；-2 服务端返回非 2xx（out->status 有码）。 */
int  http_probe(const char *url, http_resource_t *out);

/* 下载 [offset, offset+len) 区间（len<=0 表示从头下到结束）。
   referer 可为 NULL/空（防盗链站点需要它）。
   write_cb 每收到一块调用一次，返回 0 表示中止；progress 可选。
   返回实际写入字节数，或 -1 出错（含非 2xx 响应，此时不写任何字节）。 */
long long http_download(const char *url, const wchar_t *referer,
                        long long offset, long long len,
                        int (*write_cb)(void *ctx, const void *data, long long n),
                        void (*progress)(void *ctx, long long got),
                        void *ctx);

#endif
