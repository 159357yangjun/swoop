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
   0 表示使用系统默认。 */
static void apply_timeouts(HINTERNET h)
{
    if (W.SetTimeouts) W.SetTimeouts(h, 0, 60000, 30000, 3000);
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
    return W.Open(L"IDMNextNative/1.0", g_access, NULL, NULL, 0);
}

static int mb2w(const char *s, wchar_t *out, int n)
{
    int r = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    return r;
}

int http_probe(const char *url, http_resource_t *out)
{
    out->content_length = -1;
    out->supports_range = 0;
    wchar_t wu[2048]; mb2w(url, wu, 2048);
    HINTERNET s = make_session(); if (!s) return -1;

    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwUserNameLength = (DWORD)-1;
    uc.dwPasswordLength = (DWORD)-1;
    if (!W.CrackUrl(wu, 0, 0, &uc)) { W.CloseHandle(s); return -1; }

    int https = (wcsnicmp(wu, L"https", 5) == 0);
    HINTERNET c = W.Connect(s, uc.lpszHostName, uc.nPort, 0);
    if (!c) { W.CloseHandle(s); return -1; }
    HINTERNET r = W.OpenRequest(c, L"GET", uc.lpszUrlPath, NULL, NULL, NULL,
                                https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { W.CloseHandle(c); W.CloseHandle(s); return -1; }
    apply_timeouts(r);

    /* 用 Range: bytes=0-0 探测：服务端回 206 + Content-Range: bytes 0-0/TOTAL */
    W.AddRequestHeaders(r, L"Range: bytes=0-0\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!W.SendRequest(r, NULL, 0, NULL, 0, 0, 0) || !W.ReceiveResponse(r, NULL)) {
        W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s); return -1;
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
    W.CloseHandle(r); W.CloseHandle(c); W.CloseHandle(s);
    return 0;
}

long long http_download(const char *url, long long offset, long long len,
                        int (*write_cb)(void *ctx, const void *data, long long n),
                        void (*progress)(void *ctx, long long got),
                        void *ctx)
{
    wchar_t wu[2048]; mb2w(url, wu, 2048);
    HINTERNET s = make_session(); if (!s) return -1;

    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwUserNameLength = (DWORD)-1;
    uc.dwPasswordLength = (DWORD)-1;
    if (!W.CrackUrl(wu, 0, 0, &uc)) { W.CloseHandle(s); return -1; }

    int https = (wcsnicmp(wu, L"https", 5) == 0);
    HINTERNET c = W.Connect(s, uc.lpszHostName, uc.nPort, 0);
    if (!c) { W.CloseHandle(s); return -1; }
    HINTERNET r = W.OpenRequest(c, L"GET", uc.lpszUrlPath, NULL, NULL, NULL,
                                https ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { W.CloseHandle(c); W.CloseHandle(s); return -1; }
    apply_timeouts(r);

    wchar_t hdr[80];
    if (len > 0)
        _snwprintf(hdr, 80, L"Range: bytes=%lld-%lld\r\n", offset, offset + len - 1);
    else
        _snwprintf(hdr, 80, L"Range: bytes=%lld-\r\n", offset);
    W.AddRequestHeaders(r, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!W.SendRequest(r, NULL, 0, NULL, 0, 0, 0) || !W.ReceiveResponse(r, NULL)) {
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
