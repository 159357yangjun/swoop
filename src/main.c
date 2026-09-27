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
#include <wchar.h>
#include "gui/main_window.h"
#include "gui/resources.h"
#include "engine/download.h"
#include "engine/http.h"
#include "common/util.h"
#include "common/ipcmsg.h"
#include "selftest_run.h"

const wchar_t *g_class_name = IDM_WINDOW_CLASS;

/* 从 Unicode 命令行抓第一个下载地址。
   不再从 WinMain 的 LPSTR/ACP 参数提取后再猜编码：带中文、签名参数或其他
   非 ASCII 字符的 URL 在“首次启动”和“转发到已有实例”两条路径必须完全一致。 */
static void extract_url_w(const wchar_t *cmd, wchar_t *out, size_t n)
{
    out[0] = 0;
    if (!cmd || !cmd[0] || n == 0) return;

    static const wchar_t *schemes[] = { L"http://", L"https://", L"magnet:" };
    const wchar_t *best = NULL;
    for (int i = 0; i < (int)(sizeof schemes / sizeof schemes[0]); i++) {
        const wchar_t *p = wcsstr(cmd, schemes[i]);
        if (p && (!best || p < best)) best = p;
    }
    if (!best) return;

    size_t i = 0;
    while (best[i] && best[i] != L' ' && best[i] != L'\t' && best[i] != L'"'
           && i + 1 < n) {
        out[i] = best[i];
        i++;
    }
    out[i] = 0;
}

/* 已有常驻实例 → 可选地把 URL 用 WM_COPYDATA 转交过去，并把已有窗口恢复到前台。
   即使本次启动没有 URL，也必须退出第二个进程：否则两个 GUI/托盘/下载引擎会同时存在。
   WM_COPYDATA 协议使用 UTF-8，所以直接从宽字符串转 UTF-8，避免 ACP 往返损坏 URL。 */
static int forward_to_existing(const wchar_t *url)
{
    HWND w = FindWindowW(IDM_WINDOW_CLASS, NULL);
    if (!w) return 0;

    if (url && url[0]) {
        char u8[4096];
        u8[0] = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, url, -1, u8, (int)sizeof u8, NULL, NULL)) {
            MessageBoxW(NULL, L"无法编码下载地址。", L"Swoop", MB_ICONERROR);
            return 1;   /* 已确认存在主实例，失败也不能再启动第二套 GUI */
        }

        char payload[4200];
        _snprintf(payload, sizeof payload, "%s\n\n", u8);
        payload[sizeof payload - 1] = 0;
        COPYDATASTRUCT cds;
        cds.dwData = IDM_COPYDATA_MAGIC;
        cds.cbData = (DWORD)strlen(payload) + 1;
        cds.lpData = payload;
        SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
    }

    /* 托盘隐藏/最小化/被其他窗口遮挡时，二次双击应该表现为“打开已有 Swoop”。 */
    ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
    return 1;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp;
    if (strstr(cmd, "--selftest")) return run_selftest();

    wchar_t starturl[4096];
    extract_url_w(GetCommandLineW(), starturl, sizeof starturl / sizeof starturl[0]);

    /* 真单实例：无论是否带 URL，只要已有主窗口就复用它。 */
    if (forward_to_existing(starturl[0] ? starturl : NULL)) return 0;

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
        MessageBoxW(NULL, L"无法加载 winhttp.dll", L"Swoop", MB_ICONERROR);
        return 1;
    }

    /* 日志与数据放同一目录：exe 目录可写就用它（便携），
       否则（如装在 Program Files）退到 %LOCALAPPDATA%\Swoop。 */
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

    if (starturl[0]) {   /* 首次启动即带 URL：Unicode 原样预填 */
        ui_open_new_task_url(starturl, NULL, NULL);
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