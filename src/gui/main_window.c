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
static wchar_t g_pending_file[512];   /* 站点建议的文件名（可空） */
static wchar_t g_pending_ref[2048];   /* 来源页，防盗链需要（可空） */
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
    /* 必须把主菜单挂上：否则「设置 / 关于」等命令没有任何入口
       （菜单是 WM_COMMAND 的唯一来源，托盘菜单只有显示/退出两项）。 */
    HMENU menu = LoadMenuW(h, MAKEINTRESOURCEW(IDR_MAIN));
    return CreateWindowExW(0, g_class_name, L"IDM Next — 原生下载管理器",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           780, 500, NULL, menu, h, NULL);
}

/* 宽字符 → UTF-8（不要用 wcstombs：默认 C locale 下非 ASCII 会失败/截断） */
static void w2u8(const wchar_t *w, char *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (w && w[0]) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
}

/* 是否已有「正在使用该链接或该保存位置」的任务（避免两个任务写同一个文件）。
   已完成 / 已失败的不算 —— 它们不再写文件。否则用户想重新下一个已完成的链接时
   会被挡下来，只能先去列表里把旧记录删掉。返回下标或 -1。 */
static int find_dup(const char *url, const wchar_t *outfile)
{
    for (int i = 0; i < g_ntasks; i++) {
        download_task_t *t = g_tasks[i];
        if (t->status == DL_COMPLETE || t->status == DL_ERROR) continue;
        if (url && url[0] && _stricmp(t->url, url) == 0) return i;
        if (outfile && outfile[0] && _wcsicmp(t->outfile, outfile) == 0) return i;
    }
    return -1;
}

static INT_PTR CALLBACK new_task_dlg(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    if (m == WM_INITDIALOG) {
        SetDlgItemTextW(d, IDC_URL, g_pending_url);   /* 浏览器嗅探/命令行预填 */
        SetDlgItemTextW(d, IDC_SAVEAS, L"");
        return TRUE;
    }
    if (m == WM_COMMAND) {
        int id = (int)LOWORD(w);
        if (id == IDCANCEL) {
            g_pending_url[0] = g_pending_file[0] = g_pending_ref[0] = 0;
            EndDialog(d, 0);
            return TRUE;
        }
        if (id == IDOK) {
            wchar_t url[2048], save[2048];
            GetDlgItemTextW(d, IDC_URL, url, 2048);
            GetDlgItemTextW(d, IDC_SAVEAS, save, 2048);
            if (url[0]) {
                char curl[4096]; w2u8(url, curl, sizeof curl);
                int kind = torrent_kind_for_url(curl);
                wchar_t out[MAX_PATH];
                if (kind == IDM_KIND_TORRENT) {
                    /* BT：outfile 的语义是「下载目录」，交给 aria2 --dir */
                    if (save[0]) wcsncpy(out, save, MAX_PATH - 1);
                    else         wcsncpy(out, g_base_dir, MAX_PATH - 1);
                    out[MAX_PATH - 1] = 0;
                } else if (save[0]) {
                    wcsncpy(out, save, MAX_PATH - 1); out[MAX_PATH - 1] = 0;
                } else {
                    /* 留空 → 按类型分类；站点给了建议文件名就优先用它 */
                    const wchar_t *name_src = g_pending_file[0] ? g_pending_file : url;
                    category_build_path(g_base_dir, name_src, g_use_cat, out, MAX_PATH);
                }

                if (find_dup(curl, out) >= 0) {
                    MessageBoxW(d, L"相同链接或相同保存位置的任务已存在，未重复添加。",
                                L"IDM Next", MB_OK | MB_ICONINFORMATION);
                    g_pending_url[0] = g_pending_file[0] = g_pending_ref[0] = 0;
                    EndDialog(d, 1);
                    return TRUE;
                }

                download_task_t *t = task_create(curl, out,
                                                 settings_get_int("num_connections", 8));
                if (t) {
                    t->kind = kind;
                    if (g_pending_ref[0]) wcsncpy(t->referer, g_pending_ref, 2047);
                    ui_add_task(t);     /* 先入列表，由队列调度按并发上限决定何时开始 */
                    save_tasks();
                    /* 这里**不**直接 schedule_queue()：首次启动要同步探测服务器
                       （最长 10s 连接超时），放在对话框的 WM_COMMAND 里会让「确定」
                       按钮卡到探测结束才关窗口，看起来像死机。
                       交给 250ms 的定时器去启动 —— 窗口先关、任务先显示成「排队中」。 */
                }
            }
            g_pending_url[0] = g_pending_file[0] = g_pending_ref[0] = 0;
            EndDialog(d, 1);
            return TRUE;
        }
    }
    return FALSE;
}

