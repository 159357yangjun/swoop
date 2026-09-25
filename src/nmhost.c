#define WIN32_LEAN_AND_MEAN
#ifndef WINVER
#define WINVER 0x0501
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "common/jsonlite.h"
#include "common/ipcmsg.h"

/* Swoop 原生消息宿主 —— 对标 IDM 的 IDMMsgHost.exe。
   浏览器(Chrome/Edge)按 native messaging 协议经 stdio 与本进程通信：
   每帧 = 4 字节小端长度 + UTF-8 JSON。本进程把嗅探到的下载 URL 经 WM_COPYDATA
   转交给常驻的 swoop.exe；若主进程未运行则直接拉起它。 */

#define HOST_NAME "com.yangjun.swoop"

/* ---------------- native messaging 帧 ---------------- */

static int read_frame(FILE *in, char **out, unsigned long *outlen)
{
    unsigned char hdr[4];
    if (fread(hdr, 1, 4, in) != 4) return 0;
    unsigned long len = (unsigned long)hdr[0] | ((unsigned long)hdr[1] << 8) |
                        ((unsigned long)hdr[2] << 16) | ((unsigned long)hdr[3] << 24);
    if (len == 0 || len > (16u << 20)) return 0;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return 0;
    if (fread(buf, 1, len, in) != len) { free(buf); return 0; }
    buf[len] = 0;
    *out = buf; *outlen = len;
    return 1;
}

static void write_frame(FILE *out, const char *s)
{
    unsigned long len = (unsigned long)strlen(s);
    unsigned char hdr[4] = {
        (unsigned char)(len & 0xFF), (unsigned char)((len >> 8) & 0xFF),
        (unsigned char)((len >> 16) & 0xFF), (unsigned char)((len >> 24) & 0xFF)
    };
    fwrite(hdr, 1, 4, out);
    fwrite(s, 1, len, out);
    fflush(out);
}

/* ---------------- 转交主进程 ---------------- */

static int launch_main_with_url(const char *url)
{
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *sl = wcsrchr(exe, L'\\'); if (sl) *(sl + 1) = 0;
    wcscat(exe, L"swoop.exe");
    wchar_t wurl[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) return 0;
    wchar_t cmd[MAX_PATH + 2200];
    _snwprintf(cmd, MAX_PATH + 2200, L"\"%s\" \"%s\"", exe, wurl);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 1;
    }
    return 0;
}

static int launch_main(void)
{
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *sl = wcsrchr(exe, L'\\'); if (sl) *(sl + 1) = 0;
    wcscat(exe, L"swoop.exe");
    wchar_t cmd[MAX_PATH + 32];
    _snwprintf(cmd, MAX_PATH + 32, L"\"%s\"", exe);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 1;
    }
    return 0;
}

static int forward_url(const char *url, const char *fn, const char *ref)
{
    char payload[4600];
    _snprintf(payload, sizeof payload, "%s\n%s\n%s", url, fn ? fn : "", ref ? ref : "");
    HWND w = FindWindowW(IDM_WINDOW_CLASS, NULL);
    if (w) {
        COPYDATASTRUCT cds;
        cds.dwData = IDM_COPYDATA_MAGIC;
        cds.cbData = (DWORD)strlen(payload) + 1;
        cds.lpData = payload;
        SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
        return 1;
    }
    return launch_main_with_url(url) ? 2 : 0;
}

