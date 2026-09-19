/*
 * network_curl.c — libcurl 网络模块实现（替换原 WinHTTP 的 network.c）
 *
 * 接口与 network.h 完全一致：download_core.c / main_window.cpp / settings_dialog.cpp
 * 等调用方无需任何改动。仅传输层由 WinHTTP 换成 libcurl。
 *
 * 能力对照（相对原 WinHTTP 实现）：
 *   ✅ HTTP/HTTPS GET/HEAD/POST
 *   ✅ Range 分片下载（断点续传）
 *   ✅ UA / Referer / Cookie
 *   ✅ HTTP/HTTPS/SOCKS4/SOCKS5 代理
 *   ✅ 重定向自动跟随（CURLOPT_FOLLOWLOCATION，含相对/绝对路径解析）
 *   ✅ 超时 + 指数退避重试
 *   ✅ SSL 证书校验开关
 *   ✅ Content-Length / Accept-Ranges 探测（HEAD）
 *   ✅ gzip/br/deflate 自动解压（仅 HTML/POST；分片下载保持原始字节）
 *   ✅ HTTP/2（CURLOPT_HTTP_VERSION=2TLS，HTTPS 经 ALPN 协商，明文自动回退 1.1）
 *
 * 依赖：项目第三方库 third_party/libcurl（curl 8.22.0 官方 MinGW-w64 构建），编译链接 libcurl。
 *       详见 third_party/libcurl/README.md。
 */

#include "network.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <windows.h>   /* Sleep（重试退避） */

/* ── 默认 User-Agent ── */
static const char *DEFAULT_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36 IDM/6.42";

/* ── 全局状态 ── */
static int g_http2_enabled = 1;
static int g_curl_init = 0;

/* ── 前向声明 ── */
static char *strcasestr(const char *haystack, const char *needle);
static void parse_content_disposition(const char *hdr, char *out, int out_len);

/* ── UTF-8 路径安全打开文件 ──
 * Windows 下 fopen 按 ANSI 代码页解释路径，传入 UTF-8 字节的中文文件名会被
 * 变成乱码文件名。这里统一转成 UTF-16 走 _wfopen，保证中文/非 ASCII 路径正确。 */
static FILE *utf8_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024];
    wchar_t wmode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return NULL;
    if (MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 16) <= 0) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* ════════════════════════════════════════════
 * 初始化 / 清理
 * ══════════════════════════════════════════ */
int network_init(void) {
    if (g_curl_init) return 0;
    /* CURL_GLOBAL_DEFAULT = 初始化 SSL（LibreSSL）+ Win32 套接字。
     * 多线程并发调用 curl_easy_perform 是安全的（每个 easy handle 独立）。 */
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) return -1;
    g_curl_init = 1;
    return 0;
}

void network_cleanup(void) {
    if (g_curl_init) {
        curl_global_cleanup();
        g_curl_init = 0;
    }
}

/* ── HTTP/2 协商运行时开关（供设置面板/启动初始化调用）── */
void network_set_http2_enabled(int enabled) {
    g_http2_enabled = enabled ? 1 : 0;
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
    o.proxy_type      = PROXY_NONE;
    return o;
}

/* ════════════════════════════════════════════
 * 内存缓冲区 / 写回调
 * ══════════════════════════════════════════ */
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
static void mb_free(MemBuf *m) { free(m->data); m->data = NULL; m->len = m->cap = 0; }

/* 分片下载：数据到达即顺序落盘（调用方已 fseek 到 range_start） */
typedef struct {
    FILE   *fp;
    int64_t written;
} RangeCtx;

static size_t write_file_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    RangeCtx *c = (RangeCtx*)userdata;
    size_t total = size * nmemb;
    if (total > 0) {
        size_t w = fwrite(ptr, 1, total, c->fp);
        c->written += (int64_t)w;
        if (w != total) return w;   /* 写盘失败，通知 libcurl 中止 */
    }
    return total;
}