void ui_open_new_task_url(const wchar_t *url, const wchar_t *filename, const wchar_t *referer)
{
    if (!url || !url[0]) return;
    wcsncpy(g_pending_url, url, 2047);           g_pending_url[2047] = 0;
    if (filename) { wcsncpy(g_pending_file, filename, 511); }
    else g_pending_file[0] = 0;
    g_pending_file[511] = 0;
    if (referer) { wcsncpy(g_pending_ref, referer, 2047); }
    else g_pending_ref[0] = 0;
    g_pending_ref[2047] = 0;

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
            /* 去掉结尾分隔符，后面按 "%s\\%s" 拼，留着会拼出双反斜杠 */
            size_t bl = wcslen(bd);
            while (bl > 0 && (bd[bl - 1] == L'\\' || bd[bl - 1] == L'/')) bd[--bl] = 0;
            if (bd[0]) {
                settings_set_str("base_dir", bd);
                wcscpy(g_base_dir, bd);
            } else {
                /* 清空 = 恢复默认下载目录。以前是「什么都不做」，
                   用户以为改了其实没改，而且没有任何提示。 */
                category_default_base(g_base_dir, MAX_PATH);
                settings_set_str("base_dir", g_base_dir);
            }
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

/* 收集所有选中行（升序）。列表已去掉 LVS_SINGLESEL，支持多选批量操作。 */
static int collect_selected(int *idx, int max)
{
    int n = 0, i = -1;
    while ((i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) >= 0) {
        if (i >= 0 && i < g_ntasks && n < max) idx[n++] = i;
    }
    return n;
}

/* 「开始/续传」：只置为排队，真正启动交给队列调度 —— 否则会绕过并发上限。 */
static void start_selected(void)
{
    int idx[64]; int n = collect_selected(idx, 64);
    if (n == 0) return;
    for (int k = 0; k < n; k++) {
        download_task_t *t = g_tasks[idx[k]];
        if (t->status == DL_DOWNLOADING) continue;
        if (t->status == DL_COMPLETE) {          /* 对已完成的任务 = 重新下载 */
            t->downloaded = 0; t->total = 0; t->seg_count = 0; t->speed = 0.0;
            for (int s = 0; s < 16; s++) t->seg_written[s] = 0;
        }
        t->user_paused = 0;
        t->status = DL_QUEUED;
    }
    save_tasks();
    schedule_queue();
}

/* 「暂停」：先给所有选中任务发停止信号，再统一 join。
   逐个 join 会串行等待（每个最多等一次接收超时），选一堆任务时界面会卡很久。 */
static void pause_selected(void)
{
    int idx[64]; int n = collect_selected(idx, 64);
    if (n == 0) return;
    for (int k = 0; k < n; k++) {
        download_task_t *t = g_tasks[idx[k]];
        if (t->status != DL_COMPLETE) t->user_paused = 1;   /* 用户意图：调度不得自动恢复 */
        if (t->status == DL_DOWNLOADING) task_stop(t);
    }
    for (int k = 0; k < n; k++) task_pause(g_tasks[idx[k]]);
    save_tasks();
}

static void delete_selected(void)
{
    int idx[64]; int n = collect_selected(idx, 64);
    if (n == 0) return;
    for (int k = n - 1; k >= 0; k--) {             /* 从后往前删，索引不位移 */
        int i = idx[k];
        download_task_t *t = g_tasks[i];
        for (int j = i; j < g_ntasks - 1; j++) g_tasks[j] = g_tasks[j + 1];
        g_ntasks--;
        ListView_DeleteItem(g_list, i);
        task_free(t);          /* 停线程 + 回收内存，避免泄漏 */
    }
    save_tasks();
}

/* 「打开文件 / 打开所在文件夹」——只对第一个选中项生效 */
static void open_selected(int folder)
{
    int idx[64]; int n = collect_selected(idx, 64);
    if (n == 0) return;
    download_task_t *t = g_tasks[idx[0]];
    if (t->kind == IDM_KIND_TORRENT) { open_path(t->outfile); return; }  /* BT 的 outfile 是目录 */
    if (folder) {
        wchar_t p[MAX_PATH]; wcsncpy(p, t->outfile, MAX_PATH - 1); p[MAX_PATH - 1] = 0;
        wchar_t *sl = wcsrchr(p, L'\\');
        if (sl) *sl = 0;
        open_path(p);
    } else if (GetFileAttributesW(t->outfile) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"文件还不存在（任务未完成，或已被移动/删除）。",
                    L"IDM Next", MB_OK | MB_ICONINFORMATION);
    } else {
        open_path(t->outfile);
    }
}

