/*
 * storage.h
 * 本地持久化模块 — 基于 cJSON
 * 功能：保存/加载 任务队列、软件配置、历史记录
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─────────────────────────────────────────────
 * 软件配置结构
 * ───────────────────────────────────────────── */
typedef struct {
    char  save_dir[512];       /* 默认下载目录 */
    int   max_threads;         /* 默认并发线程数 */
    int   speed_limit;         /* 全局限速 Bytes/s，0=不限 */
    int   browser_hook_enabled;/* 浏览器嗅探开关 */
    int   auto_start;          /* 添加后自动开始 */
    char  proxy[256];          /* 代理地址，空=不使用 */
    int   max_retries;         /* 分片最大重试次数 */
} AppConfig;

/* 历史记录条目 */
typedef struct {
    int    task_id;
    char   url[2048];
    char   filename[256];
    char   save_path[768];
    int64_t file_size;
    int64_t complete_time;     /* Unix 时间戳（秒） */
    int    success;
} HistoryItem;

/* ─────────────────────────────────────────────
 * 初始化（指定数据文件所在目录）
 * ───────────────────────────────────────────── */
int storage_init(const char *data_dir);  /* 0=成功 */
void storage_cleanup(void);

/* ─────────────────────────────────────────────
 * 配置读写
 * ───────────────────────────────────────────── */
int storage_load_config(AppConfig *out);           /* 0=成功，-1=无文件（用默认值） */
int storage_save_config(const AppConfig *cfg);     /* 0=成功 */
AppConfig storage_default_config(void);

/* ─────────────────────────────────────────────
 * 任务队列读写（与 download_core 协同）
 * ───────────────────────────────────────────── */
int storage_save_tasks(const char *json_str);      /* 直接保存 JSON 字符串 */
char *storage_load_tasks(void);                    /* 返回 malloc 的 JSON 字符串，调用方 free() */

/* ─────────────────────────────────────────────
 * 历史记录
 * ───────────────────────────────────────────── */
int storage_add_history(const HistoryItem *item);
int storage_load_history(HistoryItem **out, int *count); /* 调用方 free(*out) */
int storage_clear_history(void);

/* ─────────────────────────────────────────────
 * 网站适配规则（Referer / UA 白名单）
 * ───────────────────────────────────────────── */
typedef struct {
    char domain[256];
    char referer[256];
    char user_agent[256];
    int  needs_cookie;
} SiteRule;

int storage_load_site_rules(SiteRule **out, int *count);
int storage_save_site_rules(const SiteRule *rules, int count);

/* ─────────────────────────────────────────────
 * 一次性加载/保存（程序启动 / 退出时调用）
 * ───────────────────────────────────────────── */

/* 启动时加载配置 + 历史记录 + 任务状态 */
int  storage_load_all(void);

/* 退出时保存全部数据到磁盘 */
int  storage_save_all(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