/* 内存响应体（HTML/POST） */
static size_t write_mem_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    mb_append((MemBuf*)userdata, ptr, size * nmemb);
    return size * nmemb;
}

/* 响应头捕获（probe 解析 Accept-Ranges / Content-Disposition 用） */
static size_t write_hdr_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    mb_append((MemBuf*)userdata, ptr, size * nmemb);
    return size * nmemb;
}

/* 把 libcurl 协商到的协议版本映射为字符串 */
static void proto_string(long v, char *out, int outlen) {
    if (v == CURL_HTTP_VERSION_2)      strncpy(out, "HTTP/2", outlen - 1);
    else if (v == CURL_HTTP_VERSION_3) strncpy(out, "HTTP/3", outlen - 1);
    else                               strncpy(out, "HTTP/1.1", outlen - 1);
    out[outlen - 1] = '\0';
}

/* ── 按 NetOptions 配置一个 easy handle 的公共选项 ── */
static void apply_common_opts(CURL *h, const NetOptions *opt) {
    NetOptions def = opt ? *opt : network_default_options();
    if (def.user_agent)            curl_easy_setopt(h, CURLOPT_USERAGENT, def.user_agent);
    if (def.referer)               curl_easy_setopt(h, CURLOPT_REFERER, def.referer);
    if (def.cookie && def.cookie[0]) curl_easy_setopt(h, CURLOPT_COOKIE, def.cookie);

    /* 代理
     * ⚠️ 关键：**无论用不用代理都要显式调一次 CURLOPT_PROXY**。
     * 不调用时 libcurl 会去读 http_proxy / HTTPS_PROXY / all_proxy 环境变量，
     * 于是界面上选定的「不使用代理」会被环境变量悄悄推翻——用户看到的是
     * 「设置明明选了直连，下载却走代理」，而且失败现象很费解（本机服务返回
     * 401/404，因为代理把请求行换成了绝对 URL）。这类「设置存了盘但没生效」
     * 正是本项目反复排查的坑。
     * libcurl 文档保证：CURLOPT_PROXY 设为空串 = 显式禁用代理，环境变量一律失效。 */
    if (def.proxy_type != PROXY_NONE && def.proxy_addr && def.proxy_addr[0]) {
        curl_easy_setopt(h, CURLOPT_PROXY, def.proxy_addr);
        switch (def.proxy_type) {
            case PROXY_HTTP:   curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);   break;
            case PROXY_SOCKS4: curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS4); break;
            case PROXY_SOCKS5: curl_easy_setopt(h, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5); break;
        }
        /* 代理认证（HTTP/SOCKS 均支持） */
        if (def.proxy_user && def.proxy_user[0])
            curl_easy_setopt(h, CURLOPT_PROXYUSERNAME, def.proxy_user);
        if (def.proxy_pass && def.proxy_pass[0])
            curl_easy_setopt(h, CURLOPT_PROXYPASSWORD, def.proxy_pass);
    } else {
        curl_easy_setopt(h, CURLOPT_PROXY, "");   /* 直连：屏蔽环境变量代理 */
    }

    /* 目标站点 HTTP 认证（设置页「站点登录」）：
     * CURLAUTH_ANY 让 libcurl 先按凭据做一次请求，遇到 401 时按服务端返回的
     * WWW-Authenticate 头自动改用 Basic / Digest / NTLM 等，无需用户选类型。 */
    if (def.auth_user && def.auth_user[0]) {
        curl_easy_setopt(h, CURLOPT_USERNAME, def.auth_user);
        if (def.auth_pass) curl_easy_setopt(h, CURLOPT_PASSWORD, def.auth_pass);
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    }

    /* 超时 */
    if (def.connect_timeout_sec > 0)
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, (long)def.connect_timeout_sec);
    if (def.timeout_sec > 0)
        curl_easy_setopt(h, CURLOPT_TIMEOUT, (long)def.timeout_sec);

    /* SSL 证书校验 */
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, def.ssl_verify ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, def.ssl_verify ? 2L : 0L);

    /* 重定向跟随 */
    if (def.follow_redirect > 0) {
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, (long)def.follow_redirect);
    }

    /* HTTP/2 协商（HTTPS 经 ALPN；明文自动回退 1.1） */
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION,
                     g_http2_enabled ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1);
}

