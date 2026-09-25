#ifndef IDM_MAIN_WINDOW_H
#define IDM_MAIN_WINDOW_H
#ifndef WINVER
#define WINVER 0x0501
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0501
#endif
#include <windows.h>
#include "engine/download.h"

#define WM_TRAYICON (WM_USER + 1)

LRESULT CALLBACK main_wndproc(HWND, UINT, WPARAM, LPARAM);

extern const wchar_t *g_class_name;

HWND create_main_window(HINSTANCE h);
void ui_add_task(download_task_t *t);
void ui_refresh(void);

/* 显示主窗口并弹出「新建任务」对话框，URL 已预填（供浏览器嗅探/命令行传入）。
   filename / referer 可为 NULL：站点建议的文件名 + 来源页（防盗链站点需要）。 */
void ui_open_new_task_url(const wchar_t *url, const wchar_t *filename, const wchar_t *referer);

#endif
