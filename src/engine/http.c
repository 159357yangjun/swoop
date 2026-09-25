#include "http.h"
#include "common/util.h"
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- 动态加载 WinHTTP（仿 IDMan.exe：主 exe 静态导入里没有 winhttp） ---- */
typedef HINTERNET (WINAPI *PFN_Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *PFN_Connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *PFN_OpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *PFN_SendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *PFN_ReceiveResponse)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *PFN_QueryDataAvailable)(HINTERNET, LPDWORD);
typedef BOOL (WINAPI *PFN_ReadData)(HINTERNET, LPVOID, DWORD, DWORD *);
typedef BOOL (WINAPI *PFN_QueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD *);
typedef BOOL (WINAPI *PFN_AddRequestHeaders)(HINTERNET, LPCWSTR, DWORD, DWORD);
typedef BOOL (WINAPI *PFN_CloseHandle)(HINTERNET);
typedef BOOL (WINAPI *PFN_SetOption)(HINTERNET, DWORD, LPVOID, DWORD);
typedef BOOL (WINAPI *PFN_CrackUrl)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef BOOL (WINAPI *PFN_SetTimeouts)(HINTERNET, int, int, int, int);

static struct {
    HMODULE mod;
    PFN_Open Open;
    PFN_Connect Connect;
    PFN_OpenRequest OpenRequest;
    PFN_SendRequest SendRequest;
    PFN_ReceiveResponse ReceiveResponse;
    PFN_QueryDataAvailable QueryDataAvailable;
    PFN_ReadData ReadData;
    PFN_QueryHeaders QueryHeaders;
    PFN_AddRequestHeaders AddRequestHeaders;
    PFN_CloseHandle CloseHandle;
    PFN_SetOption SetOption;
    PFN_CrackUrl CrackUrl;
    PFN_SetTimeouts SetTimeouts;
} W;

static DWORD g_access = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;

static int load_all(void)
{
    W.mod = LoadLibraryA("winhttp.dll");
    if (!W.mod) return -1;
#define G(name, str) do { W.name = (PFN_##name)GetProcAddress(W.mod, str); if (!W.name) return -1; } while (0)
    G(Open, "WinHttpOpen");
    G(Connect, "WinHttpConnect");
    G(OpenRequest, "WinHttpOpenRequest");
    G(SendRequest, "WinHttpSendRequest");
    G(ReceiveResponse, "WinHttpReceiveResponse");
    G(QueryDataAvailable, "WinHttpQueryDataAvailable");
    G(ReadData, "WinHttpReadData");
    G(QueryHeaders, "WinHttpQueryHeaders");
    G(AddRequestHeaders, "WinHttpAddRequestHeaders");
    G(CloseHandle, "WinHttpCloseHandle");
    G(SetOption, "WinHttpSetOption");
    G(CrackUrl, "WinHttpCrackUrl");
    /* 接收超时非必需，失败也不致命（个别精简版系统可能没有） */
    W.SetTimeouts = (PFN_SetTimeouts)GetProcAddress(W.mod, "WinHttpSetTimeouts");
#undef G
    return 0;
}

/* 给请求设置超时：接收超时短(3s)，这样暂停时阻塞在 ReadData 的分片线程能尽快返回。
   连接/发送超时必须有上限 —— 新建任务时的探测是**同步**跑的（在 UI 线程里），
   主机不可达时若按系统默认等 60s，界面会假死一分钟。
   10s 连不上基本等于下不了，直接报错更快。
   0 表示使用系统默认。 */
static void apply_timeouts(HINTERNET h)
{
    if (W.SetTimeouts) W.SetTimeouts(h, 0, 10000, 20000, 3000);
}

int http_init(int access_type)
{
    g_access = (DWORD)access_type;
    return load_all();
}

void http_cleanup(void)
{
    if (W.mod) { FreeLibrary(W.mod); W.mod = NULL; }
}

static HINTERNET make_session(void)
{
    HINTERNET s = W.Open(L"IDMNextNative/1.0", g_access, NULL, NULL, 0);
    if (s) {
        /* 显式钉住重定向策略：跟随 301/302/307（网盘、CDN 直链很常见），
           但不允许 https→http 的降级跳转（安全）。
           不设的话行为取决于系统默认，不同 Windows 版本可能不一致。 */
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        W.SetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
    }
    return s;
}

static int mb2w(const char *s, wchar_t *out, int n)
{
    int r = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return r;
}

/* 拆 URL：host / 端口 / 请求路径（**含 ?query**，必须显式拼 extra info，
   否则 query 会丢）/ 是否 https。成功返回 0。 */
static int crack_url(const char *url, wchar_t *wu, int wun,
                     wchar_t *host, int hostn, INTERNET_PORT *port,
                     wchar_t *reqpath, int reqn, int *https)
{
    if (!mb2w(url, wu, wun)) return -1;
    wchar_t path[2048], extra[2048];
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;  uc.dwHostNameLength = hostn;
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwUserNameLength = (DWORD)-1;
    uc.dwPasswordLength = (DWORD)-1;
    if (!W.CrackUrl(wu, 0, 0, &uc)) return -1;
    _snwprintf(reqpath, reqn, L"%s%s", path, extra);
    /* 必须用拆出来的端口：URL 里写 :18099 时用默认 80 会连错地方 */
    *port = uc.nPort ? uc.nPort : INTERNET_DEFAULT_PORT;
    *https = (wcsnicmp(wu, L"https", 5) == 0);
    return 0;
}

