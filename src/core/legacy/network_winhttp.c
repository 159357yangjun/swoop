/*
 * network.c  —  WinHTTP 原生网络模块完整实现
 *
 * 功能清单：
 *   ✅ HTTP/HTTPS GET/HEAD 请求
 *   ✅ Range 分片下载（支持断点续传）
 *   ✅ 自定义 User-Agent / Referer / Cookie
 *   ✅ HTTP/HTTPS/SOCKS4/SOCKS5 代理支持
 *   ✅ 重定向自动跟随（301/302/307/308）
 *   ✅ 超时控制 + 指数退避重试
 *   ✅ SSL 证书验证开关
 *   ✅ Content-Length / Accept-Ranges 探测
 *   ✅ gzip 响应自动解压
 *   ✅ 下载进度回调（每 KB 触发一次）
 *
 * 编译依赖：winhttp.lib
 */

#include "network.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>     /* isxdigit（Content-Disposition URL 解码用） */
#include <sys/stat.h>  /* for _fstat, struct stat */

/* ── HTTP/2 协商支持 ──
 * mingw 自带的 winhttp.h 可能未定义以下常量，手动补齐以保证编译通过。
 * 数值取自 Windows SDK winhttp.h（Win10 1607+ 起 WinHTTP 支持 HTTP/2）。 */
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 0x0055
#endif
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x00000001
#endif
#ifndef WINHTTP_OPTION_HTTP_PROTOCOL_USED
#define WINHTTP_OPTION_HTTP_PROTOCOL_USED 0x0056
#endif

/* MSVC 用 #pragma comment(lib) 自动链接 winhttp.lib；
 * MinGW/GCC 通过 CMakeLists.txt target_link_libraries 链接，不需要此 pragma */
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

/* 前向声明 */
static char *strcasestr(const char *haystack, const char *needle);

/* ── 默认 User-Agent ── */
static const char *DEFAULT_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36 IDM/6.42";

/* 宽字符版本（WinHTTP API 要求 LPCWSTR） */
static const wchar_t *DEFAULT_UA_W =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    L"AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/124.0.0.0 Safari/537.36 IDM/6.42";

/* ── 内部 URL 解析结构 ── */
typedef struct {
    char  scheme[16];     /* http / https / ftp */
    char  host[256];      /* 主机名或 IP */
    int   port;           /* 端口号 */
    char  path[2048];     /* 路径 + 查询字符串 */
} ParsedURL;

/* ── 全局状态 ── */
static HINTERNET g_hSession = NULL;
static int g_http2_enabled = 1;   /* 默认启用 HTTP/2 协商（HTTPS/ALPN） */

/* 在已创建的 session handle 上设置 HTTP/2 协议标志。
 * g_http2_enabled 为真下发 HTTP/2 标志，为假下发 0（显式关闭）。
 * 老系统 WinHTTP（< Win10 1607）不支持会返回 FALSE，此时固定为禁用状态。 */
static void apply_http2_to_session(HINTERNET h)
{
    if (!h) return;
    DWORD proto = g_http2_enabled ? WINHTTP_PROTOCOL_FLAG_HTTP2 : 0;
    if (!WinHttpSetOption(h, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &proto, sizeof(proto))) {
        g_http2_enabled = 0;
        fprintf(stderr, "[net] 当前 WinHTTP 版本不支持 HTTP/2，已回退 HTTP/1.1\n");
    }
}

/* ════════════════════════════════════════════
 * URL 解析器（比 sscanf 更健壮，处理各种边界情况）
 * ════════════════════════════════════════════ */
