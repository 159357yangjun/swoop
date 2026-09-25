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
#include "gui/resources.h"
#include "engine/download.h"
#include "engine/http.h"
#include "common/util.h"
#include "common/ipcmsg.h"
#include "selftest_run.h"

const wchar_t *g_class_name = IDM_WINDOW_CLASS;

/* 从命令行里抓第一个下载地址（浏览器/命令行把地址交过来）。
   支持 http:// / https:// / magnet: —— 磁力链必须认，否则单实例转发和
   「启动即弹新建任务」对磁力链全都失效。 */
static void extract_url(const char *cmd, char *out, size_t n)
{
    out[0] = 0;
    if (!cmd || !cmd[0]) return;
    static const char *schemes[] = { "http://", "https://", "magnet:" };
    const char *best = NULL;
    for (int i = 0; i < (int)(sizeof schemes / sizeof schemes[0]); i++) {
        const char *p = strstr(cmd, schemes[i]);
        if (p && (!best || p < best)) best = p;
    }
    if (!best) return;
    size_t i = 0;
    while (best[i] && best[i] != ' ' && best[i] != '\t' && best[i] != '"' && i + 1 < n) {
        out[i] = best[i]; i++;
    }
    out[i] = 0;
}

/* 已有常驻实例 → 用 WM_COPYDATA 把 URL 转交过去，自己退出（单实例）。
   注意编码：cmd 是 ANSI（中文系统上是 GBK），而接收端按 UTF-8 解。
   必须先 ACP→宽→UTF-8，否则带中文的链接转交过去会变乱码。 */
static int forward_to_existing(const char *url)
{
    HWND w = FindWindowW(IDM_WINDOW_CLASS, NULL);
    if (!w) return 0;

    char u8[2048];
    u8[0] = 0;
    wchar_t wu[2048];
    if (MultiByteToWideChar(CP_ACP, 0, url, -1, wu, 2048))
        WideCharToMultiByte(CP_UTF8, 0, wu, -1, u8, (int)sizeof u8, NULL, NULL);
    if (!u8[0]) { strncpy(u8, url, sizeof u8 - 1); u8[sizeof u8 - 1] = 0; }

    char payload[2100];
    _snprintf(payload, sizeof payload, "%s\n\n", u8);
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

    /* 日志与数据放同一目录：exe 目录可写就用它（便携），
       否则（如装在 Program Files）退到 %LOCALAPPDATA%\IDMNext。 */
    wchar_t ddir[MAX_PATH];
    if (data_dir(ddir, MAX_PATH) == 0) {
        size_t L = wcslen(ddir);
        while (L > 0 && ddir[L - 1] == L'\\') ddir[--L] = 0;   /* log_init 自己补反斜杠 */
        log_init(ddir);
    }

    HWND w = create_main_window(h);
    if (!w) { http_cleanup(); return 1; }

    /* 菜单里写着 Ctrl+N，就必须真的能按 —— 否则是空头承诺 */
    HACCEL acc = LoadAcceleratorsW(h, MAKEINTRESOURCEW(IDR_ACCEL));

    ShowWindow(w, starthidden ? SW_HIDE : show);
    UpdateWindow(w);

    if (starturl[0]) {   /* 首次启动即带 URL：弹预填对话框 */
        wchar_t wu[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, starturl, -1, wu, 2048))
            ui_open_new_task_url(wu, NULL, NULL);
    }

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        /* 快捷键必须先于 TranslateMessage 处理 */
        if (acc && TranslateAcceleratorW(w, acc, &m)) continue;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    http_cleanup();
    return 0;
}
