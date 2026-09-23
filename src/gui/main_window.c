#include "main_window.h"
#include "resources.h"
#include "common/util.h"
#include "common/ipcmsg.h"
#include "engine/http.h"
#include "engine/speedlimit.h"
#include "engine/sched.h"
#include "engine/taskstore.h"
#include "engine/queue.h"
#include "engine/category.h"
#include "engine/torrent.h"
#include <commctrl.h>
#include <winhttp.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

static HINSTANCE g_inst;
static HWND g_hwnd, g_toolbar, g_list, g_status;
static download_task_t *g_tasks[64];
static int g_ntasks = 0;
static NOTIFYICONDATAW g_nid;
static HMENU g_tray_menu;
static int g_close_to_tray = 1;
static wchar_t g_pending_url[2048];   /* 预填给「新建任务」对话框的 URL */
static wchar_t g_tasks_path[MAX_PATH]; /* 任务列表持久化文件路径 */

static int g_notify_balloon = 1;
static int g_notify_sound = 1;
static int g_sched_enabled = 0;
static int g_sched_start = 100;       /* 01:00 */
static int g_sched_stop = 800;        /* 08:00 */
static int g_last_min = -1;           /* 上次检查的分钟(HHMM)，每分钟只判定一次 */

/* 限速下拉：KB/s，0=不限 */
static const int g_speed_kb[] = { 0, 32, 64, 128, 256, 512, 1024, 2048 };
static const wchar_t *g_speed_label[] = {
    L"无限制", L"32 KB/s", L"64 KB/s", L"128 KB/s",
    L"256 KB/s", L"512 KB/s", L"1 MB/s", L"2 MB/s"
};
#define SPEED_N ((int)(sizeof g_speed_kb / sizeof g_speed_kb[0]))

/* 队列 / 分类目录 */
static int  g_max_concurrent = 3;
static int  g_use_cat = 1;
static wchar_t g_base_dir[MAX_PATH];

static void save_tasks(void);         /* 前向声明：新增/删除/暂停后落盘 */
static void apply_speed_limit(void);  /* 前向声明：设置对话框也要调用 */
static void schedule_queue(void);     /* 前向声明：按并发上限自动启动排队任务 */

static const wchar_t *status_text(dl_status s)
{
    switch (s) {
        case DL_QUEUED:     return L"排队中";
        case DL_DOWNLOADING:return L"下载中";
        case DL_PAUSED:     return L"已暂停";
        case DL_COMPLETE:   return L"完成";
        default:            return L"错误";
    }
}

static int register_class(HINSTANCE h)
{
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_wndproc;
    wc.hInstance = h;
    wc.lpszClassName = g_class_name;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClassExW(&wc) ? 1 : 0;
}

HWND create_main_window(HINSTANCE h)
{
    g_inst = h;
    if (!register_class(h)) return NULL;
    return CreateWindowExW(0, g_class_name, L"IDM Next — 原生下载管理器",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           780, 500, NULL, NULL, h, NULL);
}

static INT_PTR CALLBACK new_task_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_INITDIALOG) {
        SetDlgItemTextW(d, IDC_URL, g_pending_url);   /* 浏览器嗅探/命令行预填 */
        SetDlgItemTextW(d, IDC_SAVEAS, L"");
        g_pending_url[0] = 0;
        return TRUE;
    }
    if (m == WM_COMMAND) {
        int id = (int)LOWORD(w);
        if (id == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        if (id == IDOK) {
            wchar_t url[2048], save[2048];
            GetDlgItemTextW(d, IDC_URL, url, 2048);
            GetDlgItemTextW(d, IDC_SAVEAS, save, 2048);
            if (url[0]) {
                char curl[2048]; wcstombs(curl, url, 2048);
                int kind = torrent_kind_for_url(curl);
                wchar_t out[MAX_PATH];
                if (save[0]) {
                    wcscpy(out, save);
                } else if (kind == IDM_KIND_TORRENT) {
                    wcscpy(out, g_base_dir);            /* BT：保存目录（aria2 --dir） */
                } else {
                    category_build_path(g_base_dir, url, g_use_cat, out, MAX_PATH);
                }
                download_task_t *t = task_create(curl, out,
                                                 settings_get_int("num_connections", 8));
                if (t) {
                    t->kind = kind;
                    ui_add_task(t);     /* 先入列表，由队列调度按并发上限决定何时开始 */
                    save_tasks();
                    schedule_queue();
                }
            }
            EndDialog(d, 1);
            return TRUE;
        }
    }
    return FALSE;
}