/* ════════════════════════════════════════════
 * 公开 API 实现
 * ══════════════════════════════════════════ */

/* ── GET 下载 HTML ── */
NetResponse network_get_html(const char *url, const NetOptions *opt) {
    NetResponse r;
    memset(&r, 0, sizeof(r));
    if (!url || !url[0]) return r;

    CURL *h = curl_easy_init();
    if (!h) return r;

    MemBuf body; mb_init(&body);
    apply_common_opts(h, opt);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); /* 自动解压 gzip/br/deflate */

    CURLcode rc = curl_easy_perform(h);
    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    long ver  = 0; curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &ver);
    proto_string(ver, r.http_version, sizeof(r.http_version));
    r.http_code = code;

    if (rc == CURLE_OK && code >= 200 && code < 400) {
        r.data = body.data;   /* 转移所有权 */
        r.size = body.len;
        body.data = NULL;
    } else {
        mb_free(&body);
    }
    curl_easy_cleanup(h);
    if (body.data) mb_free(&body);
    return r;
}

/* ── POST ── */
NetResponse network_post(const char *url, const char *post_data,
                         size_t post_len, const NetOptions *opt) {
    NetResponse r;
    memset(&r, 0, sizeof(r));
    if (!url || !url[0]) return r;

    CURL *h = curl_easy_init();
    if (!h) return r;

    MemBuf body; mb_init(&body);
    apply_common_opts(h, opt);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)(post_len ? post_len : (post_data ? strlen(post_data) : 0)));
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, post_data ? post_data : "");
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_mem_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

    CURLcode rc = curl_easy_perform(h);
    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    long ver  = 0; curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &ver);
    proto_string(ver, r.http_version, sizeof(r.http_version));
    r.http_code = code;

    if (rc == CURLE_OK && code >= 200 && code < 400) {
        r.data = body.data;
        r.size = body.len;
        body.data = NULL;
    } else {
        mb_free(&body);
    }
    curl_easy_cleanup(h);
    if (body.data) mb_free(&body);
    return r;
}