/* 任务的显示名（列表第一列 / 通知里都用它，保证一致） */
static void task_display_name(const download_task_t *t, wchar_t *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (t->kind == IDM_KIND_TORRENT) {
        /* BT 的 outfile 是下载目录，直接取 basename 只会显示成 "Downloads" */
        torrent_display_name(t->url, out, n);
        return;
    }
    wcsncpy(out, t->outfile, n - 1);
    out[n - 1] = 0;
    wchar_t *sl = wcsrchr(out, L'\\');
    if (sl) memmove(out, sl + 1, (wcslen(sl + 1) + 1) * sizeof(wchar_t));
}

void ui_add_task(download_task_t *t)
{
    if (!t) return;
    if (g_ntasks >= 64) {
        /* 静默丢弃会让用户「点了确定却什么都没发生」，必须明确告知 */
        MessageBoxW(g_hwnd, L"任务数量已达上限（64 个），本次未添加。\n请先删除一些任务。",
                    L"IDM Next", MB_OK | MB_ICONWARNING);
        task_free(t);       /* 所有权已交到本函数，不能泄漏 */
        return;
    }
    int i = g_ntasks++;
    g_tasks[i] = t;
    LVITEMW it; memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT; it.iItem = i; it.iSubItem = 0;
    wchar_t name[MAX_PATH];
    task_display_name(t, name, MAX_PATH);
    it.pszText = name;
    ListView_InsertItem(g_list, &it);
}