void ui_open_new_task_url(const wchar_t *url)
{
    if (!url || !url[0]) return;
    wcsncpy(g_pending_url, url, 2047);
    g_pending_url[2047] = 0;
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_NEW_TASK), g_hwnd, new_task_dlg);
    }
}

static void set_hhmm(HWND d, int id, int hhmm)
{
    wchar_t b[16];
    _snwprintf(b, 16, L"%02d:%02d", (hhmm / 100) % 24, hhmm % 100);
    SetDlgItemTextW(d, id, b);
}

static int get_hhmm(HWND d, int id, int def)
{
    wchar_t b[32]; GetDlgItemTextW(d, id, b, 32);
    int h = -1, m = 0;
    if (swscanf(b, L"%d:%d", &h, &m) != 2) {
        int v = _wtoi(b);
        if (v == 0 && b[0] != L'0') return def;
        h = (v / 100) % 24; m = v % 100;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) return def;
    return h * 100 + m;
}

static INT_PTR CALLBACK settings_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_INITDIALOG) {
        wchar_t buf[16];
        _snwprintf(buf, 16, L"%d", settings_get_int("num_connections", 8));
        SetDlgItemTextW(d, IDC_CONN, buf);

        _snwprintf(buf, 16, L"%d", settings_get_int("max_concurrent", 3));
        SetDlgItemTextW(d, IDC_MAXCONC, buf);
        CheckDlgButton(d, IDC_USECAT,
                       settings_get_int("use_category_dirs", 1) ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(d, IDC_BASEDIR, g_base_dir);

        HWND sc = GetDlgItem(d, IDC_SPEED);
        for (int i = 0; i < SPEED_N; i++)
            SendMessageW(sc, CB_ADDSTRING, 0, (LPARAM)g_speed_label[i]);
        int kb = settings_get_int("speed_limit", 0), si = 0;
        for (int i = 0; i < SPEED_N; i++) if (g_speed_kb[i] == kb) si = i;
        SendMessageW(sc, CB_SETCURSEL, (WPARAM)si, 0);

        CheckDlgButton(d, IDC_CLOSETRAY,
                       settings_get_int("close_to_tray", 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_STARTHIDDEN,
                       settings_get_int("start_hidden", 0) ? BST_CHECKED : BST_UNCHECKED);

        HWND cb = GetDlgItem(d, IDC_PROXY);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"使用系统代理");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"不使用代理（直连）");
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)settings_get_int("proxy_mode", 0), 0);

        CheckDlgButton(d, IDC_NOTIFY_BALLOON,
                       settings_get_int("notify_balloon", 1) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_NOTIFY_SOUND,
                       settings_get_int("notify_sound", 1) ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(d, IDC_SCHED_EN,
                       settings_get_int("sched_enabled", 0) ? BST_CHECKED : BST_UNCHECKED);
        set_hhmm(d, IDC_SCHED_START, settings_get_int("sched_start", 100));
        set_hhmm(d, IDC_SCHED_STOP,  settings_get_int("sched_stop", 800));
        return TRUE;
    }
    if (m == WM_COMMAND) {
        int id = (int)LOWORD(w);
        if (id == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        if (id == IDOK) {
            wchar_t buf[16];
            GetDlgItemTextW(d, IDC_CONN, buf, 16);
            int nc = _wtoi(buf);
            if (nc < 1) nc = 1;
            if (nc > 16) nc = 16;
            settings_set_int("num_connections", nc);

            GetDlgItemTextW(d, IDC_MAXCONC, buf, 16);
            int mc = _wtoi(buf);
            if (mc < 1) mc = 1;
            if (mc > 16) mc = 16;
            settings_set_int("max_concurrent", mc);
            settings_set_int("use_category_dirs", IsDlgButtonChecked(d, IDC_USECAT) ? 1 : 0);
            wchar_t bd[MAX_PATH];
            GetDlgItemTextW(d, IDC_BASEDIR, bd, MAX_PATH);
            if (bd[0]) { settings_set_str("base_dir", bd); wcscpy(g_base_dir, bd); }
            g_max_concurrent = mc;
            g_use_cat = IsDlgButtonChecked(d, IDC_USECAT) ? 1 : 0;

            int si = (int)SendMessageW(GetDlgItem(d, IDC_SPEED), CB_GETCURSEL, 0, 0);
            if (si < 0 || si >= SPEED_N) si = 0;
            settings_set_int("speed_limit", g_speed_kb[si]);

            settings_set_int("close_to_tray", IsDlgButtonChecked(d, IDC_CLOSETRAY) ? 1 : 0);
            settings_set_int("start_hidden", IsDlgButtonChecked(d, IDC_STARTHIDDEN) ? 1 : 0);

            int pm = (int)SendMessageW(GetDlgItem(d, IDC_PROXY), CB_GETCURSEL, 0, 0);
            if (pm < 0) pm = 0;
            settings_set_int("proxy_mode", pm);

            settings_set_int("notify_balloon", IsDlgButtonChecked(d, IDC_NOTIFY_BALLOON) ? 1 : 0);
            settings_set_int("notify_sound", IsDlgButtonChecked(d, IDC_NOTIFY_SOUND) ? 1 : 0);

            settings_set_int("sched_enabled", IsDlgButtonChecked(d, IDC_SCHED_EN) ? 1 : 0);
            settings_set_int("sched_start", get_hhmm(d, IDC_SCHED_START, 100));
            settings_set_int("sched_stop",  get_hhmm(d, IDC_SCHED_STOP, 800));

            /* 实时生效 */
            http_init(pm == 1 ? WINHTTP_ACCESS_TYPE_NO_PROXY
                              : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY);
            g_close_to_tray  = settings_get_int("close_to_tray", 1);
            g_notify_balloon = settings_get_int("notify_balloon", 1);
            g_notify_sound   = settings_get_int("notify_sound", 1);
            g_sched_enabled  = settings_get_int("sched_enabled", 0);
            g_sched_start    = settings_get_int("sched_start", 100);
            g_sched_stop     = settings_get_int("sched_stop", 800);
            g_last_min = -1;   /* 让调度下次立即重判 */
            apply_speed_limit();
            schedule_queue();  /* 并发上限可能变大 → 立刻补启动排队任务 */
            EndDialog(d, 1);
            return TRUE;
        }
    }
    return FALSE;
}