/* ── 分片范围下载（写入已打开的 FILE*）── */
static NetDownloadResult range_write(const NetDownloadTask *task, FILE *fp) {
    NetDownloadResult res;
    memset(&res, 0, sizeof(res));
    if (!task || !task->url || !fp) {
        strncpy(res.error_msg, "无效参数", sizeof(res.error_msg) - 1);
        return res;
    }

    NetOptions opt = task->opt ? *(task->opt) : network_default_options();
    int max_retry = opt.max_retry > 0 ? opt.max_retry : 3;

    /* 构造 Range（字节区间，含端点） */
    char range_hdr[128] = "";
    if (task->range_start > 0 || task->range_end > 0) {
        if (task->range_end > task->range_start)
            snprintf(range_hdr, sizeof(range_hdr),
                     "%lld-%lld", (long long)task->range_start, (long long)task->range_end);
        else
            snprintf(range_hdr, sizeof(range_hdr),
                     "%lld-", (long long)task->range_start);
    }

    for (int attempt = 0; attempt <= max_retry; attempt++) {
        CURL *h = curl_easy_init();
        if (!h) {
            strncpy(res.error_msg, "curl_easy_init 失败", sizeof(res.error_msg) - 1);
            break;
        }

        apply_common_opts(h, &opt);
        curl_easy_setopt(h, CURLOPT_URL, task->url);
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
        if (range_hdr[0]) curl_easy_setopt(h, CURLOPT_RANGE, range_hdr);
        /* 分片下载不自动解压：保持原始字节（不设 ACCEPT_ENCODING） */
        /* HTTP >= 400 时不写响应体：否则错误页（如 401 的 "authentication required"）
         * 会被当成文件内容写进下载目标，产出内容是错误页的“成功”文件。
         * 注意：FAILONERROR 不影响 CURLINFO_RESPONSE_CODE，下面的 416 分支照旧可用。 */
        curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);

        RangeCtx ctx; ctx.fp = fp; ctx.written = 0;
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);

        /* 定位到 range_start，后续顺序写入（与 WinHTTP 行为一致）。
         * ⚠️ 必须用 _fseeki64：Windows 上 long 是 32 位，`fseek(fp, (long)range_start, ...)`
         * 对 2GB 以上的文件会把偏移截断（甚至变成负值），分片数据就写到文件的错误位置上，
         * 产出一个大小对得上、内容却错乱的文件，而且状态是「已完成」。 */
        _fseeki64(fp, task->range_start, SEEK_SET);

        CURLcode rc = curl_easy_perform(h);
        long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
        long ver  = 0; curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &ver);
        proto_string(ver, res.http_version, sizeof(res.http_version));
        res.http_code = code;
        curl_easy_cleanup(h);

        /* 服务器无视了 Range、直接回 200 + 整份文件，而我们要的是中段（range_start > 0）：
         * 此时按 range_start 定位再顺序写，等于把「从 0 开始的全量数据」灌进分片偏移处，
         * 最终文件内容整体错位却显示「已完成」。这类服务器在 CDN/防盗链上并不罕见
         * （HEAD 报 Accept-Ranges: bytes，取 Range 时却回 200），必须判失败并说清原因。
         * range_start == 0 时 200 是正常的（内容本来就从 0 开始），不受影响。 */
        if (code == 200 && task->range_start > 0) {
            res.range_ignored = 1;   /* 交给分段引擎降级为单连接，重试无意义 */
            snprintf(res.error_msg, sizeof(res.error_msg),
                     "服务器忽略 Range 请求（返回 200 而非 206）");
            res.success = 0;
            break;
        }

        if ((code == 206 || code == 200) && ctx.written > 0) {
            res.bytes_written = ctx.written;
            res.success = 1;
            break;
        } else if (code == 416) {
            /* Range Not Satisfiable — 可能已下完 */
            struct _stat64 st;
            if (_fstat64(_fileno(fp), &st) == 0 && st.st_size > 0) {
                res.success = 1;
                res.bytes_written = 0;
                strncpy(res.error_msg, "文件已存在且完整", sizeof(res.error_msg) - 1);
            }
            break;
        } else {
            /* 直接把原因写清楚：这是最终会显示到任务行的文案，不带「尝试 n/m」这类噪声。
             * code==0 表示连 HTTP 响应都没拿到，用 libcurl 的错误描述更有用。 */
            if (code > 0)
                snprintf(res.error_msg, sizeof(res.error_msg), "HTTP 错误: %ld", code);
            else
                snprintf(res.error_msg, sizeof(res.error_msg), "网络错误: %s",
                         curl_easy_strerror(rc));
            res.success = 0;
        }

        if (res.success || code == 416) break;
        /* 4xx 客户端错误是确定性的（链接失效、无权限、参数错），重试只是白等：
         * 401/403/404 立即失败，把真实原因尽早抛给用户。408/429 是“稍后再试”语义，照常重试。 */
        if (code >= 400 && code < 500 && code != 408 && code != 429) break;
        if (attempt < max_retry)
            Sleep((DWORD)((attempt + 1) * opt.retry_delay_ms));
    }
    return res;
}