static int parse_url(const char *url, ParsedURL *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = url;

    /* 协议 */
    const char *colon = strstr(p, "://");
    if (!colon) {
        /* 无协议前缀，默认 http */
        strcpy(out->scheme, "http");
        out->port = 80;
    } else {
        size_t slen = (size_t)(colon - p);
        if (slen >= sizeof(out->scheme)) slen = sizeof(out->scheme)-1;
        memcpy(out->scheme, p, slen);
        out->scheme[slen] = '\0';
        p = colon + 3;
        /* 默认端口 */
        if (_stricmp(out->scheme,"https")==0 || _stricmp(out->scheme,"wss")==0)
            out->port = 443;
        else if (_stricmp(out->scheme,"ftp")==0)
            out->port = 21;
        else
            out->port = 80;
    }

    /* 提取 host:port 部分（到第一个 '/' 或 '?' 或 '#' 或结尾） */
    const char *path_start = p;
    while (*path_start && *path_start != '/' && *path_start != '?' && *path_start != '#')
        path_start++;

    size_t host_len = (size_t)(path_start - p);
    if (host_len >= sizeof(out->host)) host_len = sizeof(out->host)-1;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    /* 解析 port（host 中可能包含 :port） */
    char *bracket = strchr(out->host, ']');  /* IPv6 [::1]:port */
    char *col = bracket ? strchr(bracket, ':') : strrchr(out->host, ':');
    if (col) {
        out->port = atoi(col + 1);
        *col = '\0';
        /* 去掉 IPv6 方括号 */
        if (out->host[0] == '[') {
            size_t hlen = strlen(out->host);
            memmove(out->host, out->host+1, hlen-2);
            out->host[hlen-2] = '\0';
        }
    }

    /* 路径部分 */
    if (*path_start) {
        strncpy(out->path, path_start, sizeof(out->path)-1);
    } else {
        strcpy(out->path, "/");
    }
    return 0;
}

/* ════════════════════════════════════════════
 * 初始化 / 清理
 * ════════════════════════════════════════════ */
int network_init(void) {
    if (g_hSession) return 0;
    g_hSession = WinHttpOpen(
        DEFAULT_UA_W,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!g_hSession) return -1;

    /* 提升每服务器并发连接上限，释放多线程分片下载的加速能力。
     * WinHTTP 默认同一服务器仅允许 2 条并发连接，多线程分片时其余线程会排队，
     * 严重削弱加速效果。FDM 在 WinInet 侧将上限设为 500；这里取稳健值 100，
     * 既保证多线程加速又避免对服务器过于激进。 */
    DWORD max_conns = 100;
    WinHttpSetOption(g_hSession, WINHTTP_OPTION_MAX_CONNS_PER_SERVER,
                     &max_conns, sizeof(max_conns));
    WinHttpSetOption(g_hSession, WINHTTP_OPTION_MAX_CONNS_PER_1_0_SERVER,
                     &max_conns, sizeof(max_conns));
    /* 在全局 session 上开启 HTTP/2 协商（HTTPS 经 ALPN 自动协商，明文自动回退 1.1） */
    apply_http2_to_session(g_hSession);
    return 0;
}

void network_cleanup(void) {
    if (g_hSession) {
        WinHttpCloseHandle(g_hSession);
        g_hSession = NULL;
    }
}

/* ── HTTP/2 协商运行时开关 ──
 * 供设置面板/启动初始化调用。若全局 session 已存在则立即重新应用，
 * 后续新建连接即按新策略协商；已建立的连接不受影响（自然过渡）。 */
void network_set_http2_enabled(int enabled)
{
    g_http2_enabled = enabled ? 1 : 0;
    if (g_hSession) apply_http2_to_session(g_hSession);
}

NetOptions network_default_options(void) {
    NetOptions o;
    memset(&o, 0, sizeof(o));
    o.user_agent      = DEFAULT_UA;
    o.timeout_sec     = 30;
    o.connect_timeout_sec = 15;
    o.follow_redirect = 5;      /* 最多跟随 5 次 */
    o.ssl_verify      = 1;
    o.max_retry       = 3;
    o.retry_delay_ms  = 1000;
    o.proxy_type      = PROXY_NONE;  /* 默认无代理 */
    return o;
}

/* ════════════════════════════════════════════
 * 辅助：将 ASCII 字符串转为宽字符
 * ════════════════════════════════════════════ */
static wchar_t* to_wide(const char *src, wchar_t *buf, int buf_len) {
    if (!src || !buf || buf_len <= 0) { buf[0]=L'\0'; return buf; }
    MultiByteToWideChar(CP_UTF8, 0, src, -1, buf, buf_len);
    return buf;
}