static void start_selected(void)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i >= 0 && i < g_ntasks) task_start(g_tasks[i]);
}

static void pause_selected(void)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i >= 0 && i < g_ntasks) { task_pause(g_tasks[i]); save_tasks(); }
}

static void delete_selected(void)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0 || i >= g_ntasks) return;
    download_task_t *t = g_tasks[i];
    for (int j = i; j < g_ntasks - 1; j++) g_tasks[j] = g_tasks[j + 1];
    g_ntasks--;
    ListView_DeleteItem(g_list, i);
    task_free(t);          /* 停线程 + 回收内存，避免泄漏 */
    save_tasks();
}

void ui_add_task(download_task_t *t)
{
    if (g_ntasks >= 64) return;
    int i = g_ntasks++;
    g_tasks[i] = t;
    LVITEMW it; memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT; it.iItem = i; it.iSubItem = 0;
    wchar_t name[MAX_PATH]; wcscpy(name, t->outfile);
    wchar_t *sl = wcsrchr(name, L'\\');
    it.pszText = sl ? sl + 1 : name;
    ListView_InsertItem(g_list, &it);
}

void ui_refresh(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        download_task_t *t = g_tasks[i];
        char tmp[64];
        wchar_t wsize[64], wspeed[64], wprog[64];

        format_bytes(t->total, tmp, 64); mbstowcs(wsize, tmp, 64);
        ListView_SetItemText(g_list, i, 1, wsize);

        double pct = (t->total > 0) ? (100.0 * (double)t->downloaded / (double)t->total) : 0.0;
        _snwprintf(wprog, 64, L"%.1f%%", pct);
        ListView_SetItemText(g_list, i, 2, wprog);

        format_speed(t->speed, tmp, 64); mbstowcs(wspeed, tmp, 64);
        ListView_SetItemText(g_list, i, 3, wspeed);

        ListView_SetItemText(g_list, i, 4, (wchar_t *)status_text(t->status));
    }

    int dl = 0, comp = 0, q = 0; double sp = 0;
    for (int i = 0; i < g_ntasks; i++) {
        if (g_tasks[i]->status == DL_DOWNLOADING) { dl++; sp += g_tasks[i]->speed; }
        if (g_tasks[i]->status == DL_COMPLETE) comp++;
        if (g_tasks[i]->status == DL_QUEUED) q++;
    }
    wchar_t txt[224];
    long long lim = dl_get_speed_limit();
    if (lim > 0)
        _snwprintf(txt, 224,
                   L"任务 %d  |  下载中 %d  |  排队 %d  |  完成 %d  |  总速度 %.2f KB/s  |  限速 %lld KB/s",
                   g_ntasks, dl, q, comp, sp / 1024.0, lim / 1024);
    else
        _snwprintf(txt, 224, L"任务 %d  |  下载中 %d  |  排队 %d  |  完成 %d  |  总速度 %.2f KB/s",
                   g_ntasks, dl, q, comp, sp / 1024.0);
    SetWindowTextW(g_status, txt);
}

