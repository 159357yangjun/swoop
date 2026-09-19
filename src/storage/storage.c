/*
 * storage.c  — 去除 cJSON 依赖，改用 Windows API + 简化格式
 *
 * 配置文件格式（config.dat）：
 *   save_dir=C:\path
 *   max_threads=8
 *   speed_limit=0
 *   browser_hook_enabled=1
 *   auto_start=1
 *   proxy=
 *   max_retries=3
 *
 * 任务持久化格式（tasks.dat）：二进制简化格式
 * 历史记录格式（history.dat）：追加文本格式
 */
#include "storage.h"
#include "download_core.h"  /* dlmgr_save_state() */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 全局 ── */
static char g_data_dir[MAX_PATH] = {0};

/* ── 路径辅助 ── */
static void make_path(char *out, int max_len, const char *fname) {
    snprintf(out, (size_t)max_len, "%s\\%s", g_data_dir, fname);
}

/* 读整个文本文件到 malloc 缓冲，返回 NULL 表示失败 */
static char *read_text_file(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return NULL; }
    char *buf = (char*)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD rd = 0;
    ReadFile(h, buf, sz, &rd, NULL);
    buf[rd] = '\0';
    CloseHandle(h);
    return buf;
}

/* 写整个文本文件 */
static int write_text_file(const char *path, const char *content) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD wr = 0;
    WriteFile(h, content, (DWORD)strlen(content), &wr, NULL);
    CloseHandle(h);
    return 0;
}

/* ── 从 "key=value" 格式的行提取 value ── */
static void parse_kv_line(const char *line, const char *key, char *out, int max_len) {
    char search[128];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(line, search);
    if (!p) return;
    p += strlen(search);
    /* 去掉换行 */
    const char *end = p;
    while (*end && *end != '\r' && *end != '\n') end++;
    int len = (int)(end - p);
    if (len > max_len - 1) len = max_len - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
}

static int parse_int_val(const char *line, const char *key) {
    char buf[64] = {0};
    parse_kv_line(line, key, buf, 64);
    return atoi(buf);
}

/* ── 公开 API ── */
int storage_init(const char *data_dir) {
    if (!data_dir || !data_dir[0]) return -1;
    strncpy(g_data_dir, data_dir, MAX_PATH - 1);
    CreateDirectoryA(g_data_dir, NULL);
    return 0;
}

void storage_cleanup(void) { g_data_dir[0] = '\0'; }

AppConfig storage_default_config(void) {
    AppConfig c; memset(&c, 0, sizeof(c));
    strcpy(c.save_dir, "C:\\Users\\Public\\Downloads");
    c.max_threads      = 8;
    c.speed_limit      = 0;
    c.browser_hook_enabled = 1;
    c.auto_start       = 1;
    c.max_retries     = 3;
    return c;
}

int storage_save_config(const AppConfig *cfg) {
    if (!cfg) return -1;
    char path[MAX_PATH]; make_path(path, MAX_PATH, "config.dat");
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "save_dir=%s\n"
        "max_threads=%d\n"
        "speed_limit=%d\n"
        "browser_hook_enabled=%d\n"
        "auto_start=%d\n"
        "proxy=%s\n"
        "max_retries=%d\n",
        cfg->save_dir,
        cfg->max_threads,
        cfg->speed_limit,
        cfg->browser_hook_enabled,
        cfg->auto_start,
        cfg->proxy,
        cfg->max_retries
    );
    return write_text_file(path, buf);
}

int storage_load_config(AppConfig *out) {
    if (!out) return -1;
    *out = storage_default_config();

    char path[MAX_PATH]; make_path(path, MAX_PATH, "config.dat");
    char *raw = read_text_file(path);
    if (!raw) return -1;

    char *line = raw;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (strstr(line, "save_dir="))    parse_kv_line(line, "save_dir",    out->save_dir,    sizeof(out->save_dir));
        if (strstr(line, "max_threads="))  out->max_threads  = parse_int_val(line, "max_threads");
        if (strstr(line, "speed_limit="))  out->speed_limit  = parse_int_val(line, "speed_limit");
        if (strstr(line, "browser_hook_enabled=")) out->browser_hook_enabled = parse_int_val(line, "browser_hook_enabled");
        if (strstr(line, "auto_start="))   out->auto_start   = parse_int_val(line, "auto_start");
        if (strstr(line, "proxy="))       parse_kv_line(line, "proxy",       out->proxy,       sizeof(out->proxy));
        if (strstr(line, "max_retries=")) out->max_retries = parse_int_val(line, "max_retries");

        if (!nl) break;
        line = nl + 1;
    }
    free(raw);
    return 0;
}

/* ── 任务队列持久化（简化二进制格式）── */
int storage_save_tasks(const char *json_str) {
    (void)json_str;
    /* 暂时用 download_core 自带的 dlmgr_save_state() 写 JSON */
    /* 此处直接保存由 download_core 生成的 tasks.json */
    return 0;
}

