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

/* 进程级单实例门闩。必须在主窗口/下载引擎初始化之前创建；仅用 FindWindow 会有
   两个进程同时启动、都还没创建窗口时的竞态。Local\ 作用域限定到当前登录会话。 */
static const wchar_t *g_single_instance_mutex = L"Local\\Swoop.SingleInstance.v1";

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

    /* 最小化才 restore；已最大化的窗口不能因为二次启动被无意还原成普通尺寸。
       托盘隐藏但未最小化时只需要 show。 */
    if (IsIconic(w))
        ShowWindow(w, SW_RESTORE);
    else if (!IsWindowVisible(w))
        ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    return 1;
}

/* 第二个进程可能在第一个进程刚拿到 mutex、主窗口尚未创建时进来。
   此时绝不能继续初始化第二套引擎；短暂等待主窗口出现后再转发。 */
static int wait_and_forward_to_existing(const wchar_t *url, DWORD timeout_ms)
{
    const DWORD step_ms = 50;
    DWORD waited = 0;
    for (;;) {
        if (forward_to_existing(url)) return 1;
        if (waited >= timeout_ms) break;
        Sleep(step_ms);
        waited += step_ms;
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hp;
    if (strstr(cmd, "--selftest")) return run_selftest();

    wchar_t starturl[4096];
    extract_url_w(GetCommandLineW(), starturl, sizeof starturl / sizeof starturl[0]);

    /* 原子单实例：mutex 必须早于 GUI / HTTP /任务存储初始化。
       ERROR_ALREADY_EXISTS 表示另一个进程已经抢到启动权，即使它的窗口暂时还没出现。 */
    HANDLE single = CreateMutexW(NULL, FALSE, g_single_instance_mutex);
    if (!single) {
        MessageBoxW(NULL, L"无法创建单实例锁。", L"Swoop", MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        int ok = wait_and_forward_to_existing(starturl[0] ? starturl : NULL, 10000);
        CloseHandle(single);
        if (ok) return 0;
        MessageBoxW(NULL, L"已有 Swoop 正在启动，但主窗口未能就绪。", L"Swoop", MB_ICONERROR);
        return 1;
    }

    /* 兼容升级场景：旧版 Swoop 可能已经运行但还没有上述 mutex。 */
    if (forward_to_existing(starturl[0] ? starturl : NULL)) {
        CloseHandle(single);
        return 0;
    }

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
        CloseHandle(single);
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
    if (!w) {
        http_cleanup();
        CloseHandle(single);
        return 1;
    }

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
    CloseHandle(single);
    return 0;
}