/* ---- 限速 ---- */
static void apply_speed_limit(void)
{
    int kb = settings_get_int("speed_limit", 0);
    if (kb < 0) kb = 0;
    dl_set_speed_limit((long long)kb * 1024);
}

/* ---- 完成提示音：动态加载 winmm（失败退回 MessageBeep，不增加静态依赖） ---- */
#ifndef SND_ASYNC
#define SND_ASYNC     0x0001
#endif
#ifndef SND_NODEFAULT
#define SND_NODEFAULT 0x0002
#endif
#ifndef SND_ALIAS
#define SND_ALIAS     0x00010000
#endif
static void play_chime(void)
{
    typedef BOOL (WINAPI *PFN_PlaySoundW)(LPCWSTR, HMODULE, DWORD);
    static PFN_PlaySoundW s_ps;
    static int s_tried = 0;
    if (!s_tried) {
        s_tried = 1;
        s_ps = (PFN_PlaySoundW)load_dll_func("winmm.dll", "PlaySoundW");
    }
    if (s_ps) s_ps(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
    else MessageBeep(MB_OK);
}

/* ---- 完成通知（托盘气泡；可关） ---- */
static void notify_complete(const wchar_t *name, int count)
{
    if (g_notify_balloon) {
        g_nid.uFlags = NIF_INFO;
        g_nid.dwInfoFlags = NIIF_INFO;
        wcscpy(g_nid.szInfoTitle, L"IDM Next");
        if (count <= 1) _snwprintf(g_nid.szInfo, 256, L"%s 下载完成", name ? name : L"");
        else            _snwprintf(g_nid.szInfo, 256, L"%d 个任务下载完成", count);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (g_notify_sound) play_chime();
}

/* ---- 任务持久化 ---- */
static void init_tasks_path(void)
{
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *sl = wcsrchr(exe, L'\\'); if (sl) *(sl + 1) = 0;
    _snwprintf(g_tasks_path, MAX_PATH, L"%sidm_tasks.dat", exe);
}

static void save_tasks(void)
{
    if (g_tasks_path[0]) taskstore_save(g_tasks_path, g_tasks, g_ntasks);
}

static void load_tasks(void)
{
    download_task_t *arr[64];
    int n = 0;
    if (taskstore_load(g_tasks_path, arr, 64, &n) != 0) return;
    for (int i = 0; i < n; i++) ui_add_task(arr[i]);
}

/* ---- 队列调度：按并发上限，把「排队中」的任务逐个启动 ---- */
static void schedule_queue(void)
{
    int running = 0;
    for (int i = 0; i < g_ntasks; i++)
        if (g_tasks[i]->status == DL_DOWNLOADING) running++;
    int slots = queue_slots(running, g_max_concurrent);
    for (int i = 0; i < g_ntasks && slots > 0; i++) {
        if (g_tasks[i]->status == DL_QUEUED) { task_start(g_tasks[i]); slots--; }
    }
}

/* ---- 调度用：全部暂停 ---- */
static void pause_all(void)
{
    for (int i = 0; i < g_ntasks; i++) task_pause(g_tasks[i]);
}
/* ---- 调度用：全部入队（保留进度）后按上限启动 ---- */
static void enqueue_all(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        dl_status s = g_tasks[i]->status;
        if (s == DL_PAUSED || s == DL_QUEUED) g_tasks[i]->status = DL_QUEUED;
    }
    schedule_queue();
}

LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_close_to_tray = settings_get_int("close_to_tray", 1);
        INITCOMMONCONTROLSEX icc; memset(&icc, 0, sizeof icc);
        icc.dwSize = sizeof icc;
        icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_COOL_CLASSES;
        InitCommonControlsEx(&icc);

        g_toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
            WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | CCS_TOP, 0, 0, 0, 0,
            hwnd, (HMENU)(INT_PTR)IDC_TOOLBAR, g_inst, NULL);
        SendMessageW(g_toolbar, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);
        int sidx = (int)SendMessageW(g_toolbar, TB_ADDSTRINGW, 0,
                                    (LPARAM)L"新建\0开始\0暂停\0删除\0");
        TBBUTTON b[4]; memset(b, 0, sizeof b);
        const int ids[] = { IDM_NEW, IDM_START, IDM_PAUSE, IDM_DELETE };
        for (int i = 0; i < 4; i++) {
            b[i].iString = sidx + i;
            b[i].fsState = TBSTATE_ENABLED;
            b[i].fsStyle = BTNS_BUTTON;
            b[i].idCommand = ids[i];
        }
        SendMessageW(g_toolbar, TB_ADDBUTTONSW, (WPARAM)4, (LPARAM)b);
        SendMessageW(g_toolbar, TB_AUTOSIZE, 0, 0);

        g_list = CreateWindowExW(0, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER, 0, 0, 0, 0,
            hwnd, (HMENU)(INT_PTR)IDC_LIST, g_inst, NULL);
        LVCOLUMNW col; memset(&col, 0, sizeof col); col.mask = LVCF_TEXT | LVCF_WIDTH;
        const wchar_t *cols[] = { L"文件名", L"大小", L"进度", L"速度", L"状态" };
        int cw[] = { 240, 100, 90, 110, 80 };
        for (int i = 0; i < 5; i++) {
            col.pszText = (wchar_t *)cols[i]; col.cx = cw[i];
            ListView_InsertColumn(g_list, i, &col);
        }

        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
            hwnd, (HMENU)(INT_PTR)IDC_STATUS, g_inst, NULL);

        g_tray_menu = LoadMenuW(g_inst, MAKEINTRESOURCEW(IDR_TRAY));
        memset(&g_nid, 0, sizeof g_nid);
        g_nid.cbSize = sizeof g_nid;
        g_nid.hWnd = hwnd; g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP),
                                        IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        wcscpy(g_nid.szTip, L"IDM Next 原生下载管理器");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        /* 设置：通知/调度/限速 */
        g_notify_balloon = settings_get_int("notify_balloon", 1);
        g_notify_sound   = settings_get_int("notify_sound", 1);
        g_sched_enabled  = settings_get_int("sched_enabled", 0);
        g_sched_start    = settings_get_int("sched_start", 100);
        g_sched_stop     = settings_get_int("sched_stop", 800);
        apply_speed_limit();

        /* 设置：队列 / 分类目录 */
        g_max_concurrent = settings_get_int("max_concurrent", 3);
        g_use_cat        = settings_get_int("use_category_dirs", 1);
        if (!settings_get_str("base_dir", g_base_dir, MAX_PATH))
            category_default_base(g_base_dir, MAX_PATH);

        init_tasks_path();
        load_tasks();               /* 重启恢复任务列表 */

        SetTimer(hwnd, 1, 250, NULL);
        return 0;
    }
    case WM_SIZE: {
        RECT r; GetClientRect(hwnd, &r);
        SendMessageW(g_toolbar, TB_AUTOSIZE, 0, 0);
        int tbh = 0; { RECT tr; GetWindowRect(g_toolbar, &tr); tbh = tr.bottom - tr.top; }
        int sbh = 24;
        MoveWindow(g_status, 0, r.bottom - sbh, r.right, sbh, TRUE);
        MoveWindow(g_list, 0, tbh, r.right, r.bottom - tbh - sbh, TRUE);
        return 0;
    }
    case WM_TIMER: {
        DWORD now = GetTickCount();
        wchar_t lastName[64] = L"";
        int comp = 0;
        for (int i = 0; i < g_ntasks; i++) {
            dl_status prev = g_tasks[i]->status;
            task_tick(g_tasks[i], now);
            if (prev != DL_COMPLETE && g_tasks[i]->status == DL_COMPLETE) {
                comp++;
                wchar_t *p = g_tasks[i]->outfile;
                wchar_t *sl = wcsrchr(p, L'\\');
                wcscpy(lastName, sl ? sl + 1 : p);
            }
        }
        if (comp > 0) { notify_complete(lastName, comp); save_tasks(); }
        schedule_queue();               /* 空出并发槽 → 启动排队任务 */
        ui_refresh();

        /* 定时调度：每分钟判定一次 */
        SYSTEMTIME st; GetLocalTime(&st);
        int cur = st.wHour * 100 + st.wMinute;
        if (cur != g_last_min) {
            g_last_min = cur;
            sched_action a = sched_decide(g_sched_enabled, g_sched_start, g_sched_stop, cur);
            if (a == SCHED_STOP)  { pause_all();  save_tasks(); }
            else if (a == SCHED_START) { enqueue_all(); save_tasks(); }
        }
        return 0;
    }
    case WM_TRAYICON: {
        if ((UINT)lp == WM_RBUTTONUP) {
            POINT p; GetCursorPos(&p);
            TrackPopupMenu(GetSubMenu(g_tray_menu, 0), TPM_RIGHTALIGN,
                           p.x, p.y, 0, hwnd, NULL);
        } else if ((UINT)lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = (int)LOWORD(wp);
        switch (id) {
            case IDM_NEW:      DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_NEW_TASK), hwnd, new_task_dlg); break;
            case IDM_START:    start_selected(); break;
            case IDM_PAUSE:    pause_selected(); break;
            case IDM_DELETE:   delete_selected(); break;
            case IDM_TRAY_SHOW:ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW); break;
            case IDM_TRAY_EXIT:DestroyWindow(hwnd); break;
            case IDM_SETTINGS: DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, settings_dlg); break;
            case IDM_ABOUT:    MessageBoxW(hwnd, L"IDM Next 原生版\n纯 C / Win32，对标 IDM",
                                           L"关于", MB_OK); break;
        }
        return 0;
    }
    case WM_COPYDATA: {
        /* 浏览器消息宿主 / 第二个实例把 URL 交过来（载荷 UTF-8，首行=url） */
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == IDM_COPYDATA_MAGIC && cds->lpData) {
            const char *p = (const char *)cds->lpData;
            char url[2048]; size_t i = 0;
            while (p[i] && p[i] != '\n' && i < sizeof url - 1) { url[i] = p[i]; i++; }
            url[i] = 0;
            if (url[0]) {
                wchar_t wu[2048];
                if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wu, 2048))
                    ui_open_new_task_url(wu);
            }
        }
        return TRUE;
    }
    case WM_CLOSE: {
        if (g_close_to_tray) { ShowWindow(hwnd, SW_HIDE); return 0; }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY: {
        save_tasks();                       /* 退出前落盘，供下次重启恢复 */
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