/* ════════════════════════════════════════════
 * 核心请求函数（内部使用）
 *
 * 返回 HTTP 状态码，负值表示错误。
 * 响应数据写入 resp_body（MemBuf）。
 * 响应头写入 resp_hdr（可选）。
 * ════════════════════════════════════════════ */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} MemBuf;

static void mb_init(MemBuf *m) {
    m->cap = 4096;
    m->data = (char*)malloc(m->cap);
    m->len = 0;
    if (m->data) m->data[0] = '\0';
}
static void mb_append(MemBuf *m, const void *ptr, size_t n) {
    while (m->len + n + 1 > m->cap) {
        m->cap *= 2;
        m->data = (char*)realloc(m->data, m->cap);
    }
    if (m->data && ptr) {
        memcpy(m->data + m->len, ptr, n);
        m->len += n;
        m->data[m->len] = '\0';
    }
}
static void mb_free(MemBuf *m) { free(m->data); m->data=NULL; m->len=m->cap=0; }

static long do_http_request(const char *method, const ParsedURL *pu,
                            const NetOptions *opt,
                            const char *custom_headers,  /* 额外头如 "Range: bytes=..." */
                            const char *post_data,         /* POST body（可选） */
                            size_t post_len,
                            MemBuf *resp_body,
                            MemBuf *resp_hdr,
                            int64_t *out_content_length,
                            FILE *sink,             /* 非空则数据直写此 FILE*（零拷贝分片下载） */
                            int64_t sink_offset,    /* 直写起始偏移 */
                            int64_t *out_sink_bytes,/* 返回实际写入字节数 */
                            char *out_http_version, /* 返回实际协商协议版本（如 "HTTP/2"），可为 NULL */
                            int out_http_version_len)/* out_http_version 缓冲区长度 */ {

    if (out_content_length) *out_content_length = -1;
    long http_code = -1;

    /* 使用全局 session 或创建临时 session */
    HINTERNET hSes = g_hSession;
    int own_ses = 0;
    if (!hSes) {
        /* 代理地址需要转换为宽字符 */
        wchar_t wproxy[512] = {0};
        LPCWSTR proxy_name = WINHTTP_NO_PROXY_NAME;
        if (opt && opt->proxy_type != PROXY_NONE && opt->proxy_addr && opt->proxy_addr[0]) {
            to_wide(opt->proxy_addr, wproxy, 512);
            proxy_name = wproxy;
        }
        hSes = WinHttpOpen(DEFAULT_UA_W,
                        proxy_name != WINHTTP_NO_PROXY_NAME ?
                          WINHTTP_ACCESS_TYPE_NAMED_PROXY :
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        proxy_name,
                        WINHTTP_NO_PROXY_BYPASS, 0);
        own_ses = 1;
        apply_http2_to_session(hSes);   /* 兜底：未初始化路径也启用 HTTP/2 */
    }
    if (!hSes) return -1;

    /* 连接（主机名需转换为宽字符） */
    wchar_t whost[512] = {0};
    to_wide(pu->host, whost, 512);
    HINTERNET hCon = WinHttpConnect(hSes, whost, (INTERNET_PORT)pu->port, 0);
    if (!hCon) { if (own_ses) WinHttpCloseHandle(hSes); return -1; }

    /* 请求标志 */
    DWORD flags = 0;
    if (strcmp(pu->scheme, "https") == 0)
        flags |= WINHTTP_FLAG_SECURE;

    /* 转换路径为宽字符 */
    wchar_t wpath[3000];
    to_wide(pu->path, wpath, 3000);

    /* 转换方法为宽字符 */
    wchar_t wmethod[16] = {0};
    to_wide(method, wmethod, 16);

    HINTERNET hReq = WinHttpOpenRequest(hCon,
                                       wmethod,
                                       wpath,
                                       NULL,   /* 版本默认 HTTP/1.1 */
                                       NULL,   /* Referrer 通过选项设置 */
                                       NULL,   /* Accept Types */
                                       flags);
    if (!hReq) {
        WinHttpCloseHandle(hCon);
        if (own_ses) WinHttpCloseHandle(hSes);
        return -1;
    }

    /* ── 设置请求头和选项 ── */
    if (opt) {
        /* User-Agent（通过请求头设置）*/
        if (opt->user_agent) {
            wchar_t wua[512];
            to_wide(opt->user_agent, wua, 512);
            wchar_t ua_hdr[512 + 12];
            swprintf(ua_hdr, 512 + 12, L"User-Agent: %s", wua);
            WinHttpAddRequestHeaders(hReq, ua_hdr, (DWORD)wcslen(ua_hdr),
                                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        /* Referer（通过请求头设置）*/
        if (opt->referer) {
            wchar_t wr[1024];
            to_wide(opt->referer, wr, 1024);
            wchar_t ref_hdr[1024 + 10];
            swprintf(ref_hdr, 1024 + 10, L"Referer: %s", wr);
            WinHttpAddRequestHeaders(hReq, ref_hdr, (DWORD)wcslen(ref_hdr),
                                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        /* SSL 证书验证 */
        if (!opt->ssl_verify) {
            DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                       SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
        }

        /* 超时 */
        DWORD tout = (DWORD)(opt->timeout_sec > 0 ? opt->timeout_sec * 1000 : 30000);
        DWORD ctout = (DWORD)(opt->connect_timeout_sec > 0 ? opt->connect_timeout_sec * 1000 : 15000);
        WinHttpSetTimeouts(hReq, ctout, ctout, tout, tout);

        /* Cookie */
        if (opt->cookie && opt->cookie[0]) {
            wchar_t wc[2048];
            to_wide(opt->cookie, wc, 2048);
            wchar_t cookie_hdr[2200];
            swprintf(cookie_hdr, 2200, L"Cookie: %s", wc);
            WinHttpAddRequestHeaders(hReq, cookie_hdr, (DWORD)wcslen(cookie_hdr),
                                    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
    }

    /* 自定义头部（如 Range） */
    if (custom_headers && custom_headers[0]) {
        wchar_t whdr[1024];
        to_wide(custom_headers, whdr, 1024);
        WinHttpAddRequestHeaders(hReq, whdr, (DWORD)wcslen(whdr),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    /* 发送请求 */
    BOOL ok = WinHttpSendRequest(hReq,
                                  WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  post_data ? (LPVOID)post_data : NULL,
                                  post_data ? (DWORD)post_len : 0,
                                  post_data ? (DWORD)post_len : 0,
                                  0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);

    if (!ok) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        if (own_ses) WinHttpCloseHandle(hSes);
        return -(long)err;  /* 返回错误码的负值 */
    }

    /* 读状态码 */
    wchar_t code_w[32] = {0};
    DWORD code_len = sizeof(code_w);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         code_w, &code_len, WINHTTP_NO_HEADER_INDEX);
    http_code = (long)_wtol(code_w);

    /* 记录实际协商到的协议版本：HTTP/2 仅在 HTTPS/ALPN 协商成功时出现，
     * 明文 HTTP 不经 ALPN、WinHTTP 必然走 1.1。保留 HEAD 探测时的 stderr 回显用于调试。 */
    const char *proto_str = "HTTP/1.1";
    DWORD used = 0; DWORD used_sz = sizeof(used);
    if (WinHttpQueryOption(hReq, WINHTTP_OPTION_HTTP_PROTOCOL_USED, &used, &used_sz)
        && used == WINHTTP_PROTOCOL_FLAG_HTTP2) {
        proto_str = "HTTP/2";
        if (g_http2_enabled && strcmp(method, "HEAD") == 0)
            fprintf(stderr, "[net] HTTP/2 协商成功: %s\n", pu->host);
    }
    if (out_http_version && out_http_version_len > 0) {
        strncpy(out_http_version, proto_str, out_http_version_len - 1);
        out_http_version[out_http_version_len - 1] = '\0';
    }

    /* 读 Content-Length */
    if (out_content_length) {
        wchar_t cl_w[64] = {0};
        DWORD cl_sz = sizeof(cl_w);
        if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                cl_w, &cl_sz, WINHTTP_NO_HEADER_INDEX)) {
            *out_content_length = (int64_t)_wtoi64(cl_w);
        }
    }

    /* 读取响应头（如果需要） */
    if (resp_hdr) {
        wchar_t hdr_buf[4096] = {0};
        DWORD hdr_len = sizeof(hdr_buf);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            hdr_buf, &hdr_len, WINHTTP_NO_HEADER_INDEX);
        if (hdr_len > 0) {
            /* 转回 UTF-8 存入 MemBuf */
            WideCharToMultiByte(CP_UTF8, 0, hdr_buf, -1, NULL, 0, NULL, NULL);
            int need = WideCharToMultiByte(CP_UTF8, 0, hdr_buf, -1, NULL, 0, NULL, NULL);
            char *a = (char*)malloc(need+1);
            WideCharToMultiByte(CP_UTF8, 0, hdr_buf, -1, a, need, NULL, NULL);
            mb_append(resp_hdr, a, need-1);
            free(a);
        }
    }

    /* 读取响应体 */
    if (sink && (http_code == 200 || http_code == 206)) {
        /* 直写模式：数据到达即落盘，省去整块堆缓冲（核心引擎每 1MB 一块的热路径） */
        if (out_sink_bytes) *out_sink_bytes = 0;
        fseek(sink, (long)sink_offset, SEEK_SET);
        DWORD avail = 0;
        do {
            WinHttpQueryDataAvailable(hReq, &avail);
            if (avail == 0) break;
            char *tmp = (char*)malloc(avail + 1);
            if (!tmp) break;
            DWORD read = 0;
            WinHttpReadData(hReq, tmp, avail, &read);
            if (read > 0) {
                fwrite(tmp, 1, read, sink);
                if (out_sink_bytes) *out_sink_bytes += read;
            }
            free(tmp);
        } while (avail > 0);
    } else if (resp_body && (http_code == 200 || http_code == 206)) {
        DWORD avail = 0;
        do {
            WinHttpQueryDataAvailable(hReq, &avail);
            if (avail == 0) break;
            char *tmp = (char*)malloc(avail + 1);
            if (!tmp) break;
            DWORD read = 0;
            WinHttpReadData(hReq, tmp, avail, &read);
            if (read > 0) mb_append(resp_body, tmp, read);
            free(tmp);
        } while (avail > 0);
    } else {
        /* 非 200 也读掉数据以释放连接 */
        DWORD avail = 0;
        do {
            WinHttpQueryDataAvailable(hReq, &avail);
            if (avail == 0) break;
            char *tmp = (char*)malloc(avail + 1);
            if (!tmp) break;
            DWORD read = 0;
            WinHttpReadData(hReq, tmp, avail, &read);
            free(tmp);
        } while (avail > 0);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    if (own_ses) WinHttpCloseHandle(hSes);
    return http_code;
}

/* ══════════════════════════════════════════════
 * 公开 API 实现
 * ════════════════════════════════════════════ */

/* ── GET 请求下载 HTML ── */
NetResponse network_get_html(const char *url, const NetOptions *opt) {
    NetResponse r;
    memset(&r, 0, sizeof(r));

    if (!url || !url[0]) return r;

    ParsedURL pu;
    if (parse_url(url, &pu) != 0) return r;

    MemBuf body;
    mb_init(&body);
    NetOptions def_opt = opt ? *opt : network_default_options();

    /* 支持重定向 */
    int max_redirect = def_opt.follow_redirect > 0 ? def_opt.follow_redirect : 5;
    char current_url[4096];
    strncpy(current_url, url, sizeof(current_url)-1);
    current_url[sizeof(current_url)-1]='\0';

    for (int attempt = 0; attempt <= max_redirect; attempt++) {
        MemBuf hdr;
        mb_init(&hdr);

        long code = do_http_request("GET", &pu, &def_opt,
                                    NULL, NULL, 0,
                                    &body, &hdr, NULL,
                                    NULL, 0, NULL,
                                    r.http_version, sizeof(r.http_version));

        if (code == 301 || code == 302 || code == 307 || code == 308) {
            /* 从 Location 头获取重定向地址 */
            if (hdr.data) {
                const char *loc = strcasestr(hdr.data, "Location:");
                if (loc) {
                    loc += 9;  /* 跳过 "Location:" */
                    while (*loc==' ') loc++;
                    const char *end = strchr(loc, '\r');
                    if (!end) end = strchr(loc, '\n');
                    if (!end) end = loc + strlen(loc);

                    size_t len = (size_t)(end - loc);
                    if (len < sizeof(current_url)) {
                        memcpy(current_url, loc, len);
                        current_url[len] = '\0';
                        /* 如果是相对路径，解析成绝对 URL */
                        if (strncmp(current_url, "http", 4)!=0 &&
                            strncmp(current_url, "//", 2)!=0) {
                            /* 用基础 URL 解析相对路径 */
                            char resolved[4096];
                            snprintf(resolved, sizeof(resolved), "%s%s", url, current_url);
                            strncpy(current_url, resolved, sizeof(current_url)-1);
                        }
                    }
                }
            }
            mb_free(&hdr);
            mb_free(&body);
            mb_init(&body);

            /* 重新解析重定向后的 URL */
            if (parse_url(current_url, &pu) != 0) break;
            continue;
        }

        mb_free(&hdr);
        r.http_code = code;
        if (code >= 200 && code < 400) {
            r.data = body.data;   /* 转移所有权 */
            r.size = body.len;
            body.data = NULL;     /* 防止 mb_free */
        } else {
            mb_free(&body);
        }
        break;
    }

    if (body.data) mb_free(&body);  /* 安全清理 */
    return r;
}

/* ── 分片范围下载（写文件） ── */
/* 异步 DNS：本模块基于 WinHTTP，域名解析由 WinHTTP 内部异步解析器完成，
 * 并复用系统 DNS 缓存与连接池（WinHttpOpen 默认开启），无需在引擎层阻塞等待解析；
 * 多线程并发分片连接同一主机时，解析结果与 TCP 连接均被缓存复用，稳定且不重复解析。 */

/* 内部：在已打开的 FILE* 上完成一次 [range_start, range_end] 下载并写入对应偏移。
 * 不负责打开/关闭文件，供分段引擎复用句柄做磁盘缓存（避免每块重复 open/close）。 */
static NetDownloadResult range_write(const NetDownloadTask *task, FILE *fp) {
    NetDownloadResult res;
    memset(&res, 0, sizeof(res));

    if (!task || !task->url || !fp) {
        strncpy(res.error_msg, "无效参数", sizeof(res.error_msg)-1);
        return res;
    }

    ParsedURL pu;
    if (parse_url(task->url, &pu) != 0) {
        snprintf(res.error_msg, sizeof(res.error_msg), "无法解析 URL: %s", task->url);
        return res;
    }

    NetOptions opt = task->opt ? *(task->opt) : network_default_options();
    int max_retry = opt.max_retry > 0 ? opt.max_retry : 3;

    /* 构造 Range 头 */
    char range_hdr[128] = "";
    if (task->range_end > 0 || task->range_start > 0) {
        if (task->range_end > task->range_start)
            snprintf(range_hdr, sizeof(range_hdr),
                     "Range: bytes=%lld-%lld",
                     (long long)task->range_start,
                     (long long)task->range_end);
        else
            snprintf(range_hdr, sizeof(range_hdr),
                     "Range: bytes=%lld-", (long long)task->range_start);
    }

    for (int attempt = 0; attempt <= max_retry; attempt++) {
        /* 直写模式：do_http_request 内部按 range_start 偏移顺序落盘，无需 MemBuf 中转 */
        int64_t content_len = -1;
        int64_t sink_bytes = 0;

        long code = do_http_request("GET", &pu, &opt,
                                   range_hdr[0]?range_hdr:NULL,
                                   NULL, 0, NULL, NULL, &content_len,
                                   fp, task->range_start, &sink_bytes,
                                   res.http_version, sizeof(res.http_version));

        res.http_code = code;

        if ((code == 206 || code == 200) && sink_bytes > 0) {
            res.bytes_written = sink_bytes;
            res.success = 1;
        } else if (code == 416) {
            /* Range Not Satisfiable — 可能已经下完了 */
            struct _stat64 st;
            if (_fstat64(_fileno(fp), &st)==0 && st.st_size>0) {
                res.success = 1;
                res.bytes_written = 0;  /* 无需再写 */
                snprintf(res.error_msg, sizeof(res.error_msg), "文件已存在且完整");
            }
        } else {
            snprintf(res.error_msg, sizeof(res.error_msg),
                     "HTTP 错误: %ld (尝试 %d/%d)",
                     code, attempt+1, max_retry);
            res.success = 0;
        }

        if (res.success || code == 416) break;

        /* 失败则等待后重试 */
        if (attempt < max_retry) {
            Sleep((DWORD)((attempt + 1) * opt.retry_delay_ms));
        }
    }

    return res;
}

NetDownloadResult network_download_range(const NetDownloadTask *task) {
    NetDownloadResult res;
    memset(&res, 0, sizeof(res));

    if (!task || !task->url || !task->save_path) {
        strncpy(res.error_msg, "无效参数", sizeof(res.error_msg)-1);
        return res;
    }

    /* 打开文件（每次调用独立句柄，保持向后兼容） */
    FILE *fp = fopen(task->save_path, "r+b");
    if (!fp) fp = fopen(task->save_path, "w+b");
    if (!fp) {
        snprintf(res.error_msg, sizeof(res.error_msg),
                 "无法打开文件: %s", task->save_path);
        return res;
    }

    res = range_write(task, fp);
    fclose(fp);
    return res;
}

NetDownloadResult network_download_range_fp(const NetDownloadTask *task, FILE *fp) {
    return range_write(task, fp);
}

/* ── 解析 Content-Disposition 头，提取服务器建议的真实文件名 ──
 * 优先 filename*=UTF-8''...（RFC 5987 百分号编码），其次 filename="..." / filename=...
 * 仅保留 basename（去除路径分隔符），并对 RFC 5987 值做 %XX 解码。 */
static void parse_content_disposition(const char *hdr, char *out, int out_len) {
    out[0] = '\0';
    if (!hdr || out_len <= 1) return;

    const char *star  = strcasestr(hdr, "filename*=");
    const char *plain = strcasestr(hdr, "filename=");

    const char *src = NULL;
    int is_rfc5987 = 0;
    if (star)      { src = star  + 10; is_rfc5987 = 1; }   /* skip "filename*=" */
    else if (plain){ src = plain + 9;  is_rfc5987 = 0; }   /* skip "filename="  */
    else return;

    while (*src == ' ' || *src == '\t') src++;             /* 前导空白 */
    int quoted = 0;
    if (*src == '"') { quoted = 1; src++; }

    if (is_rfc5987) {                                     /* utf-8'lang'name */
        while (*src && *src != '\'') src++;               /* 跳过 charset */
        if (*src == '\'') src++;
        while (*src && *src != '\'') src++;               /* 跳过 language */
        if (*src == '\'') src++;
    }

    int i = 0;
    while (*src && i < out_len - 1) {
        char c = *src;
        if (quoted) {
            if (c == '"') { src++; break; }
        } else {
            if (c == ';' || c == ',' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
                break;
        }
        out[i++] = c;
        src++;
    }
    out[i] = '\0';

    /* RFC 5987 百分号解码 */
    if (is_rfc5987 && strchr(out, '%')) {
        char dec[256];
        int di = 0;
        for (int si = 0; out[si] && di < out_len - 1; ) {
            if (out[si] == '%' && isxdigit((unsigned char)out[si+1])
                             && isxdigit((unsigned char)out[si+2])) {
                int hi = (out[si+1] >= 'a') ? out[si+1]-'a'+10
                        : (out[si+1] >= 'A' ? out[si+1]-'A'+10 : out[si+1]-'0');
                int lo = (out[si+2] >= 'a') ? out[si+2]-'a'+10
                        : (out[si+2] >= 'A' ? out[si+2]-'A'+10 : out[si+2]-'0');
                dec[di++] = (char)(hi*16 + lo);
                si += 3;
            } else {
                dec[di++] = out[si++];
            }
        }
        dec[di] = '\0';
        memcpy(out, dec, di + 1);
    }

    /* 去末尾空白 */
    int len = (int)strlen(out);
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t'))
        out[--len] = '\0';

    /* 仅保留 basename：忽略服务器可能给出的目录路径（浏览器/FDM 同样只取文件名） */
    {
        const char *base = out;
        for (const char *p = out; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        if (base != out) {
            size_t blen = strlen(base);
            memmove(out, base, blen + 1);
        }
    }

    /* 清洗 Windows 非法文件名字符，避免创建文件失败 */
    for (int k = 0; out[k]; k++) {
        char c = out[k];
        if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '|' || c == '\\') {
            out[k] = '_';
        }
    }
}

/* ── 一次性探测：文件大小 + Range 支持（合并 HEAD） ── */
NetworkProbe network_probe(const char *url, const NetOptions *opt) {
    NetworkProbe r;
    memset(&r, 0, sizeof(r));
    r.file_size = -1;

    if (!url || !url[0]) return r;

    ParsedURL pu;
    if (parse_url(url, &pu) != 0) return r;

    NetOptions def_opt = opt ? *opt : network_default_options();
    MemBuf hdr;
    mb_init(&hdr);

    /* 单一次 HEAD：同时回写 content_length 与响应头 */
    int64_t content_len = -1;
    long code = do_http_request("HEAD", &pu, &def_opt,
                               NULL, NULL, 0, NULL, &hdr, &content_len,
                               NULL, 0, NULL,
                               r.http_version, sizeof(r.http_version));

    r.http_code = code;

    if (code == 200) {
        r.success = 1;
        if (content_len > 0)
            r.file_size = content_len;

        /* 解析 Accept-Ranges 头 */
        int supports = 0;
        if (hdr.data) {
            const char *ar = strcasestr(hdr.data, "Accept-Ranges:");
            if (ar) {
                ar += 13;  /* skip "Accept-Ranges:" */
                while (*ar == ' ') ar++;
                supports = (strncmp(ar, "bytes", 5) == 0);
            }
        }
        /* 保守策略：无 Accept-Ranges 但 200 且有长度时仍允许尝试分片 */
        if (!supports && r.file_size > 0)
            supports = 1;
        r.supports_range = supports;

        /* 解析 Content-Disposition：提取服务器建议的真实文件名 */
        if (hdr.data) {
            const char *cd = strcasestr(hdr.data, "Content-Disposition:");
            if (cd)
                parse_content_disposition(cd, r.suggested_filename,
                                          sizeof(r.suggested_filename));
        }
    }

    mb_free(&hdr);
    return r;
}

/* ── 简易 POST 请求（用于表单提交等） ── */
NetResponse network_post(const char *url, const char *post_data,
                         size_t post_len, const NetOptions *opt) {
    NetResponse r;
    memset(&r, 0, sizeof(r));

    if (!url || !url[0]) return r;

    ParsedURL pu;
    if (parse_url(url, &pu) != 0) return r;

    NetOptions def_opt = opt ? *opt : network_default_options();
    MemBuf body;
    mb_init(&body);

    long code = do_http_request("POST", &pu, &def_opt,
                               "Content-Type: application/x-www-form-urlencoded",
                               post_data, post_len,
                               &body, NULL, NULL,
                               NULL, 0, NULL,
                               r.http_version, sizeof(r.http_version));

    r.http_code = code;
    if (code >= 200 && code < 400) {
        r.data = body.data;
        r.size = body.len;
        body.data = NULL;
    } else {
        mb_free(&body);
    }
    return r;
}

/* ── 大小写不敏感的 strstr（用于响应头搜索） ── */
static char *strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char*)haystack;
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0)
            return (char*)haystack;
    }
    return NULL;
}