void ui_refresh(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        download_task_t *t = g_tasks[i];
        char tmp[64];
        wchar_t wsize[64], wspeed[64], wprog[64];
        wsize[0] = wspeed[0] = wprog[0] = 0;

        if (t->kind == IDM_KIND_TORRENT) {
            /* 进度由 aria2c 自己管，本进程拿不到 —— 明确标注，
               不要显示成 "-1 B / 0.0%"（那看起来像卡死了）。 */
            wcscpy(wsize, L"—");
            wcscpy(wprog, L"aria2");
            wcscpy(wspeed, L"—");
        } else {
            if (t->total > 0) {
                format_bytes(t->total, tmp, 64); mbstowcs(wsize, tmp, 64);
                double pct = 100.0 * (double)t->downloaded / (double)t->total;
                if (pct > 100.0) pct = 100.0;      /* 兜底：续传重算时别显示 >100% */
                if (pct < 0.0) pct = 0.0;
                _snwprintf(wprog, 64, L"%.1f%%", pct);
            } else {
                wcscpy(wsize, L"未知");   /* 服务端没给 Content-Length */
                wcscpy(wprog, L"—");
            }
            format_speed(t->speed, tmp, 64); mbstowcs(wspeed, tmp, 64);
        }

        ListView_SetItemText(g_list, i, 1, wsize);
        ListView_SetItemText(g_list, i, 2, wprog);
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

/* ---- 失败通知：以前失败是完全静默的，用户只能自己盯状态列 ---- */
static void notify_failed(const wchar_t *name, int count)
{
    if (g_notify_balloon) {
        g_nid.uFlags = NIF_INFO;
        g_nid.dwInfoFlags = NIIF_WARNING;
        wcscpy(g_nid.szInfoTitle, L"IDM Next — 有任务失败");
        if (count <= 1) _snwprintf(g_nid.szInfo, 256, L"%s 下载失败", name ? name : L"");
        else            _snwprintf(g_nid.szInfo, 256, L"%d 个任务下载失败", count);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (g_notify_sound) MessageBeep(MB_ICONHAND);
}

/* ---- 任务持久化 ---- */
static void init_tasks_path(void)
{
    /* 装到 Program Files 下时 exe 目录不可写 → 退到 %LOCALAPPDATA%\IDMNext。
       否则 taskstore_save 静默失败，用户重启后整个任务列表消失且毫无提示。 */
    wchar_t d[MAX_PATH];
    if (data_dir(d, MAX_PATH) != 0) { g_tasks_path[0] = 0; return; }
    _snwprintf(g_tasks_path, MAX_PATH, L"%sidm_tasks.dat", d);
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
    int idx[64];
    /* 挑选逻辑抽成纯函数（queue_pick）：只挑排队中且用户没暂停的。
       以前这里直接把所有 DL_QUEUED 都拉起来，于是「暂停」排队任务无效。 */
    int n = queue_pick(g_tasks, g_ntasks, slots, idx, 64);
    for (int k = 0; k < n; k++) task_start(g_tasks[idx[k]]);
}

/* ---- 调度用：全部暂停。先统一发停止信号再统一 join，
   否则逐个 join 会串行等超时，选一堆任务时界面会卡很久。
   注意：本函数是「临时停」，不写 user_paused（定时调度到点还要自动恢复）。 ---- */
static void pause_all(void)
{
    for (int i = 0; i < g_ntasks; i++)
        if (g_tasks[i]->status == DL_DOWNLOADING) task_stop(g_tasks[i]);
    for (int i = 0; i < g_ntasks; i++) task_pause(g_tasks[i]);
}

/* ---- 用户点「全部暂停」：这是用户的明确意图，要标记 user_paused，
   否则定时调度的「到点开始」会把它整体恢复（用户会觉得暂停失效）。 ---- */
static void pause_all_user(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        if (g_tasks[i]->status == DL_COMPLETE) continue;   /* 完成的不需要暂停 */
        g_tasks[i]->user_paused = 1;
    }
    pause_all();
    save_tasks();
}

/* ---- 用户点「全部开始」：清掉 user_paused（用户自己的意图）。
   不强动已完成的 —— 「全部开始」的语义是「把没下完的都跑起来」，
   若把已完成的也重置重下，会把用户磁盘上已有的文件整份覆盖掉。 ---- */
static void start_all(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        download_task_t *t = g_tasks[i];
        if (t->status == DL_DOWNLOADING) continue;
        if (t->status == DL_COMPLETE) continue;   /* 要重下请选中它点「开始 / 续传」 */
        t->user_paused = 0;
        t->status = DL_QUEUED;
    }
    save_tasks();
    schedule_queue();
}

/* ---- 调度用：到点开始。必须跳过 user_paused，
   否则「用户明确暂停的大任务」会被定时器强行拉起来下载。 ---- */
static void enqueue_all(void)
{
    for (int i = 0; i < g_ntasks; i++) {
        download_task_t *t = g_tasks[i];
        if (t->user_paused) continue;
        dl_status s = t->status;
        if (s == DL_PAUSED || s == DL_QUEUED) t->status = DL_QUEUED;
    }
    schedule_queue();
}