/* 读响应状态码；取不到返回 0。 */
static int query_status(HINTERNET r)
{
    DWORD code = 0, len = sizeof code;
    if (!W.QueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, NULL))
        return 0;
    return (int)code;
}

/* 2xx 视为成功（下载器不主动跟 3xx，交给 WinHTTP 的重定向策略）。 */
static int status_ok(int st)
{
    return st >= 200 && st < 300;
}

int http_probe(const char *url, http_resource_t *out)
{
    out->content_length = -1;
    out->supports_range = 0;
    out->status = 0;

    wchar_t wu[2048], host[256], path[2048];
    int https = 0; INTERNET_PORT port = INTERNET_DEFAULT_PORT;
    if (crack_url(url, wu, 2048, host, 256, &port, path, 2048, &https) != 0) return -1;

    HINTERNET s = make_session(); if (!s) return -1;
    HINTERNET c = W.Connect(s, host, port, 0);
    if (!c) { W.CloseHandle(s); return -1; }
    HINTERNET r = W.OpenRequest(c, L"GET", path, NULL, NULL, NULL,
                                https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { W.CloseHandle(c); W.CloseHandle(s); return -1; }
    apply_timeouts(r);

    /* 用 Range: bytes=0-0 探测：服务端回 206 + Content-Range: bytes 0-0/TOTAL */
    W.AddRequestHeaders(r, L"Range: bytes=0-0\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!W.SendRequest(r, NULL, 0, NULL, 0, 0, 0) || !W.ReceiveResponse(r, NULL)) {
        W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s); return -1;
    }

    /* 关键：拿到状态码才算数。404/403/500 的错误页不能被当成下载内容。 */
    int st = query_status(r);
    out->status = st;
    if (st && !status_ok(st)) {
        log_msg("http_probe: HTTP %d for %s", st, url);
        W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s);
        return -2;
    }

    wchar_t buf[128]; DWORD len = sizeof buf;
    if (W.QueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"Content-Range", buf, &len, NULL)) {
        wchar_t *slash = wcsrchr(buf, L'/');
        if (slash) { out->content_length = _wcstoi64(slash + 1, NULL, 10); out->supports_range = 1; }
    }
    /* 没有 Content-Range 就看 Accept-Ranges */
    if (!out->supports_range) {
        wchar_t ab[64]; DWORD al = sizeof ab;
        if (W.QueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"Accept-Ranges", ab, &al, NULL)) {
            if (wcsstr(ab, L"bytes")) out->supports_range = 1;
        }
    }
    /* 206 没带 Content-Range 时，退回 Content-Length（此时是全长） */
    if (out->content_length <= 0) {
        wchar_t cl[64]; DWORD cll = sizeof cl;
        if (W.QueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"Content-Length", cl, &cll, NULL)) {
            long long v = _wcstoi64(cl, NULL, 10);
            if (v > 0) out->content_length = v;
        }
    }
    W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s);
    return 0;
}

long long http_download(const char *url, const wchar_t *referer,
                        long long offset, long long len,
                        int (*write_cb)(void *ctx, const void *data, long long n),
                        void (*progress)(void *ctx, long long got),
                        void *ctx)
{
    wchar_t wu[2048], host[256], path[2048];
    int https = 0; INTERNET_PORT port = INTERNET_DEFAULT_PORT;
    if (crack_url(url, wu, 2048, host, 256, &port, path, 2048, &https) != 0) return -1;

    HINTERNET s = make_session(); if (!s) return -1;
    HINTERNET c = W.Connect(s, host, port, 0);
    if (!c) { W.CloseHandle(s); return -1; }
    HINTERNET r = W.OpenRequest(c, L"GET", path, NULL, NULL, NULL,
                                https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { W.CloseHandle(c); W.CloseHandle(s); return -1; }
    apply_timeouts(r);

    /* 防盗链：带上来源页 */
    if (referer && referer[0]) {
        wchar_t refh[2200];
        _snwprintf(refh, 2200, L"Referer: %s\r\n", referer);
        W.AddRequestHeaders(r, refh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    wchar_t hdr[80];
    if (len > 0)
        _snwprintf(hdr, 80, L"Range: bytes=%lld-%lld\r\n", offset, offset + len - 1);
    else
        _snwprintf(hdr, 80, L"Range: bytes=%lld-\r\n", offset);
    W.AddRequestHeaders(r, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!W.SendRequest(r, NULL, 0, NULL, 0, 0, 0) || !W.ReceiveResponse(r, NULL)) {
        W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s); return -1;
    }

    /* 非 2xx：不写任何字节，直接报错（否则错误页会被当成文件内容）。 */
    int st = query_status(r);
    if (st && !status_ok(st)) {
        log_msg("http_download: HTTP %d for %s", st, url);
        W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s); return -1;
    }

    char buf[1 << 16];
    long long total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!W.QueryDataAvailable(r, &avail)) break;
        if (avail == 0) break;
        if (avail > (DWORD)sizeof buf) avail = (DWORD)sizeof buf;
        DWORD rd = 0;
        if (!W.ReadData(r, buf, avail, &rd)) break;
        if (rd == 0) break;
        if (write_cb && write_cb(ctx, buf, (long long)rd) == 0) break; /* 中止信号 */
        total += rd;
        if (progress) progress(ctx, total);
        if (len > 0 && total >= len) break;
    }

    W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s);
    return total;
}