static void handle_message(const char *json, int dry)
{
    char action[64] = "", url[2048] = "", fn[512] = "", ref[2048] = "";
    json_get_str(json, "action", action, sizeof action);
    json_get_str(json, "url", url, sizeof url);
    json_get_str(json, "filename", fn, sizeof fn);
    json_get_str(json, "referer", ref, sizeof ref);
    if (!action[0]) strcpy(action, "download");

    if (!strcmp(action, "ping")) {
        write_frame(stdout, "{\"ok\":true,\"app\":\"SwoopNative\"}");
        return;
    }
    if (!strcmp(action, "show")) {
        if (dry) {
            write_frame(stdout, "{\"ok\":true,\"dry\":true}");
            return;
        }
        HWND w = FindWindowW(IDM_WINDOW_CLASS, NULL);
        if (w) {
            ShowWindow(w, SW_SHOW);
            SetForegroundWindow(w);
            write_frame(stdout, "{\"ok\":true,\"shown\":true,\"existing\":true}");
            return;
        }
        if (launch_main())
            write_frame(stdout, "{\"ok\":true,\"shown\":true,\"started\":true}");
        else
            write_frame(stdout, "{\"ok\":false,\"error\":\"failed to start swoop.exe\"}");
        return;
    }
    if (!url[0]) { write_frame(stdout, "{\"ok\":false,\"error\":\"missing url\"}"); return; }

    if (dry) {
        /* 文本走 stderr，保证 stdout 始终只含 native messaging 帧 */
        fprintf(stderr, "dry-run: action=%s url=%s filename=%s referer=%s\n",
                action, url, fn, ref);
        write_frame(stdout, "{\"ok\":true,\"dry\":true}");
        return;
    }
    int towin = forward_url(url, fn, ref);
    if (towin == 1) {
        write_frame(stdout, "{\"ok\":true,\"forwarded\":true,\"existing\":true}");
        fprintf(stderr, "nmhost: WM_COPYDATA->主进程 url=%s\n", url);
    } else if (towin == 2) {
        /* 只有确认 CreateProcessW 成功后才允许扩展取消浏览器原下载。 */
        write_frame(stdout, "{\"ok\":true,\"forwarded\":true,\"started\":true}");
        fprintf(stderr, "nmhost: 拉起 swoop.exe url=%s\n", url);
    } else {
        write_frame(stdout, "{\"ok\":false,\"error\":\"failed to start or reach swoop.exe\"}");
        fprintf(stderr, "nmhost: 转交失败 url=%s\n", url);
    }
}

/* ---------------- 注册 native messaging host ---------------- */

static void json_escape_path(const char *in, char *out, size_t n)
{
    size_t i = 0;
    for (const char *p = in; *p && i + 2 < n; p++) {
        if (*p == '\\' || *p == '"') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = 0;
}

static void write_utf8_file(const wchar_t *path, const char *utf8)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fwrite(utf8, 1, strlen(utf8), f);
    fclose(f);
}

static int register_host(int argc, char **argv)
{
    const char *extid = NULL;
    int manifest_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--manifest-only")) manifest_only = 1;
        else if (argv[i][0] != '-') extid = argv[i];
    }

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy(dir, exe);
    wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *(sl + 1) = 0;

    wchar_t manifest[MAX_PATH];
    _snwprintf(manifest, MAX_PATH, L"%s%s.json", dir, L"" HOST_NAME);

    char exeA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exeA, MAX_PATH, NULL, NULL);
    char exeEsc[MAX_PATH * 2]; json_escape_path(exeA, exeEsc, sizeof exeEsc);

    char json[4096];
    _snprintf(json, sizeof json,
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Swoop 原生消息宿主\",\n"
        "  \"path\": \"%s\",\n"
        "  \"type\": \"stdio\",\n"
        "  \"allowed_origins\": [ \"chrome-extension://%s/\" ]\n"
        "}\n",
        HOST_NAME, exeEsc, extid ? extid : "REPLACE_WITH_EXTENSION_ID");
    write_utf8_file(manifest, json);

    static const char *roots[] = {
        "Software\\Google\\Chrome\\NativeMessagingHosts\\" HOST_NAME,
        "Software\\Microsoft\\Edge\\NativeMessagingHosts\\" HOST_NAME,
        "Software\\Chromium\\NativeMessagingHosts\\" HOST_NAME,
    };
    DWORD bytes = (DWORD)((wcslen(manifest) + 1) * sizeof(wchar_t));
    int okc = 0;
    if (!manifest_only) {
        for (int i = 0; i < 3; i++) {
            HKEY h; DWORD disp;
            if (RegCreateKeyExA(HKEY_CURRENT_USER, roots[i], 0, NULL,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &h, &disp) == ERROR_SUCCESS) {
                if (RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE *)manifest, bytes) == ERROR_SUCCESS)
                    okc++;
                RegCloseKey(h);
            }
        }
    }

    char manA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, manifest, -1, manA, MAX_PATH, NULL, NULL);
    printf("宿主清单: %s\n", manA);
    if (manifest_only) printf("(仅生成清单，未写注册表)\n");
    else printf("注册表: Chrome/Edge/Chromium 共 %d/3 写入\n", okc);
    if (!extid)
        printf("注意: 未提供扩展 ID，allowed_origins 为占位符。\n"
               "      请用 `swoop_nmhost.exe --register-nmhost <扩展ID>` 重跑（扩展ID 见 chrome://extensions）。\n");
    return manifest_only ? 0 : (okc > 0 ? 0 : 1);
}