/* 取 payload 里第 line 行（0 起）。载荷格式：url\nfilename\nreferer */
static void copy_line(const char *p, int line, char *out, size_t n)
{
    if (!out || n == 0) return;
    out[0] = 0;
    if (!p) return;
    int cur = 0;
    size_t i = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '\n') {
            if (cur == line) break;
            cur++; i = 0;
            continue;
        }
        if (cur == line && i + 1 < n) out[i++] = *q;
    }
    out[i] = 0;
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
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER, 0, 0, 0, 0,
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
        wchar_t okName[64] = L"", errName[64] = L"";
        int comp = 0, fail = 0;
        for (int i = 0; i < g_ntasks; i++) {
            dl_status prev = g_tasks[i]->status;
            task_tick(g_tasks[i], now);
            dl_status cur = g_tasks[i]->status;
            if (prev != DL_COMPLETE && cur == DL_COMPLETE) {
                comp++;
                task_display_name(g_tasks[i], okName, 64);
            }
            /* 失败以前是静默的：只统计完成，错误连落盘都不做 */
            if (prev != DL_ERROR && cur == DL_ERROR) {
                fail++;
                task_display_name(g_tasks[i], errName, 64);
            }
        }
        if (comp > 0) { notify_complete(okName, comp); save_tasks(); }
        if (fail > 0) { notify_failed(errName, fail); save_tasks(); }
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
            /* 不 SetForegroundWindow 的话，菜单点外面不会消失（经典 Win32 坑） */
            SetForegroundWindow(hwnd);
            TrackPopupMenu(GetSubMenu(g_tray_menu, 0), TPM_RIGHTALIGN,
                           p.x, p.y, 0, hwnd, NULL);
            PostMessage(hwnd, WM_NULL, 0, 0);
        } else if ((UINT)lp == WM_LBUTTONDBLCLK) {
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) ShowWindow(hwnd, SW_HIDE);
            else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh && nh->hwndFrom == g_list && nh->code == NM_DBLCLK) {
            open_selected(0);                  /* 双击打开文件 */
            return 0;
        }
        break;                                 /* 其余通知交回默认处理 */
    }
    case WM_COMMAND: {
        int id = (int)LOWORD(wp);
        switch (id) {
            case IDM_NEW:      DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_NEW_TASK), hwnd, new_task_dlg); break;
            case IDM_START:    start_selected(); break;
            case IDM_PAUSE:    pause_selected(); break;
            case IDM_DELETE:   delete_selected(); break;
            case IDM_START_ALL:start_all(); break;
            case IDM_PAUSE_ALL:pause_all_user(); break;
            case IDM_OPEN_FILE:open_selected(0); break;
            case IDM_OPEN_DIR: open_selected(1); break;
            case IDM_TRAY_SHOW:
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) ShowWindow(hwnd, SW_HIDE);
                else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
                break;
            case IDM_TRAY_EXIT:DestroyWindow(hwnd); break;
            case IDM_SETTINGS: DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd, settings_dlg); break;
            case IDM_ABOUT:    MessageBoxW(hwnd, L"IDM Next 原生版\n纯 C / Win32，对标 IDM",
                                           L"关于", MB_OK); break;
        }
        return 0;
    }
    case WM_COPYDATA: {
        /* 浏览器消息宿主 / 第二个实例把任务交过来。
           载荷三行：url \n filename(可空) \n referer(可空)。
           以前只取第一行 → 站点建议的文件名和来源页全被丢掉，
           防盗链站点会 403。 */
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == IDM_COPYDATA_MAGIC && cds->lpData) {
            const char *p = (const char *)cds->lpData;
            char url[2048], fn[512], ref[2048];
            copy_line(p, 0, url, sizeof url);
            copy_line(p, 1, fn,  sizeof fn);
            copy_line(p, 2, ref, sizeof ref);
            if (url[0]) {
                wchar_t wu[2048], wf[512], wr[2048];
                wf[0] = wr[0] = 0;
                if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wu, 2048)) {
                    if (fn[0])  MultiByteToWideChar(CP_UTF8, 0, fn,  -1, wf, 512);
                    if (ref[0]) MultiByteToWideChar(CP_UTF8, 0, ref, -1, wr, 2048);
                    ui_open_new_task_url(wu, wf, wr);
                }
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