NetDownloadResult network_download_range(const NetDownloadTask *task) {
    NetDownloadResult res;
    memset(&res, 0, sizeof(res));
    if (!task || !task->url || !task->save_path) {
        strncpy(res.error_msg, "无效参数", sizeof(res.error_msg) - 1);
        return res;
    }
    FILE *fp = utf8_fopen(task->save_path, "r+b");
    if (!fp) fp = utf8_fopen(task->save_path, "w+b");
    if (!fp) {
        snprintf(res.error_msg, sizeof(res.error_msg), "无法打开文件: %s", task->save_path);
        return res;
    }
    res = range_write(task, fp);
    fclose(fp);
    return res;
}

NetDownloadResult network_download_range_fp(const NetDownloadTask *task, FILE *fp) {
    return range_write(task, fp);
}

/* ── 探测：文件大小 + Range 支持（HEAD）── */
NetworkProbe network_probe(const char *url, const NetOptions *opt) {
    NetworkProbe r;
    memset(&r, 0, sizeof(r));
    r.file_size = -1;
    if (!url || !url[0]) return r;

    CURL *h = curl_easy_init();
    if (!h) return r;

    MemBuf hdr; mb_init(&hdr);
    apply_common_opts(h, opt);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);          /* HEAD */
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);  /* 探测只发一次，不跟随 */
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, write_hdr_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &hdr);

    CURLcode rc = curl_easy_perform(h);
    long code = 0; curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    long ver  = 0; curl_easy_getinfo(h, CURLINFO_HTTP_VERSION, &ver);
    proto_string(ver, r.http_version, sizeof(r.http_version));
    curl_off_t cl = 0; curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    r.http_code = code;

    if (code == 200) {
        r.success = 1;
        if (cl > 0) r.file_size = (int64_t)cl;

        if (hdr.data) {
            const char *ar = strcasestr(hdr.data, "Accept-Ranges:");
            int supports = 0;
            if (ar) {
                ar += 13;
                while (*ar == ' ') ar++;
                supports = (strncmp(ar, "bytes", 5) == 0);
            }
            /* 保守策略：无 Accept-Ranges 但有长度时仍允许尝试分片 */
            if (!supports && r.file_size > 0) supports = 1;
            r.supports_range = supports;

            const char *cd = strcasestr(hdr.data, "Content-Disposition:");
            if (cd) parse_content_disposition(cd, r.suggested_filename, sizeof(r.suggested_filename));
        }
    } else if (rc != CURLE_OK) {
        strncpy(r.suggested_filename, "", 0);
    }

    curl_easy_cleanup(h);
    mb_free(&hdr);
    return r;
}

/* ── Content-Disposition 解析（复用原实现，纯字符串）── */
static void parse_content_disposition(const char *hdr, char *out, int out_len) {
    out[0] = '\0';
    if (!hdr || out_len <= 1) return;

    const char *star  = strcasestr(hdr, "filename*=");
    const char *plain = strcasestr(hdr, "filename=");
    const char *src = NULL;
    int is_rfc5987 = 0;
    if (star)      { src = star  + 10; is_rfc5987 = 1; }
    else if (plain){ src = plain + 9;  is_rfc5987 = 0; }
    else return;

    while (*src == ' ' || *src == '\t') src++;
    int quoted = 0;
    if (*src == '"') { quoted = 1; src++; }

    if (is_rfc5987) {
        while (*src && *src != '\'') src++;
        if (*src == '\'') src++;
        while (*src && *src != '\'') src++;
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

    int len = (int)strlen(out);
    while (len > 0 && (out[len-1] == ' ' || out[len-1] == '\t'))
        out[--len] = '\0';

    {
        const char *base = out;
        for (const char *p = out; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        if (base != out) {
            size_t blen = strlen(base);
            memmove(out, base, blen + 1);
        }
    }

    for (int k = 0; out[k]; k++) {
        char c = out[k];
        if (c == '"' || c == '*' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '|' || c == '\\') {
            out[k] = '_';
        }
    }
}

/* ── 大小写不敏感 strstr ── */
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