static int unregister_host(void)
{
    static const char *roots[] = {
        "Software\\Google\\Chrome\\NativeMessagingHosts\\" HOST_NAME,
        "Software\\Microsoft\\Edge\\NativeMessagingHosts\\" HOST_NAME,
        "Software\\Chromium\\NativeMessagingHosts\\" HOST_NAME,
    };
    for (int i = 0; i < 3; i++)
        RegDeleteKeyA(HKEY_CURRENT_USER, roots[i]);
    printf("已注销 %s\n", HOST_NAME);
    return 0;
}

/* ---------------- 自测 ---------------- */

static int nm_selftest(void)
{
    int fails = 0;
    struct { const char *js; const char *key; const char *want; } t[] = {
        { "{\"action\":\"download\",\"url\":\"https://a/b.bin\"}", "url", "https://a/b.bin" },
        { "{\"url\":\"x\",\"filename\":\"c:\\\\d\\\\e.zip\"}", "filename", "c:\\d\\e.zip" },
        { "{\"referer\":\"q=\\u4e2d\\u6587\"}", "referer", "q=\xe4\xb8\xad\xe6\x96\x87" },
        { "{\"url\":\"a\\\"b\\/c\"}", "url", "a\"b/c" },
    };
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
        char buf[512]; int ok = json_get_str(t[i].js, t[i].key, buf, sizeof buf);
        int pass = ok && strcmp(buf, t[i].want) == 0;
        if (!pass) fails++;
        printf("json[%d] %s key=%s -> '%s' (期望 '%s')\n",
               i, pass ? "PASS" : "FAIL", t[i].key, buf, t[i].want);
    }
    /* 缺键应返回 0 */
    { char buf[64]; int ok = json_get_str("{\"other\":1}", "url", buf, sizeof buf);
      printf("json[missing] %s\n", ok ? "FAIL" : "PASS"); if (ok) fails++; }
    /* 值里出现同名子串不应误判为键 */
    { char buf[64]; int ok = json_get_str("{\"x\":\"url\",\"url\":\"real\"}", "url", buf, sizeof buf);
      int pass = ok && strcmp(buf, "real") == 0;
      printf("json[keyfield] %s -> '%s'\n", pass ? "PASS" : "FAIL", buf); if (!pass) fails++; }

    /* 帧收发环回 */
    FILE *tf = tmpfile();
    if (tf) {
        const char *msg = "{\"action\":\"download\",\"url\":\"https://x/y\"}";
        write_frame(tf, msg);
        fflush(tf); fseek(tf, 0, SEEK_SET);
        char *got = NULL; unsigned long gl = 0;
        int ok = read_frame(tf, &got, &gl) && got && strcmp(got, msg) == 0
                 && gl == strlen(msg);
        printf("frame roundtrip %s (len=%lu)\n", ok ? "PASS" : "FAIL", gl);
        if (!ok) fails++;
        free(got);
        fclose(tf);
    } else { printf("frame roundtrip FAIL (tmpfile)\n"); fails++; }

    char rf[MAX_PATH]; GetModuleFileNameA(NULL, rf, MAX_PATH);
    char *sl = strrchr(rf, '\\'); if (sl) *(sl + 1) = 0;
    strcat(rf, "swoop_nmhost_selftest_result.txt");
    FILE *f = fopen(rf, "w");
    if (f) { fprintf(f, "nmhost selftest %s fails=%d\n", fails ? "FAIL" : "PASS", fails); fclose(f); }

    printf("nmhost selftest: %s (fails=%d)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    int reg = 0, unreg = 0, selftest = 0, dry = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--register-nmhost")) reg = 1;
        else if (!strcmp(argv[i], "--unregister-nmhost")) unreg = 1;
        else if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "--dry-run")) dry = 1;
    }
    if (reg)   return register_host(argc, argv);
    if (unreg) return unregister_host();
    if (selftest) return nm_selftest();

    /* native messaging 主循环：浏览器经 stdin 发帧，我们经 stdout 回帧 */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    char *msg; unsigned long mlen;
    while (read_frame(stdin, &msg, &mlen)) {
        handle_message(msg, dry);
        free(msg);
        if (dry) break;    /* 便于管道单帧测试 */
    }
    return 0;
}