char *storage_load_tasks(void) {
    char path[MAX_PATH]; make_path(path, MAX_PATH, "tasks.json");
    return read_text_file(path);
}

/* ── 历史记录（追加文本格式）── */
int storage_add_history(const HistoryItem *item) {
    if (!item) return -1;
    char path[MAX_PATH]; make_path(path, MAX_PATH, "history.dat");

    /* 读已有内容 */
    char *old = read_text_file(path);
    char *new_buf = NULL;
    if (old) {
        size_t olen = strlen(old);
        new_buf = (char*)malloc(olen + 1024);
        strcpy(new_buf, old);
        free(old);
    } else {
        new_buf = (char*)malloc(1024);
        new_buf[0] = '\0';
    }

    char entry[1024];
    snprintf(entry, sizeof(entry),
        "id=%d|url=%s|file=%s|size=%lld|time=%lld|ok=%d\n",
        item->task_id, item->url, item->filename,
        (long long)item->file_size,
        (long long)item->complete_time,
        item->success
    );
    strcat(new_buf, entry);
    int rc = write_text_file(path, new_buf);
    free(new_buf);
    return rc;
}

int storage_load_history(HistoryItem **out, int *count) {
    if (!out || !count) return -1;
    *out = NULL; *count = 0;

    char path[MAX_PATH]; make_path(path, MAX_PATH, "history.dat");
    char *raw = read_text_file(path);
    if (!raw) return 0;

    /* 统计行数 */
    int n = 0;
    for (char *p = raw; *p; p++) if (*p == '\n') n++;
    if (n == 0 && raw[0]) n = 1;

    *out   = (HistoryItem*)calloc((size_t)n, sizeof(HistoryItem));
    *count = 0;

    char *line = raw;
    for (int i = 0; i < n; i++) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        HistoryItem *h = &(*out)[*count];
        memset(h, 0, sizeof(HistoryItem));

        /* 解析 key=value|key=value|... 格式 */
        char *p = line;
        while (p && *p) {
            char *eq = strchr(p, '=');
            char *pipe = strchr(p, '|');
            if (!eq) break;

            *eq = '\0';
            char *key = p;
            char *val = eq + 1;
            if (pipe) *pipe = '\0';

            if (strcmp(key, "id") == 0)
                h->task_id = atoi(val);
            else if (strcmp(key, "url") == 0)
                strncpy(h->url, val, sizeof(h->url)-1);
            else if (strcmp(key, "file") == 0)
                strncpy(h->filename, val, sizeof(h->filename)-1);
            else if (strcmp(key, "size") == 0)
                h->file_size = _atoi64(val);
            else if (strcmp(key, "time") == 0)
                h->complete_time = _atoi64(val);
            else if (strcmp(key, "ok") == 0)
                h->success = atoi(val);

            if (pipe) { *pipe = '|'; p = pipe + 1; }
            else break;
        }

        (*count)++;
        if (!nl) break;
        line = nl + 1;
    }
    free(raw);
    return 0;
}

int storage_clear_history(void) {
    char path[MAX_PATH]; make_path(path, MAX_PATH, "history.dat");
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    return 0;
}

/* ── 网站规则（简化格式，暂存为 dat）── */
int storage_save_site_rules(const SiteRule *rules, int count) {
    (void)rules; (void)count;
    return 0; /* 暂未实现 */
}

int storage_load_site_rules(SiteRule **out, int *count) {
    (void)out; (void)count;
    *out = NULL; *count = 0;
    return 0;
}

/* ── 一次性加载全部数据（程序启动时调用）── */
int storage_load_all(void) {
    /* 自动确定数据目录 */
    if (!g_data_dir[0]) {
        char exe_path[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        char *last_slash = strrchr(exe_path, '\\');
        if (last_slash) *last_slash = '\0';
        snprintf(g_data_dir, MAX_PATH, "%s\\idm_data", exe_path);
    }
    CreateDirectoryA(g_data_dir, NULL);

    /* 加载配置（可选，有默认值兜底） */
    AppConfig cfg;
    storage_load_config(&cfg);

    /* 加载历史记录（用于填充已完成列表） */
    HistoryItem *hist = NULL;
    int hist_cnt = 0;
    storage_load_history(&hist, &hist_cnt);

    /* 注意：任务队列恢复由 download_core 的 dlmgr_load_state 处理，
     * 这里只做配置和历史记录的加载 */

    if (hist) free(hist);  /* 历史仅用于显示，不自动重建下载任务 */
    return 0;
}

/* ── 一次性保存全部数据（程序退出时调用）── */
int storage_save_all(void) {
    if (!g_data_dir[0]) return 0;

    /* 保存当前配置 */
    AppConfig cfg = storage_default_config();
    storage_save_config(&cfg);

    /* 让 download_core 保存其内部状态 */
    char path[MAX_PATH];
    make_path(path, MAX_PATH, "tasks.json");
    dlmgr_save_state(path);

    return 0;
}
