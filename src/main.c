#define WIN32_LEAN_AND_MEAN
#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0501
#endif
#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include "gui/main_window.h"
#include "engine/download.h"
#include "engine/http.h"
#include "common/util.h"
#include "common/ipcmsg.h"
#include "selftest_run.h"

const wchar_t *g_class_name = IDM_WINDOW_CLASS;

/* 从命令行里抓第一个 http(s) URL（浏览器/命令行把下载地址交过来）。 */
static void extract_url(const char *cmd, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(cmd, "http://");
    const char *q = strstr(cmd, "https://");
    if (!p || (q && q < p)) p = q;
    if (!p) return;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '"' && i + 1 < n) {
        out[i] = p[i]; i++;
    }
    out[i] = 0;
}

/* 已有常驻实例 → 用 WM_COPYDATA 把 URL 转交过去，自己退出（单实例）。 */
static int forward_to_existing(const char *url)
{
    HWND w = FindWindowW(IDM_WINDOW_CLASS, NULL);
    if (!w) return 0;
    char payload[2048];
    _snprintf(payload, sizeof payload, "%s\n\n", url);
    COPYDATASTRUCT cds;
    cds.dwData = IDM_COPYDATA_MAGIC;
    cds.cbData = (DWORD)strlen(payload) + 1;
    cds.lpData = payload;
    SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
    return 1;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp;
    if (strstr(cmd, "--selftest")) return run_selftest();

    char starturl[2048];
    extract_url(cmd, starturl, sizeof starturl);
    if (starturl[0] && forward_to_existing(starturl)) return 0;   /* 转交后退出 */

    /* 命令行 /starthidden 优先；否则按设置项「启动时隐藏到托盘」 */
    int starthidden = (strstr(cmd, "/starthidden") != NULL) ||
                      (settings_get_int("start_hidden", 0) != 0);

    INITCOMMONCONTROLSEX icc; memset(&icc, 0, sizeof icc);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_COOL_CLASSES;
    InitCommonControlsEx(&icc);

    /* 代理：0 = 跟随系统，1 = 直连（winhttp 运行时加载，不进静态导入） */
    int proxy_mode = settings_get_int("proxy_mode", 0);
    if (http_init(proxy_mode == 1 ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                  : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY) != 0) {
        MessageBoxW(NULL, L"无法加载 winhttp.dll", L"IDM Next", MB_ICONERROR);
        return 1;
    }

    wchar_t exep[MAX_PATH]; GetModuleFileNameW(NULL, exep, MAX_PATH);
    wchar_t *slash = wcsrchr(exep, L'\\'); if (slash) *slash = 0;
    log_init(exep);

    HWND w = create_main_window(h);
    if (!w) { http_cleanup(); return 1; }

    ShowWindow(w, starthidden ? SW_HIDE : show);
    UpdateWindow(w);

    if (starturl[0]) {   /* 首次启动即带 URL：弹预填对话框 */
        wchar_t wu[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, starturl, -1, wu, 2048))
            ui_open_new_task_url(wu);
    }

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    http_cleanup();
    return 0;
}
