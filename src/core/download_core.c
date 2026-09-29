/*
 * download_core.c
 * 多线程下载调度引擎实现（Windows 原生线程 + CRITICAL_SECTION）
 */
#include "download_core.h"
#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <wchar.h> /* wmemmove / wcsncpy（长路径前缀拼接用） */
#include <io.h>    /* _chsize */
#include <sys/stat.h>  /* fstat, struct stat */

/* Windows 头文件 */
#ifdef _WIN32
#  include <windows.h>
#else
/* 非 Windows 平台桩定义（当前项目仅目标 Windows，此处仅为编译兼容） */
typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* LPVOID;
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define INFINITE 0xFFFFFFFF
#define WINAPI
static void InitializeCriticalSection(void*c){(void)c;}
static void DeleteCriticalSection(void*c){(void)c;}
static void EnterCriticalSection(void*c){(void)c;}
static void LeaveCriticalSection(void*c){(void)c;}
static HANDLE CreateThread(void*a,size_t b,void*(*fn)(void*),void*c,int d,void*e){
    (void)a;(void)b;(void)fn;(void)c;(void)d;(void)e; return INVALID_HANDLE_VALUE;
}
static DWORD WaitForMultipleObjects(DWORD n,HANDLE*h,BOOL a,DWORD m){
    (void)n;(void)h;(void)a;(void)m; return 0;
}
static void CloseHandle(HANDLE h){(void)h;}
static void Sleep(DWORD ms){(void)ms;}
static DWORD GetTickCount64(void){return 0;}
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
#endif

/* ─────────────────────────────────────
 * 全局状态
 * ───────────────────────────────────── */
static DownloadTask    g_tasks[MAX_TASKS];
static int             g_task_count = 0;
static DownloadConfig  g_cfg;
static CRITICAL_SECTION g_lock;

/* 引擎正在拆除：置位后一律不再向 GUI 投递回调。
 * 存在的意义是关掉「worker 在锁内已读出回调指针 → 出锁调用」与
 * 「dlmgr_destroy 摘回调」之间的那个窗口。 */
static volatile int    g_shutdown = 0;

/* 全局请求选项（含代理）：每次网络请求复用，由 dlmgr_set_proxy 更新 */
static NetOptions g_net_opt;
static char       g_proxy_addr_buf[512];
/* 持久化的代理凭据 / UA 字符串缓冲：NetOptions 持有的是 const char*，
 * 需要稳定的存储地址，故用全局缓冲承载（配置变更时整体覆盖）。 */
static char       g_proxy_user_buf[256];
static char       g_proxy_pass_buf[256];
static char       g_ua_buf[512];
/* 引擎默认 UA（与 network_default_options 的 Chrome UA 不同，这里是纯下载器 UA；
 * 若用户在设置中指定了 UA 则覆盖此值）。 */
static const char *kDefaultUA = "IDM-Next/0.1 (Windows; libcurl)";
static int             g_initialized = 0;
static int             g_next_id     = 1;  /* 任务 ID 自增计数器 */

/* 全局限速器（虚拟时钟：累计放行字节 ÷ 限速 = 应耗时，实际超前就休眠补足） */
static CRITICAL_SECTION g_throttle_lock;
static int64_t          g_throttle_window_start = 0;  /* 虚拟时钟基准时刻(ms)，0=未启动 */
static int64_t          g_throttle_bytes       = 0;   /* 自基准起累计放行字节 */
#define THROTTLE_IDLE_RESET_MS 5000                   /* 空闲超过此时长则重置信用，避免之后一次性突发 */

/* ── 引擎行为追踪（默认关闭，零成本）──
 * 设 IDM_TRACE_ENGINE=1 后，把「设置是否真的下发到引擎」这类关键决策打到 stderr：
 * 匹配到的站点凭据、探测结果、分片数、分片失败原因。用于在无调试器的环境里
 * 定位「界面存了盘但引擎没用上」的问题（本项目反复踩过的一类坑）。
 * 不设该变量时只多一次 getenv，生产路径无输出。 */
static int trace_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("IDM_TRACE_ENGINE");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v;
}
#define TRACE(...) do { if (trace_on()) { fprintf(stderr, "[engine] " __VA_ARGS__); \
                         fprintf(stderr, "\n"); fflush(stderr); } } while (0)

#define DOWNLOAD_BLOCK_SIZE (1024 * 1024)  /* 每小块的下载大小（1MB），便于暂停时快速响应 */

/* ── UTF-8 路径安全文件 API ──
 * Windows 的 fopen / DeleteFileA / MoveFileA 走 ANSI 代码页，若路径含 UTF-8 编码的
 * 中文（下载文件名叫中文，或用户名是中文），会生成乱码文件名或操作失败。
 * 这里统一转 UTF-16 后调用宽字符 API，保证非 ASCII 路径正确。 */
/* 把 UTF-8 路径转成宽字符路径，并在超长时补上 Windows `\\?\` 长路径前缀。
 *
 * 背景：Windows 传统上把**完整路径**限制在 260 字符（MAX_PATH）。当保存目录很深、
 * 或文件名很长（典型场景：网盘/相册按日期分的多级子目录、超长标题的视频）时，
 * CreateFileW / _wfopen / MoveFileW / GetFileAttributesW 会直接失败
 * （ERROR_FILENAME_EXCED_RANGE），用户只看到「任务失败」，却不知道是路径太长。
 * 在绝对本地路径前加 `\\?\` 前缀即可把上限放宽到 ~32767 字符。
 *
 * 关键约束（决定实现）：
 *   - `\\?\` 只接受**绝对**路径，且必须是 `\` 分隔（不接受 `/`）；
 *   - 每个路径**分量**（单个目录名/文件名）仍受 255 字符限制 —— 前缀只解「总路径过长」，
 *     不解「单文件名过长」，后者本就属于非法文件名；
 *   - 只在转换后的宽路径长度 ≥ 260 时才加前缀：绝大多数正常路径行为零变化，
 *     只有真正超长的路径才进入长路径模式，避免 `\\?\` 带来的「不规范化」副作用
 *     （它禁用 `..` 折叠、尾随点/空格清理等，可能改变某些特殊路径的语义）；
 *   - 已带 `\\?\` 的路径原样返回，不重复加。 */
static int utf8_to_wpath(const char *path, wchar_t *wpath, int wcap)
{
#ifdef _WIN32
    wchar_t tmp[1024];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, tmp, 1024);
    if (n <= 0) return 0;
    size_t len = (size_t)(n - 1);   /* 不含结尾 \0 的宽字符数 */
    if (len >= 260 && (size_t)wcap > len + 8) {
        if (tmp[0] == L'\\' && tmp[1] == L'?' && tmp[2] == L'\\') {
            /* 已带前缀，原样使用 */
        } else if (tmp[0] == L'\\' && tmp[1] == L'\\') {
            /* UNC 路径：\\server\share\... → \\?\UNC\server\share\... */
            for (wchar_t *q = tmp; *q; q++) if (*q == L'/') *q = L'\\';
            wmemmove(tmp + 8, tmp + 2, len - 1);   /* 腾出 \\?\UNC（8 字符） */
            tmp[0] = L'\\'; tmp[1] = L'\\'; tmp[2] = L'?'; tmp[3] = L'\\';
            tmp[4] = L'U';  tmp[5] = L'N';  tmp[6] = L'C';  tmp[7] = L'\\';
        } else if (tmp[1] == L':') {
            /* 盘符绝对路径 C:\... → \\?\C:\... */
            for (wchar_t *q = tmp; *q; q++) if (*q == L'/') *q = L'\\';
            wmemmove(tmp + 4, tmp, len + 1);
            tmp[0] = L'\\'; tmp[1] = L'\\'; tmp[2] = L'?'; tmp[3] = L'\\';
        }
        /* 相对路径等：无法用 \\?\ 表达，保持原样交系统处理 */
    }
    wcsncpy(wpath, tmp, (size_t)wcap - 1);
    wpath[wcap - 1] = L'\0';
    return 1;
#else
    (void)path; (void)wpath; (void)wcap;
    return 0;
#endif
}

static FILE *utf8_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[1024], wmode[16];
    if (!utf8_to_wpath(path, wpath, 1024)) return NULL;
    if (MultiByteToWideChar(CP_ACP, 0, mode, -1, wmode, 16) <= 0) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

static int utf8_delete(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (!utf8_to_wpath(path, wpath, 1024)) return 0;
    return DeleteFileW(wpath) ? 1 : 0;
#else
    return remove(path) == 0 ? 1 : 0;
#endif
}

static int utf8_move(const char *src, const char *dst) {
#ifdef _WIN32
    wchar_t wsrc[1024], wdst[1024];
    if (!utf8_to_wpath(src, wsrc, 1024)) return 0;
    if (!utf8_to_wpath(dst, wdst, 1024)) return 0;
    return MoveFileW(wsrc, wdst) ? 1 : 0;
#else
    return rename(src, dst) == 0 ? 1 : 0;
#endif
}

static int utf8_file_exists(const char *path) {
#ifdef _WIN32
    wchar_t wpath[1024];
    if (!utf8_to_wpath(path, wpath, 1024)) return 0;
    DWORD a = GetFileAttributesW(wpath);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
#endif
}

/* 目标名已存在时挑一个空闲名：`base (1).ext`、`base (2).ext` …最多试 999 次，
 * 全被占则退到 `base (YYYYMMDD-HHMMSS).ext`。原地改写 filename，并输出完整路径。
 * 返回 1 = 用了新名字，0 = 原名即可（或写入失败）。 */
static int pick_free_filename(char *filename, int cap, const char *dir,
                              char *out, int out_cap) {
    if (!out || out_cap <= 0) return 0;
    out[0] = '\0';                       /* 任何提前返回都留下一个可判定的空串 */
    if (!dir || !dir[0]) return 0;
    if (!filename) return 0;
    if (!filename[0]) {
        /* 兜底：状态文件里出现空 filename 时，绝不能让未初始化的路径进 MoveFileW */
        snprintf(filename, (size_t)cap, "%s", "download.bin");
    }

    snprintf(out, (size_t)out_cap, "%s\\%s", dir, filename);
    if (!utf8_file_exists(out)) return 0;

    /* 拆主名与扩展名：以最后一个 '.' 为界；'.' 在首位（.bashrc 这类）视为无扩展名 */
    char base[300], ext[64];
    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename && strlen(dot) < sizeof(ext)) {
        size_t bl = (size_t)(dot - filename);
        if (bl >= sizeof(base)) bl = sizeof(base) - 1;
        memcpy(base, filename, bl); base[bl] = '\0';
        strcpy(ext, dot);
    } else {
        snprintf(base, sizeof(base), "%s", filename);
        ext[0] = '\0';
    }

    char cand[400];
    for (int n = 1; n <= 999; n++) {
        snprintf(cand, sizeof(cand), "%s (%d)%s", base, n, ext);
        snprintf(out, (size_t)out_cap, "%s\\%s", dir, cand);
        if (!utf8_file_exists(out)) {
            snprintf(filename, (size_t)cap, "%s", cand);
            return 1;
        }
    }
    /* 极端情况：同名副本已达 999 个，用时间戳保证唯一 */
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    tmv = *localtime(&now);
#endif
    snprintf(cand, sizeof(cand), "%s (%04d%02d%02d-%02d%02d%02d)%s",
             base, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ext);
    snprintf(out, (size_t)out_cap, "%s\\%s", dir, cand);
    snprintf(filename, (size_t)cap, "%s", cand);
    return 1;
}

/* 把「打开/重命名文件失败」翻译成用户能看懂的一句话，并保留路径。
 *
 * 以前全项目只有一句「无法打开文件: <路径>」：磁盘满、没有写入权限、保存目录被删、
 * 文件名非法、文件被别的程序占着 —— 提示长得一模一样，用户只能看到一个路径，
 * 根本不知道该做什么。失败原因（errno / GetLastError）在调用点还是准的，就在这里翻。
 * 原因文字放**前面**：error_msg 只有 256 字节，长路径会把后半段挤掉。 */
static void describe_file_error(const char *action, const char *path,
                                char *out, size_t cap) {
    int   e = errno;
    DWORD w = GetLastError();
    const char *why = NULL;

#ifdef _WIN32
    switch (w) {
    case ERROR_DISK_FULL:            why = "磁盘空间不足"; break;
    case ERROR_HANDLE_DISK_FULL:     why = "磁盘空间不足"; break;
    case ERROR_ACCESS_DENIED:        why = "没有权限（或文件被其他程序占用）"; break;
    case ERROR_SHARING_VIOLATION:    why = "文件被其他程序占用"; break;
    case ERROR_WRITE_PROTECT:        why = "目标磁盘是只读的"; break;
    case ERROR_NOT_ENOUGH_QUOTA:     why = "超出磁盘配额"; break;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:       why = "保存路径不存在（目录可能已被删除）"; break;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:         why = "文件名或路径含有非法字符"; break;
    case ERROR_FILENAME_EXCED_RANGE: why = "文件路径过长（Windows 默认上限 260 字符）"; break;
    default: break;
    }
#endif
    if (!why) {
        switch (e) {
        case ENOSPC: why = "磁盘空间不足"; break;
        case EACCES: why = "没有权限（或文件被其他程序占用）"; break;
        case ENOENT: why = "保存路径不存在（目录可能已被删除）"; break;
        case EMFILE: why = "打开的文件过多（句柄耗尽）"; break;
        case EINVAL: why = "文件名或路径非法"; break;
        default: break;
        }
    }
    if (!why) why = "未知原因";

    snprintf(out, cap, "%s失败：%s（%s）", action, why, path);
}

/* 逐级创建目录（UTF-8 路径）。CreateDirectoryW 一次只能建一级、父目录不存在就直接
 * 失败，所以从卷标之后开始逐段建；目录已存在视为成功。
 *
 * 为什么放在引擎里、而不是只靠 GUI 的 mkpath：引擎还有别的入口 —— 从状态文件恢复的
 * 任务（用户可能已经把那个目录删了）、CLI、以及把 download_core 当库直接用的调用方。
 * 目录缺失时 utf8_fopen(tmp_path) 会立刻失败，用户只看到「任务失败」，根本看不出
 * 是路径问题。引擎自己兜底，任何入口都不会因此失败。 */
/* 创建单个目录（已处理长路径 `\\?\` 前缀）。CreateDirectoryW 一次只建一级，
 * 父目录不存在就失败，所以由 ensure_dir_utf8 逐级调用。 */
static void create_one_dir_w(wchar_t *p)
{
    size_t len = wcslen(p);
    if (len >= 260 && p[1] == L':') {
        for (wchar_t *q = p; *q; q++) if (*q == L'/') *q = L'\\';
        wchar_t buf[1100];
        if (len + 8 <= (int)(sizeof(buf) / sizeof(buf[0]))) {
            wmemmove(buf + 4, p, len + 1);
            buf[0] = L'\\'; buf[1] = L'\\'; buf[2] = L'?'; buf[3] = L'\\';
            CreateDirectoryW(buf, NULL);
            return;
        }
    }
    CreateDirectoryW(p, NULL);
}

static int ensure_dir_utf8(const char *dir) {
    if (!dir || !dir[0]) return 0;
#ifdef _WIN32
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 1024) <= 0) return 0;

    for (wchar_t *q = w; *q; q++) if (*q == L'/') *q = L'\\';

    wchar_t *p = w;
    if (p[0] && p[1] == L':')                p += 2;   /* "C:\..."：跳过卷标 */
    else if (p[0] == L'\\' && p[1] == L'\\') p += 2;   /* UNC "\\server\..."：跳过前两斜杠 */

    for (; *p; p++) {
        if (*p != L'\\') continue;
        *p = L'\0';
        create_one_dir_w(w);   /* 中间级：失败也无妨，末级会给出结论 */
        *p = L'\\';
    }
    create_one_dir_w(w);   /* 末级（可能超长 → 内部加前缀） */
    return 1;
#else
    /* 非 Windows：逐级 mkdir，忽略「已存在」。当前目标平台只有 Windows，尽力而为。 */
    char tmp[1024];
    size_t n = strlen(dir);
    if (n >= sizeof(tmp)) return 0;
    memcpy(tmp, dir, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0755);
        *p = '/';
    }
    mkdir(tmp, 0755);
    return 1;
#endif
}

/* ─────────────────────────────────────
 * 内部工具
 * ───────────────────────────────────── */
int64_t dl_time_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    return (int64_t)time(NULL) * 1000;
#endif
}

/* Windows 保留设备名：以这些名字（扩展名之前的部分）命名的文件即使语法合法
 * 也创建不了 —— CON/PRN/AUX/NUL/COM1-9/LPT1-9。命中就加前缀 '_'。 */
static int is_reserved_device_name(const char *name) {
    static const char *kNames[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    char base[8];
    int  n = 0;
    for (const char *p = name; *p && *p != '.' && n < 7; p++) {
        unsigned char c = (unsigned char)*p;
        base[n++] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    base[n] = '\0';
    if (!base[0]) return 0;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        if (strcmp(base, kNames[i]) == 0) return 1;
    return 0;
}

/* 文件名清理（引擎层唯一入口）。
 *
 * 背景：`<>:"/\|?*` 和控制字符在 Windows 上会让 CreateFileW 直接返回
 * ERROR_INVALID_NAME；尾随空格与点会被文件系统**静默吞掉**，表现为「任务显示
 * 下载成功，但用户按列表里的名字去磁盘上找却找不到」。这两种失败以前都没有
 * 任何处理 —— URL 带查询串（`?...`）时连 `?` 都会原样进文件名。
 *
 * 三处调用：用户显式文件名 / URL 推断名 / Content-Disposition 建议名。
 * 注：按**字节**清理，UTF-8 多字节序列的每个字节都 >0x7F，不会命中 ':' 这类
 * ASCII 保留字符，所以中文与 emoji 文件名原样保留。 */
static void sanitize_filename_utf8(const char *in, char *out, int max_len) {
    if (!out || max_len <= 0) return;
    if (max_len < 2) { out[0] = '\0'; return; }
    if (!in) in = "";

    int n = 0;
    for (const char *p = in; *p && n < max_len - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) continue;              /* 控制字符直接丢弃 */
        if (strchr("<>:\"/\\|?*", (int)c)) c = '_';
        out[n++] = (char)c;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) n--;  /* 尾随空格/点 */
    out[n] = '\0';

    if (n == 0) {                                          /* 全被清掉 → 兜底名 */
        strncpy(out, "download.bin", (size_t)(max_len - 1));
        out[max_len - 1] = '\0';
        return;
    }
    if (is_reserved_device_name(out)) {
        char tmp[520];
        snprintf(tmp, sizeof(tmp), "_%s", out);
        strncpy(out, tmp, (size_t)(max_len - 1));
        out[max_len - 1] = '\0';
    }
}

/* 从 URL 推断文件名（供引擎内部与外部调用）。
 * 修掉两个真实缺陷：
 *   ① 长度按「URL 起点到 ?」算，却从路径最后一段起拷 —— 带查询串的 URL 会把
 *      `?a=b` 一起当文件名（`a.txt?x=1`），CreateFile 必然失败；
 *   ② 完全不做字符清理（见 sanitize_filename_utf8）。
 * 另加 %XX 解码：`My%20File.zip` 以前会存成带 %20 的怪名字。 */
void dl_infer_filename(const char *url, char *out, int max_len) {
    if (!url || !out || max_len <= 0) return;
    if (max_len < 2) { out[0] = '\0'; return; }

    const char *q = strchr(url, '?');                     /* 砍查询串 */
    size_t url_len = q ? (size_t)(q - url) : strlen(url);
    const char *h = (const char *)memchr(url, '#', url_len);   /* 砍 fragment */
    if (h) url_len = (size_t)(h - url);

    const char *fname = url;
    for (const char *p = url; (size_t)(p - url) < url_len; p++)
        if (*p == '/') fname = p + 1;
    size_t len = url_len - (size_t)(fname - url);

    /* %XX 解码（只处理合法十六进制对，其余原样保留） */
    char raw[512];
    size_t rn = 0;
    for (size_t i = 0; i < len && rn < sizeof(raw) - 1; i++) {
        if (fname[i] == '%' && i + 2 < len) {
            int hi = -1, lo = -1;
            unsigned char a = (unsigned char)fname[i + 1];
            unsigned char b = (unsigned char)fname[i + 2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                raw[rn++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        raw[rn++] = fname[i];
    }
    raw[rn] = '\0';

    /* 空名字（URL 以 / 结尾）交给清理函数走 download.bin 兜底 */
    sanitize_filename_utf8(rn ? raw : "download.bin", out, max_len);
}

/* 全局限速节流：在分片线程（线程池 worker）每下载一块后调用。
 *
 * 虚拟时钟模型：把「累计放行字节」换算成「应当消耗的时间」，实际耗时落后
 * 于该值就休眠补足差额。与块大小无关 —— 单次写入是 64 KB 还是 4 MB，
 * 长期速率都收敛到 speed_limit_global。
 *
 * 与旧的 100ms 滑动窗口实现相比，实测精度相当（1 MiB @ 256 KB/s：新 4188 ms /
 * 旧 4140 ms，理论 4000 ms），真正的差别在**响应性**：旧实现算出的 sleep_ms 可以
 * 到秒级且一次睡到底，暂停/取消要等它睡完才生效；这里切成不超过 250ms 的片，
 * 暂停最迟 250ms 内响应。另外空闲超过 5 秒重置信用，长时间暂停后恢复不攒突发。 */
static void throttle_limit(int64_t bytes) {
    if (g_cfg.speed_limit_global <= 0 || bytes <= 0) return;

    int64_t sleep_ms = 0;
    EnterCriticalSection(&g_throttle_lock);
    int64_t now = dl_time_ms();
    if (g_throttle_window_start == 0) g_throttle_window_start = now;

    int64_t used_ms = now - g_throttle_window_start;
    /* 空闲过久（分片间隙、暂停）→ 重置信用，否则恢复后会一次性突发 */
    if (used_ms > g_throttle_bytes * 1000 / g_cfg.speed_limit_global + THROTTLE_IDLE_RESET_MS) {
        g_throttle_window_start = now;
        g_throttle_bytes = 0;
        used_ms = 0;
    }

    g_throttle_bytes += bytes;
    int64_t want_ms = g_throttle_bytes * 1000 / g_cfg.speed_limit_global;
    sleep_ms = want_ms - used_ms;
    LeaveCriticalSection(&g_throttle_lock);

    while (sleep_ms > 0) {
        DWORD s = (sleep_ms > 250) ? 250 : (DWORD)sleep_ms;
        Sleep(s);
        sleep_ms -= s;
    }
}

/* 限速值变更时重置虚拟时钟：否则会拿旧的累计字节按新速率去算应耗时，
 * 表现为「刚调完限速的那一块要么不动、要么猛冲」。 */
static void throttle_reset(void) {
    EnterCriticalSection(&g_throttle_lock);
    g_throttle_window_start = 0;
    g_throttle_bytes = 0;
    LeaveCriticalSection(&g_throttle_lock);
}

/* 任务数组始终按 task_id 升序排列（add 追加递增 id；remove 用 memmove 保序；
 * load 完成后显式排序——文件是外部可编辑的输入，不能指望它本来就有序），
 * 故用二分查找 O(log n) 取代线性扫描 O(n)。 */
static DownloadTask *find_task(int task_id) {
    int lo = 0, hi = g_task_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int id = g_tasks[mid].task_id;
        if (id == task_id) return &g_tasks[mid];
        if (id < task_id) lo = mid + 1;
        else              hi = mid - 1;
    }
    return NULL;
}

/* 就地摘除某个槽位（memmove 保序）。**调用者必须已持有 g_lock**，
 * 且必须确认该槽位没有活动线程（worker_alive == 0）——有线程时挪动槽位会让
 * 线程手里的指针指向别的任务。 */
static int detach_slot(int task_id) {
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].task_id == task_id) {
            memmove(&g_tasks[i], &g_tasks[i + 1],
                    (size_t)(g_task_count - i - 1) * sizeof(DownloadTask));
            g_task_count--;
            return 1;
        }
    }
    return 0;
}

/* 生效的每任务并发连接数：由「请求值」与「每服务器连接数上限」取小。
 * 一个下载任务基本就是对同一台主机开 N 条连接，所以设置页的「每服务器连接数」
 * 天然就是每任务并发数的上限（与 FDM 的 max connections per server 同义）。
 * 两者中任一为 0/负数视为不限，用另一个；都无效则回落到 1。 */
static int effective_threads(int requested)
{
    int a = requested;
    int b = g_cfg.max_conn_per_server;
    if (a <= 0) a = b;
    if (b <= 0) b = a;
    int v = (a < b) ? a : b;
    if (v <= 0) v = 1;
    if (v > MAX_CHUNKS_PER_TASK) v = MAX_CHUNKS_PER_TASK;
    return v;
}

/* 取 URL 的主机名（小写不转换，比较时用 _stricmp）。无主机名则写出空串。 */
static void url_host_of(const char *url, char *out, int outlen)
{
    out[0] = '\0';
    if (!url) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && i < outlen - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* 把「站点登录」凭据按 URL 匹配到任务上（设置页该项原本只存盘、引擎从不下发）。
 * 匹配规则：
 *   match 含 "://"  → 当作 URL 前缀匹配
 *   否则            → 当作域名匹配，支持子域（match="example.com" 命中 "files.example.com"）
 * 命中后把凭据指针指向任务自己的缓冲区（不能用临时/全局缓冲，否则多任务会互相覆盖）。 */
static void apply_site_auth(DownloadTask *t)
{
    t->net_opts = g_net_opt;            /* 先继承全局选项（代理/UA/超时/SSL…） */
    t->auth_user[0] = '\0';
    t->auth_pass[0] = '\0';
    t->net_opts.auth_user = NULL;
    t->net_opts.auth_pass = NULL;
    if (g_cfg.site_login_count <= 0 || !t->url[0]) {
        TRACE("auth: 未配置站点登录（条数=%d）→ 请求不带凭据  url=%s",
              g_cfg.site_login_count, t->url);
        return;
    }

    char host[256];
    url_host_of(t->url, host, sizeof(host));

    for (int i = 0; i < g_cfg.site_login_count && i < MAX_SITE_LOGINS; i++) {
        const SiteCredential *c = &g_cfg.site_logins[i];
        if (!c->match[0] || !c->user[0]) continue;

        int hit = 0;
        if (strstr(c->match, "://")) {
            size_t n = strlen(c->match);
            hit = (_strnicmp(t->url, c->match, n) == 0);
        } else if (host[0]) {
            if (_stricmp(host, c->match) == 0) hit = 1;
            else {
                size_t hl = strlen(host), ml = strlen(c->match);
                /* 子域匹配：host = xxx.<match> */
                if (hl > ml + 1 && host[hl - ml - 1] == '.'
                    && _stricmp(host + hl - ml, c->match) == 0)
                    hit = 1;
            }
        }
        if (hit) {
            strncpy(t->auth_user, c->user, sizeof(t->auth_user) - 1);
            t->auth_user[sizeof(t->auth_user) - 1] = '\0';
            strncpy(t->auth_pass, c->pass, sizeof(t->auth_pass) - 1);
            t->auth_pass[sizeof(t->auth_pass) - 1] = '\0';
            t->net_opts.auth_user = t->auth_user;
            t->net_opts.auth_pass = t->auth_pass;
            TRACE("auth: host=%s 命中站点登录 match=\"%s\" user=\"%s\" → 已下发 HTTP 认证",
                  host, c->match, c->user);
            return;   /* 首条命中即生效 */
        }
    }
    TRACE("auth: host=%s 未命中任何站点登录（共 %d 条）→ 请求不带凭据",
          host, g_cfg.site_login_count);
}

/* 计算分片 */
static void calc_chunks(DownloadTask *t) {
    if (t->file_size <= 0 || t->file_size < MIN_CHUNK_SIZE) {
        t->chunk_count = 1;
        t->chunks[0].chunk_id   = 0;
        t->chunks[0].start      = 0;
        t->chunks[0].end        = t->file_size > 0 ? t->file_size - 1 : -1;
        t->chunks[0].downloaded = 0;
        t->chunks[0].done       = 0;
        t->chunks[0].retry_count = 0;
        return;
    }
    int cnt = t->thread_count;
    if (cnt <= 0) cnt = 1;
    /* 细分：保证每片不超过默认片大小，使线程池能动态调度（work-stealing），
     * 避免“片数=线程数”时某片偏慢拖垮整体速度。 */
    int by_size = (int)(t->file_size / DEFAULT_CHUNK_SIZE);
    if (by_size > cnt) cnt = by_size;
    if (cnt > MAX_CHUNKS_PER_TASK) cnt = MAX_CHUNKS_PER_TASK;
    if (cnt <= 0) cnt = 1;
    t->chunk_count = cnt;
    int64_t chunk_sz = t->file_size / cnt;
    for (int i = 0; i < cnt; i++) {
        t->chunks[i].chunk_id   = i;
        t->chunks[i].start      = (int64_t)i * chunk_sz;
        t->chunks[i].end        = (i == cnt - 1) ? (t->file_size - 1)
                                                     : ((int64_t)(i + 1) * chunk_sz - 1);
        t->chunks[i].downloaded = 0;
        t->chunks[i].done       = 0;
        t->chunks[i].retry_count = 0;
    }
}

/* ─────────────────────────────────────
 * 分片下载线程
 * ───────────────────────────────────── */
/* 网络层在重试等待开始前打来的通知：把「服务器让我们等到什么时候」记到任务上。
 * 只记绝对截止时刻，换算剩余秒数是读侧（dlmgr_get_task_info）的事。
 * 注意这是在下载线程上被调用，g_lock 此刻由本函数自己持有，不要在外面等它。 */
static void on_net_wait(int http_code, long wait_ms, void *ud) {
    DownloadTask *t = (DownloadTask*)ud;
    if (!t || wait_ms <= 0) return;
    EnterCriticalSection(&g_lock);
    t->throttle_http      = http_code;
    t->throttle_until_ms  = dl_time_ms() + (int64_t)wait_ms;
    LeaveCriticalSection(&g_lock);
}

/* 单个分片的下载逻辑（被线程池动态调度）：整个分片只打开一次文件句柄，
 * 循环下载各小块并写入，减少 open/close 开销（磁盘缓存）。 */
static int download_one_chunk(DownloadTask *t, int cid) {
    if (!t || cid < 0 || cid >= t->chunk_count) return 1;

    DownloadChunk *c = &t->chunks[cid];
    if (c->done) return 0;

    /* c->end < 0 表示「从 start 一直下到流末尾」——即服务器没给出文件大小
     * （HEAD 被拒 401/403、分块传输、动态端点等）。
     * ⚠️ 这种情况下**不能**用 end - start + 1 算长度：那会得到 0，整个分片被当成
     * 空片直接标记完成，最终产出一个 0 字节的「下载完成」文件（真实踩过的坑）。
     * 这里改为一次性流式下载（range_end = -1 → 不发 Range 头，取整个响应体）。 */
    const int unknown_len = (c->end < 0);
    int64_t total_in_chunk = unknown_len ? -1 : (c->end - c->start + 1);
    int max_retry = g_cfg.retry_max > 0 ? g_cfg.retry_max : 3;
    char last_err[256] = "";

    /* 本分片打开一次文件句柄（整个任务共享同一 tmp 文件，各片写不同偏移、互不干扰） */
    FILE *fp = utf8_fopen(t->tmp_path, "r+b");
    if (!fp) fp = utf8_fopen(t->tmp_path, "w+b");
    if (!fp) {
        char why[320];
        describe_file_error("打开临时文件", t->tmp_path, why, sizeof(why));
        EnterCriticalSection(&g_lock);
        t->status = TASK_FAILED;
        snprintf(t->error_msg, sizeof(t->error_msg), "%s", why);
        LeaveCriticalSection(&g_lock);
        return 1;
    }

    /* 连接复用：本分片开一个 easy handle，块与块之间用它，分片结束才关。
       分片下载必须每 1MiB 发一个请求才能在块边界响应暂停/取消，
       但「每块一个新 handle」等于每块重做 TCP（+TLS）握手 —— 12MiB 就是 12 次。
       handle 只属于这条分片线程，绝不跨线程传，所以不需要任何锁。
       拿不到 handle 时 nh 为 NULL，下面原样退回旧行为，不影响正确性。 */
    NetHandle *nh = net_handle_open();

    for (int r = 0; r <= max_retry; r++) {
        int any_failure = 0;
        int fatal_no_retry = 0;   /* 磁盘满/无权限这类确定性失败：重试不会变好 */

        /* 将整个分片拆成 DOWNLOAD_BLOCK_SIZE 的小块循环下载
         * 每块之间检查暂停/取消状态，实现优雅中断。
         * 长度未知时只跑一轮（一次请求取完整个响应体）。 */
        while (unknown_len || c->downloaded < total_in_chunk) {
            /* ── 检查暂停 / 取消状态 ── */
            EnterCriticalSection(&g_lock);
            if (t->status == TASK_PAUSED || t->status == TASK_CANCELLED) {
                LeaveCriticalSection(&g_lock);
                fclose(fp);
                net_handle_close(nh);
                return 0;  /* 优雅退出，保留 c->downloaded 作为断点 */
            }
            LeaveCriticalSection(&g_lock);

            /* 计算本次下载的范围（最多 DOWNLOAD_BLOCK_SIZE） */
            int64_t block_start = c->start + c->downloaded;
            int64_t remaining   = unknown_len ? -1 : (total_in_chunk - c->downloaded);
            int64_t block_size  = unknown_len ? -1
                                 : ((remaining > DOWNLOAD_BLOCK_SIZE)
                                    ? DOWNLOAD_BLOCK_SIZE : remaining);
            int64_t block_end   = unknown_len ? -1 : (block_start + block_size - 1);

            NetDownloadTask nd = {0};
            nd.url         = t->url;
            nd.save_path   = t->tmp_path;
            nd.range_start = block_start;
            nd.range_end   = block_end;
            nd.opt         = &t->net_opts;   /* 本任务选项（含代理 + 站点认证） */
            nd.handle      = nh;              /* 复用同一条连接 */
            nd.notice_wait = on_net_wait;    /* 429/503 的等待要在界面上看得见，见其注释 */
            nd.notice_ud   = t;

            NetDownloadResult result = network_download_range_fp(&nd, fp);
            if (!result.success && result.range_ignored && t->chunk_count > 1) {
                /* 服务器不支持 Range：继续按分片写只会产出内容错位的坏文件。
                 * 停下所有分片，交给监督线程在全部退出后改成单连接重下（见函数尾）。 */
                EnterCriticalSection(&g_lock);
                t->range_downgrade = 1;
                t->status = TASK_CANCELLED;   /* 让其余分片也在下一块边界退出 */
                LeaveCriticalSection(&g_lock);
                TRACE("chunk %d: 服务器不支持 Range，降级为单连接重下", cid);
                fclose(fp);
                net_handle_close(nh);
                return 0;
            }
            if (!result.success) {
                any_failure = 1;
                /* 记下底层原因（如「HTTP 错误: 401」），用于回填任务错误文案，
                 * 否则用户只看得到笼统的「分片下载失败」。 */
                if (result.error_msg[0]) {
                    strncpy(last_err, result.error_msg, sizeof(last_err) - 1);
                    last_err[sizeof(last_err) - 1] = '\0';
                }
                TRACE("chunk %d: 块下载失败（range=%lld..%lld http=%ld）%s",
                      cid, (long long)block_start, (long long)block_end,
                      result.http_code, result.error_msg);
                if (result.no_retry) fatal_no_retry = 1;
                break;  /* 本块失败，跳出小块循环，触发外层重试 */
            }

            EnterCriticalSection(&g_lock);
            c->downloaded += result.bytes_written;
            t->downloaded += result.bytes_written;
            /* 字节真的开始进来了 = 限流等待已经过去，立刻清掉倒计时。
             * 不能只靠「到点自动失效」：服务器提前放行时，那几秒里界面还在喊
             * 「N 秒后重试」，说反话比信息滞后更糟。 */
            if (t->throttle_until_ms) { t->throttle_until_ms = 0; t->throttle_http = 0; }
            /* 记录分片下载实际协商到的协议版本（首个非空即锁定，多线程同主机一致） */
            if (!t->http_version[0] && result.http_version[0]) {
                strncpy(t->http_version, result.http_version, sizeof(t->http_version) - 1);
                t->http_version[sizeof(t->http_version) - 1] = '\0';
            }

            /* 速度估算：全程平均值作兜底初值 + 滑动窗口 EMA(α=0.3) 平滑 */
            int64_t now = dl_time_ms();
            int64_t avg = 0;
            if (t->start_time > 0 && now > t->start_time)
                avg = (int64_t)(t->downloaded * 1000 / (now - t->start_time));

            if (t->start_time > 0) {
                if (avg > 0 && t->speed_ema == 0)
                    t->speed_ema = avg;   /* 首窗用平均值播种，避免初期为 0 */
                if (t->ema_last_ms == 0)
                    t->ema_last_ms = now;
                int64_t dt = now - t->ema_last_ms;
                if (dt < 0) dt = 0;
                t->ema_bytes     += result.bytes_written;
                t->ema_window_ms += dt;
                t->ema_last_ms    = now;
                /* 每累积约 800ms 刷新一次指数滑动平均 */
                if (t->ema_window_ms >= 800 && t->ema_bytes > 0) {
                    int64_t inst = t->ema_bytes * 1000 / t->ema_window_ms;
                    t->speed_ema = (t->speed_ema * 7 + inst * 3) / 10;
                    t->ema_bytes     = 0;
                    t->ema_window_ms = 0;
                }
                t->speed_bps = t->speed_ema ? t->speed_ema : avg;
                if (t->file_size > t->downloaded && t->speed_bps > 0)
                    t->eta_sec = (int)((t->file_size - t->downloaded) / t->speed_bps);
            }

            /* 在锁外调用外部回调：避免持有全局锁时重入引擎造成死锁，并缩短锁持有时间
             * 节流：每块 1MB 写盘即回调一次会在高并发下造成跨线程信号泛滥（频繁唤醒 GUI 事件循环）。
             *       仅当距上次回调 >=100ms 或本块为分片末块（强制刷最终值）时才真正下发；
             *       被跳过的增量值由 4Hz 下行刷新路径兜底，不影响进度显示正确性。 */
            {
                int64_t cb_dl = t->downloaded;
                int64_t cb_fs = t->file_size;
                int     cb_sp = (int)t->speed_bps;
                int     cb_id = t->task_id;
                void (*cb_fn)(int,int64_t,int64_t,int,void*) =
                    g_shutdown ? NULL : t->progress_cb;
                void  *cb_ud = g_shutdown ? NULL : t->progress_ud;

                int is_last_block = (c->downloaded >= total_in_chunk) ? 1 : 0;
                if (is_last_block || (now - t->last_progress_cb_ms) >= 100) {
                    t->last_progress_cb_ms = now;
                    LeaveCriticalSection(&g_lock);
                    if (cb_fn) cb_fn(cb_id, cb_dl, cb_fs, cb_sp, cb_ud);
                } else {
                    LeaveCriticalSection(&g_lock);
                }
            }

            /* 全局限速：按 100ms 窗口节流总带宽 */
            throttle_limit(result.bytes_written);

            /* 长度未知时一次请求即取完整个响应体，必须跳出，
             * 否则 unknown_len 恒为真会无限循环。
             * ⚠️ 但这条路径**没有任何字节数可对账**：服务器提前断流时底层照样是
             * 200 + 有字节，旧代码连着 break → c->done=1 → 任务显示「已完成」，
             * 用户拿到的却是一个被截断的文件（与既往「大小对、内容坏」同族，
             * 这次连大小都不对，更隐蔽）。已知总长度的分片没这问题——
             * while (c->downloaded < total_in_chunk) 本身就会再发一次 Range 补齐。
             * 所以断流在这里改判未完成：已落盘的字节保留在 c->downloaded 里，
             * 下一轮重试自动带 Range 从断点续传；续不上（服务器不吃 Range）
             * 就重试到上限后诚实报失败，绝不产出一个假装完成的半截文件。 */
            if (unknown_len) {
                if (result.truncated) {
                    any_failure = 1;
                    snprintf(last_err, sizeof(last_err),
                             "连接中断：服务器提前断流（已收到 %lld 字节，正在续传）",
                             (long long)result.bytes_written);
                    TRACE("chunk %d: 长度未知且响应被截断，按断点续传重试", cid);
                    break;
                }
                break;   /* 响应体完整收尾，单流下载结束 */
            }
        }

        if (!any_failure) {
            /* 所有小块全部成功 */
            EnterCriticalSection(&g_lock);
            c->done = 1;
            LeaveCriticalSection(&g_lock);
            fclose(fp);
            net_handle_close(nh);
            return 0;
        }

        /* 小块失败后重试（从断点继续） */
        if (fatal_no_retry) break;    /* 确定性失败（磁盘满/无权限）不再白等重试 */
        c->retry_count++;
        if (r < max_retry)
            Sleep(1000 * (DWORD)(r + 1));
    }

    /* 所有重试失败 */
    int cb_id = t->task_id;
    void (*cb_fn)(int,int,void*) = NULL;
    void *cb_ud = NULL;
    EnterCriticalSection(&g_lock);
    t->status = TASK_FAILED;
    /* 优先把底层的具体原因（HTTP 401/404、无法连接…）透出去，GUI 才能显示
     * 「HTTP 错误: 401」而不是无信息量的「分片下载失败」。 */
    if (last_err[0])
        snprintf(t->error_msg, sizeof(t->error_msg), "%s", last_err);
    else
        snprintf(t->error_msg, sizeof(t->error_msg),
                 "分片 %d 下载失败，已重试 %d 次", cid, max_retry);
    cb_fn = g_shutdown ? NULL : t->complete_cb;
    cb_ud = g_shutdown ? NULL : t->complete_ud;
    LeaveCriticalSection(&g_lock);

    if (cb_fn) cb_fn(cb_id, 0, cb_ud);

    fclose(fp);
    net_handle_close(nh);
    return 1;
}

/* 线程池工作线程：循环从 dispatch_idx 领取未完成的片并下载（动态调度 / work-stealing）。
 * 先完成的线程自动领取后续分片，负载更均衡，避免“片=线程”时慢片拖垮整体。 */
static DWORD WINAPI pool_worker(LPVOID arg) {
    DownloadTask *t = (DownloadTask*)arg;
    if (!t) return 1;
    for (;;) {
        EnterCriticalSection(&g_lock);
        int i = t->dispatch_idx++;
        LeaveCriticalSection(&g_lock);
        if (i >= t->chunk_count) break;   /* 无更多片，退出 */
        if (t->chunks[i].done) continue;  /* 续传：跳过已完成片 */
        download_one_chunk(t, i);
    }
    return 0;
}

/* ─────────────────────────────────────
 * 监督线程：启动所有分片，等待完成，重命名文件
 * ───────────────────────────────────── */
static DWORD WINAPI start_task_threads(LPVOID arg) {
    DownloadTask *t = (DownloadTask*)arg;
    if (!t) return 1;

    /* 先登记「本槽位有活动线程」，再做任何事：worker_alive > 0 期间 dlmgr_remove
     * 不会 memmove 数组，t 指针在整个函数体内始终指向同一个任务。
     * 启动瞬间就已被删除的任务直接就地摘除槽位后返回。 */
    EnterCriticalSection(&g_lock);
    if (t->removed) {
        int tid = t->task_id;
        detach_slot(tid);
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    t->worker_alive++;
    LeaveCriticalSection(&g_lock);

    /* 状态立刻置「下载中」，必须放在探测之前。
     * ⚠️ 探测已经从调用线程搬到本函数里（见下面的 need_probe 段），如果把状态留到
     *    探测之后才改，用户点「开始」/定时到点拉起之后，任务行在整段探测期间
     *    还写着「已暂停」——实测定时探针就是这样判成"没被拉起"的（status 停在 2）。
     *    这里先置位，界面当下就能反映"已经拉起来了"。 */
    EnterCriticalSection(&g_lock);
    t->status = TASK_RUNNING;
    t->error_msg[0] = '\0';   /* 清掉上一次失败留下的原因，否则重试时 GUI 还显示旧错误 */
    LeaveCriticalSection(&g_lock);

    /* 保存目录兜底：不存在就逐级建出来。放在这里（而不是 dlmgr_add）是因为
     * 从状态文件恢复的任务不经过 add，而所有下载都必然经过本函数。
     * 取目录的规则必须与下面拼 tmp_path 的规则一致，否则建了 A 却往 B 里写。 */
    ensure_dir_utf8(t->save_dir[0] ? t->save_dir : g_cfg.default_save_dir);

    /* ── 探测：文件大小 + Range 支持 + 服务器建议文件名 ──
     * 原来这段在 dlmgr_start 里，也就是**在 GUI 线程上**跑；一次 HEAD 探测最坏挂满
     * connect_timeout（实测慢服务器 3000ms 起步，「全部开始」还要逐个串着卡）。
     * 搬到监督线程里之后，点「开始」立刻返回，界面该转圈转圈。
     * ⚠️ 判据仍是「本任务还没分过段」（chunk_count == 0）而**不是**「状态是 PENDING」：
     *   ① 新建后立刻被暂停的任务是 PAUSED，用状态判断会跳过探测、chunk_count 停在 0，
     *      下面拿着 0 个分片空转（状态显示下载中，实际一动不动）；
     *   ② 从状态文件恢复、chunks 为空数组的任务同理。
     * 分片一旦算过（含"长度未知"的单流兜底 chunk_count=1）就保持，续传靠 c->downloaded。
     * 这里可以直接继续用 t，不必像老代码那样"探测完再 find_task 一遍"：
     * 本函数开头已 worker_alive++，槽位在此期间不会被 detach_slot 挪动（见该字段注释），
     * 顺带把「探测期间任务被删除」这一类竞态也堵掉了。 */
    EnterCriticalSection(&g_lock);
    /* 每次启动都重算一次本任务的网络选项：设置可能在任务创建之后才改（代理/UA/站点登录），
     * 而且从状态文件恢复的任务 net_opts 是空的，必须补上。 */
    apply_site_auth(t);
    int need_probe = (t->chunk_count == 0) ? 1 : 0;
    LeaveCriticalSection(&g_lock);

    if (need_probe) {
        /* 单请求探测：文件大小 + Range 支持（2 RTT → 1 RTT）。锁外做，别占着锁等网络。 */
        NetworkProbe probe = network_probe(t->url, &t->net_opts);
        int64_t fsize   = probe.file_size;
        int     supports = probe.supports_range;
        TRACE("probe: success=%d http=%ld size=%lld range=%d ver=%s （auth_user=%s）",
              probe.success, probe.http_code, (long long)fsize, supports,
              probe.http_version,
              t->net_opts.auth_user ? t->net_opts.auth_user : "(null)");

        EnterCriticalSection(&g_lock);
        t->file_size = fsize;
        /* 记录 HEAD 探测实际协商到的协议版本（HTTP/2 仅 HTTPS 出现） */
        if (probe.http_version[0]) {
            strncpy(t->http_version, probe.http_version, sizeof(t->http_version) - 1);
            t->http_version[sizeof(t->http_version) - 1] = '\0';
        }

        /* 若用户未显式指定文件名，采用服务器建议的文件名（Content-Disposition）。
         * 这能修正大量动态下载链接 / API 下载 / 无扩展名 URL 的文件名错误问题。 */
        if (!t->filename_from_user && probe.suggested_filename[0]) {
            /* 服务器给的名字同样不可信（Content-Disposition 里带 : * ? 的都有） */
            sanitize_filename_utf8(probe.suggested_filename, t->filename,
                                   sizeof(t->filename));
            snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
                     t->save_dir, t->filename);
        }

        /* range_downgrade：已经实测过这台服务器不吃 Range（回 200+整份文件），
         * HEAD 的 Accept-Ranges 不可信，直接按单连接处理，不再分片。 */
        if (supports && fsize >= MIN_CHUNK_SIZE && !t->range_downgrade)
            calc_chunks(t);
        else {
            t->chunk_count = 1;
            t->chunks[0].chunk_id   = 0;
            t->chunks[0].start      = 0;
            /* end = -1 → 连 Range 头都不发（下载范围未知 = 取整个响应体），
             * 就当一个普通单连接下载，不会再被服务器回一次 200。 */
            t->chunks[0].end        = t->range_downgrade ? -1
                                                         : (fsize > 0 ? fsize - 1 : -1);
            t->chunks[0].downloaded = 0;
            t->chunks[0].done       = 0;
            t->chunks[0].retry_count = 0;
        }
        TRACE("chunks: 本任务 → %d 片（size=%lld range=%d threads=%d）",
              t->chunk_count, (long long)fsize, supports, t->thread_count);
        LeaveCriticalSection(&g_lock);
    }

    /* 预分配临时文件大小 */
    FILE *fp = utf8_fopen(t->tmp_path, "r+b");
    if (!fp) fp = utf8_fopen(t->tmp_path, "w+b");
    if (fp) {
        /* ⚠️ 必须用 _chsize_s（64 位）：Windows 上 long 是 32 位，`_chsize(fd, (long)file_size)`
         * 对 >2GB 的文件会把长度截断甚至变成负数，预分配失败后各分片按错误的文件长度写入，
         * 最终产出大小不对、内容错位的文件，而状态却是「已完成」。 */
        if (t->file_size > 0)
            if (_chsize_s(_fileno(fp), (__int64)t->file_size) != 0)
                /* 预分配只是优化（无 sparse 支持的文件系统上会真占盘）。失败不在这里
                 * 判死 —— 但必须留痕：磁盘满时后面的分片写入必然失败，靠这条 trace 才能
                 * 把「写入磁盘失败」和「预分配就失败了」对上。 */
                TRACE("预分配 %lld 字节失败（errno=%d），继续尝试写入",
                      (long long)t->file_size, errno);
        fclose(fp);
    }

    EnterCriticalSection(&g_lock);
    t->dispatch_idx = 0;   /* 重置动态调度计数器 */
    t->start_time = dl_time_ms();
    /* 状态与 error_msg 已在本函数开头置好（必须先于探测，否则探测期间界面还说谎）。
     * 这里**不能**再写一次 TASK_RUNNING：探测期间用户可能已经按了暂停，
     * 再写就把 PAUSED 覆盖回 RUNNING，等于把「暂停」悄悄吃回去。 */
    LeaveCriticalSection(&g_lock);

    /* 动态线程池：固定数量工作线程从 dispatch_idx 取片，先做完的线程继续领后续分片
     * （work-stealing），负载更均衡。
     * ⚠️ 并发数必须取本任务已定好的 thread_count —— 它在 dlmgr_add / 状态恢复时由
     *    effective_threads() 算出，等于 min(全局线程池大小, 每服务器连接数上限)。
     *    这里**不能**直接用 g_cfg.thread_pool_size：那样「每服务器连接数」就只影响
     *    分片数、完全约束不住真实并发（自测实测：封顶设 2 时并发峰值照样是 7），
     *    设置页那一项等于没生效——正是本项目反复排查的「死设置」。 */
    int pool = t->thread_count;
    if (pool <= 0 || pool > g_cfg.thread_pool_size)
        pool = g_cfg.thread_pool_size > 0 ? g_cfg.thread_pool_size : 1;
    if (pool > t->chunk_count) pool = t->chunk_count;
    if (pool <= 0) pool = 1;
    HANDLE handles[MAX_CHUNKS_PER_TASK];
    int launched = 0;

#ifdef _WIN32
    for (int i = 0; i < pool; i++) {
        HANDLE h = CreateThread(NULL, 0, pool_worker, t, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            handles[launched++] = h;
    }

    if (launched > 0) {
        WaitForMultipleObjects((DWORD)launched, handles, TRUE, INFINITE);
        for (int i = 0; i < launched; i++) CloseHandle(handles[i]);
    }
#endif

    /* 检查是否全部完成 */
    EnterCriticalSection(&g_lock);
    int all_done = 1;
    for (int i = 0; i < t->chunk_count; i++) {
        if (!t->chunks[i].done) { all_done = 0; break; }
    }
    if (all_done && t->status == TASK_RUNNING) {
        /* 兜底闸：一个字节都没收到、服务器也没声明大小 —— 绝不能算成功。
         * 否则会把「0 字节的空文件」重命名成正式文件、在界面上显示「已完成」，
         * 用户打开才发现是空的（比直接报错更难排查）。 */
        if (t->downloaded <= 0 && t->file_size <= 0) {
            t->status = TASK_FAILED;
            snprintf(t->error_msg, sizeof(t->error_msg),
                     "服务器未返回任何内容（可能被拒绝、需登录或链接无效）");
        } else {
            /* 同名策略：默认自动重命名（`名字 (1).ext`）——**绝不静默删掉用户已有的
             * 同名文件**。旧实现是无条件 utf8_delete(final_path) 再 move，重新下载一个
             * 同名文件就会把用户原来的那份直接抹掉，且界面上看不出任何提示。
             * 只有用户在设置里显式勾了「覆盖同名文件」才走删除分支。
             * 重命名后会同步 t->filename，所以列表里显示的就是真实落盘名。 */
            char final_path[1024] = {0};
            if (g_cfg.overwrite_existing) {
                snprintf(final_path, sizeof(final_path), "%s\\%s",
                         t->save_dir, t->filename);
                utf8_delete(final_path);
            } else {
                pick_free_filename(t->filename, sizeof(t->filename),
                                   t->save_dir, final_path, sizeof(final_path));
                if (!final_path[0])   /* 兜底，理由同 pick_free_filename 内的注释 */
                    snprintf(final_path, sizeof(final_path), "%s\\%s",
                             t->save_dir, t->filename);
            }
            if (utf8_move(t->tmp_path, final_path)) {
                t->status     = TASK_COMPLETED;
                t->downloaded = t->file_size > 0 ? t->file_size : t->downloaded;
            } else {
                t->status = TASK_FAILED;
                char why[320];
                describe_file_error("保存文件（重命名）", final_path, why, sizeof(why));
                snprintf(t->error_msg, sizeof(t->error_msg), "%s", why);
            }
        }
    }
    int st  = t->status;
    int tid = t->task_id;
    int relaunch_single = 0;
    int success = (st == TASK_COMPLETED);
    void (*cb)(int,int,void*) = g_shutdown ? NULL : t->complete_cb;
    void *ud = g_shutdown ? NULL : t->complete_ud;

    /* 被取消/被删除的任务：到这里所有工作线程都已退出、文件句柄已关闭，
     * 此时删 .idmtmp 才真的删得掉（dlmgr_cancel 里删会因文件被占用而失败，
     * 于是「取消」之后残留一个 .idmtmp 到下次启动）。 */
    if (st == TASK_CANCELLED)
        utf8_delete(t->tmp_path);

    t->worker_alive--;
    int gone = t->removed;
    int relaunch = (!gone && t->restart_pending);
    if (relaunch)
        t->restart_pending = 0;
    /* 不支持 Range 的降级重下：同样等线程清空后再动分片表 */
    if (!gone && !relaunch && t->range_downgrade && st == TASK_CANCELLED)
        relaunch_single = 1;
    /* 「暂停 → 继续」的记账，配合 dlmgr_start 的 worker_alive 守卫看：
     * 本代线程收工时若分片还没做完、而用户此刻要的仍是「继续」（状态还是 RUNNING，
     * 说明没被再按一次暂停），就必须补起新一代 —— 否则任务会永远停在「下载中」
     * 却一个线程都没有（幽灵任务）。分片已做完就不用补，交给上面的完成分支改名。 */
    int resume_later = 0;
    if (!gone && !relaunch && !relaunch_single && t->resume_pending) {
        t->resume_pending = 0;
        if (!all_done && st == TASK_RUNNING) resume_later = 1;
    }
    if (gone) {
        /* 槽位现在可以安全挪动了。摘除后 t 已失效，不得再解引用。 */
        detach_slot(tid);
        cb = NULL;   /* 已被删除的任务不再回调：GUI 侧早已把它移出列表 */
    } else if (relaunch || relaunch_single) {
        /* 马上会自动重来，别先报一次「失败」——那会让任务行闪一下红色、
         * 还会弹一条完成通知，用户以为下载挂了。 */
        cb = NULL;
    } else if (resume_later) {
        /* 同上：马上补起新一代，别闪红。状态要退回 PENDING，
         * 否则 dlmgr_start 一看「RUNNING」就直接返回，什么都不会发生。 */
        t->status = TASK_PENDING;
        cb = NULL;
    }
    LeaveCriticalSection(&g_lock);

    if (cb) cb(tid, success, ud);

    /* 三种「线程清空后重来」：下载中收到的重新开始、不支持 Range 的降级重下、
     * 以及等待期间收到却被本代线程没接住的「继续」。
     * 前两者都走 dlmgr_restart 的「无活动线程」分支（删临时文件 + 重置 + 重新探测启动），
     * 第三者只走 dlmgr_start（保留分片与断点，不删临时文件）。
     * 必须放在锁外调用——这两个 API 自己会拿锁，并且会再创建线程。 */
    if (relaunch || relaunch_single)
        dlmgr_restart(tid);
    else if (resume_later)
        dlmgr_start(tid);
    return 0;
}

/* ─────────────────────────────────────
 * 公开 API
 * ───────────────────────────────────── */

int dlmgr_init(const DownloadConfig *cfg) {
    if (g_initialized) return 0;
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = 0;
    g_shutdown = 0;   /* 允许 init → destroy → init 循环复用 */
    if (cfg) g_cfg = *cfg;
    else {
        g_cfg.thread_pool_size   = THREAD_POOL_SIZE;
        g_cfg.max_concurrent     = 3;
        g_cfg.retry_max          = 3;
        g_cfg.speed_limit_global = 0;
        /* 0 = 不额外封顶（GUI 启动后会经 applyToEngine 下发设置页的真实值） */
        g_cfg.max_conn_per_server = 0;
        g_cfg.overwrite_existing  = 0;   /* 默认自动重命名，不删同名文件 */
        g_cfg.site_login_count    = 0;
        strncpy(g_cfg.default_save_dir, "C:\\Users\\Public\\Downloads",
                sizeof(g_cfg.default_save_dir) - 1);
    }
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_throttle_lock);
    /* 初始化全局请求选项（默认不含代理，沿用系统代理） */
    g_net_opt = network_default_options();
    g_net_opt.proxy_addr = g_proxy_addr_buf;
    network_init();
    g_initialized = 1;
    return 0;
}

void dlmgr_destroy(void) {
    if (!g_initialized) return;

    /* ① 先摘掉所有回调并请求取消。
     * 目的是让「引擎已开始拆、worker 还在跑」这段时间里，不再有任何一次
     * 跨线程回调被投递给正在析构的 GUI 对象（跳板是 static_cast<DownloadManager*>(ud)
     * 后直接解引用，指针非空但可能已悬空，`if (self)` 挡不住）。
     * 回调是成对读出的（函数指针 + ud），所以置空时也必须成对置空。 */
    EnterCriticalSection(&g_lock);
    g_shutdown = 1;
    for (int i = 0; i < g_task_count; i++) {
        g_tasks[i].progress_cb = NULL;
        g_tasks[i].progress_ud = NULL;
        g_tasks[i].complete_cb = NULL;
        g_tasks[i].complete_ud = NULL;
        if (g_tasks[i].status == TASK_RUNNING)
            g_tasks[i].status = TASK_CANCELLED;
    }
    LeaveCriticalSection(&g_lock);

    /* ② 等 worker 真的退出。
     * 取消检查发生在每个 1 MiB 小块之间，所以线程会很快在下一个块边界退出；
     * 之前是「置位后立刻 network_cleanup + DeleteCriticalSection」，等于在活线程
     * 脚下抽掉锁 —— 线程随后 EnterCriticalSection 就是 UB（实测表现为偶发退出崩溃）。 */
    int alive = 0;
    for (int waited = 0; waited < 3000; waited += 10) {
        alive = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_task_count; i++)
            alive += g_tasks[i].worker_alive;
        LeaveCriticalSection(&g_lock);
        if (alive == 0) break;
        Sleep(10);
    }

    /* 如果网络卡住导致 3 秒内没退干净：宁可不删临界区（进程马上退出，泄漏无意义），
     * 也不要在活线程还在取锁的时候把它删掉。 */
    const int clean = (alive == 0);
    if (!clean)
        TRACE("destroy: 仍有 %d 个 worker 未退出，保留临界区不删（避免删锁后线程再取锁）",
              alive);
    else
        TRACE("destroy: 所有 worker 已退出");

    network_cleanup();
    if (clean) {
        DeleteCriticalSection(&g_lock);
        DeleteCriticalSection(&g_throttle_lock);
    }
    g_initialized = 0;
}

int dlmgr_add(const char *url,
               const char *save_dir,
               const char *filename,
               int thread_count,
               void (*progress_cb)(int,int64_t,int64_t,int,void*),
               void *progress_ud,
               void (*complete_cb)(int,int,void*),
               void *complete_ud) {
    if (!g_initialized || !url || *url == '\00') return -1;

    EnterCriticalSection(&g_lock);
    if (g_task_count >= MAX_TASKS) {
        LeaveCriticalSection(&g_lock);
        return -1;
    }

    DownloadTask *t = &g_tasks[g_task_count];
    memset(t, 0, sizeof(DownloadTask));
    t->task_id = g_next_id++;

    strncpy(t->url, url, sizeof(t->url) - 1);

    const char *dir = (save_dir && save_dir[0])
                     ? save_dir : g_cfg.default_save_dir;
    strncpy(t->save_dir, dir, sizeof(t->save_dir) - 1);

    if (filename && filename[0]) {
        /* 用户手填的名字也要过清理：冒号/星号之类会让 CreateFile 直接失败 */
        sanitize_filename_utf8(filename, t->filename, sizeof(t->filename));
        t->filename_from_user = 1;
    } else {
        dl_infer_filename(url, t->filename, sizeof(t->filename));
        t->filename_from_user = 0;
    }

    snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
             t->save_dir, t->filename);

    t->status       = TASK_PENDING;
    /* 并发数受「每服务器连接数」上限约束，设置页该项才真正生效 */
    t->thread_count = effective_threads(thread_count > 0 ? thread_count
                                                         : g_cfg.thread_pool_size);
    /* 建立本任务的网络选项（含按 URL 命中「站点登录」的 HTTP 认证） */
    apply_site_auth(t);
    t->speed_ema    = 0;
    t->ema_bytes    = 0;
    t->ema_window_ms = 0;
    t->ema_last_ms  = 0;
    t->last_progress_cb_ms = 0;
    t->progress_cb  = progress_cb;
    t->progress_ud  = progress_ud;
    t->complete_cb  = complete_cb;
    t->complete_ud  = complete_ud;

    int id = t->task_id;
    g_task_count++;
    LeaveCriticalSection(&g_lock);
    return id;
}

int dlmgr_start(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (!t) { LeaveCriticalSection(&g_lock); return -1; }
    if (t->status == TASK_RUNNING) { LeaveCriticalSection(&g_lock); return 0; }

    /* ⚠️ 上一代线程还没退干净时，绝不再起第二组监督线程。
     * 最常见的触发路径就是「暂停 → 继续」：暂停只在下一个小块边界才生效，而下载线程
     * 可能正卡在重试 Sleep（429 + Retry-After，最长可到 1 小时）或一个大请求里。
     * 这时状态是 PAUSED，上面那句 RUNNING 判断挡不住，于是同一个任务出现两代线程同时
     * 盯同一份 chunks[] 和同一个 .idmtmp；而改名要求文件句柄已经关闭，实测结果是
     * 「保存文件（重命名）失败：文件被其他程序占用」—— 任务显示失败，字节却早就下完了
     * （回归用例见 tools/engine_selftest.c 的 [25]）。
     * 这里只做一件事：把状态从「暂停」改回来，旧线程在下一个块边界自己接着下；
     * 万一它们已经因为上次暂停提前收工，resume_pending 会让旧组的监督线程退出时
     * 补起新一代（见 start_task_threads 尾部的 resume_later）。 */
    if (t->worker_alive > 0) {
        t->status            = TASK_RUNNING;
        t->resume_pending    = 1;
        t->throttle_until_ms = 0;    /* 已经继续了，不该还挂着上一轮「N 秒后重试」 */
        t->throttle_http     = 0;
        apply_site_auth(t);          /* 与老路径一致：改过的代理/站点登录对「继续」也要生效 */
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    /* 探测（HEAD 取大小/Range/建议文件名）与站点凭据下发都挪到监督线程里做了，
     * 见 start_task_threads 的 need_probe 段。原因很直接：dlmgr_start 是 GUI 线程调的
     * （DownloadManager::startTask 直调、「全部开始」经 dlmgr_start_all 逐个串着调），
     * 而一次探测最坏会挂满 connect_timeout —— 实测对慢服务器本函数要 3000ms 才返回，
     * 用户点完「开始」就是整整三秒不响应。
     * 这里只留下必须同步决定的事：能不能起（RUNNING）、要不要补起（worker_alive 守卫）。 */
    TRACE("start: id=%d threads=%d（上限 %d）url=%s",
          task_id, t->thread_count, g_cfg.max_conn_per_server, t->url);
    LeaveCriticalSection(&g_lock);

#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, start_task_threads,
                            find_task(task_id), 0, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    else return -1;
#endif
    return 0;
}

int dlmgr_pause(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        /* 排队中（PENDING）也要真的置为 PAUSED。
         * 原来的实现只在 RUNNING 时改状态、对 PENDING 静默返回 0，于是 GUI 把行标成
         * 「已暂停」、引擎里却还是 PENDING：一旦退出就会以 PENDING 落盘，下次启动
         * 队列调度器照样把它拉起来下载 —— 用户的「暂停」等于没按。
         * 终态（已完成/已失败/已取消）不动，避免把结束的任务改回可续状态。 */
        if (t->status == TASK_PENDING || t->status == TASK_RUNNING)
            t->status = TASK_PAUSED;
        /* 用户按了暂停，就不该再显示「N 秒后重试」——那是给正在等的下载用的。
         * 底层那一觉可能还在睡，但界面上说的必须是当前真话。 */
        t->throttle_until_ms = 0;
        t->throttle_http     = 0;
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_cancel(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        t->status = TASK_CANCELLED;
        /* 临时文件只能在没有活动线程时删：有线程还开着这个文件时 DeleteFileW 会因
         * 占用而失败（fopen 不共享删除权限），于是「取消」看着成功、.idmtmp 却留着。
         * 有线程的情况交给 start_task_threads 收尾时删。 */
        if (t->worker_alive == 0)
            utf8_delete(t->tmp_path);
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_restart(int task_id) {
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (!t) { LeaveCriticalSection(&g_lock); return -1; }

    /* 还在下载中就直接重置分片、并让 dlmgr_start 再起一组线程，
     * 会变成两组线程同时写同一个 .idmtmp（旧线程的指针还盯着 chunks[]，
     * 而 chunks 已经被 memset 了）——产出文件必然损坏。
     * 这里只置位并让它停下，等最后一个线程退出时由监督线程重新发起。 */
    if (t->worker_alive > 0) {
        t->restart_pending = 1;
        t->status = TASK_CANCELLED;
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    /* 删除旧临时文件 */
    utf8_delete(t->tmp_path);

    /* 重置任务为初始状态，保留 URL / 路径 / 文件名 / 回调 */
    t->status     = TASK_PENDING;
    t->downloaded = 0;
    t->file_size  = -1;      /* 强制重新探测文件大小 */
    t->speed_bps  = 0;
    t->speed_ema  = 0;
    t->ema_bytes     = 0;
    t->ema_window_ms = 0;
    t->ema_last_ms   = 0;
    t->eta_sec    = 0;
    t->start_time = 0;
    t->throttle_until_ms = 0;   /* 重新开始：上一轮的限流等待作废，别挂着假倒计时 */
    t->throttle_http     = 0;
    memset(t->chunks, 0, sizeof(t->chunks));
    /* chunk_count 必须跟着清零：dlmgr_start 现在用「chunk_count == 0」判断是否需要
     * 重新探测分段（不再看状态）。只清数组不清计数，等于对外宣称"已经分好段了"，
     * 而 chunks[] 全是 0 值 —— 会静默产生一个从头覆盖的坏文件。 */
    t->chunk_count = 0;
    LeaveCriticalSection(&g_lock);

    /* dlmgr_start 检测到「尚未分段」会重新获取文件大小并计算分片 */
    return dlmgr_start(task_id);
}

int dlmgr_remove(int task_id) {
    int found = 0;
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        found = 1;
        if (t->worker_alive > 0) {
            /* 还有线程在跑（删除一个正在下载的任务就会走到这里）。
             * 此刻 memmove 把整个结构体挪一格，线程手里的指针就会指向**另一个任务**：
             * 它会把自己的进度、分片数据、乃至「下载完成」状态写到那个无辜任务身上，
             * 而那个任务的文件会被重命名成当前这个任务的名字。跨任务污染，且无法事后察觉。
             * 正确做法：只登记删除意图并置为「已取消」（线程据此优雅退出），
             * 等最后一个线程退出时由它就地摘除槽位。*/
            t->removed = 1;
            t->status  = TASK_CANCELLED;
        } else {
            detach_slot(task_id);
        }
    }
    LeaveCriticalSection(&g_lock);
    return found ? 0 : -1;
}

/* 重新绑定任务的进度/完成回调（dlmgr_load_state 恢复的任务用） */
int dlmgr_set_callbacks(int task_id,
                        void (*progress_cb)(int,int64_t,int64_t,int,void*), void* progress_ud,
                        void (*complete_cb)(int,int,void*), void* complete_ud)
{
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        t->progress_cb = progress_cb;
        t->progress_ud = progress_ud;
        t->complete_cb = complete_cb;
        t->complete_ud = complete_ud;
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

int dlmgr_get_task_info(int task_id, TaskInfo *info) {
    if (!info) return -1;
    EnterCriticalSection(&g_lock);
    DownloadTask *t = find_task(task_id);
    if (t) {
        info->task_id    = t->task_id;
        info->downloaded = t->downloaded;
        info->file_size  = t->file_size;
        info->speed_bps  = t->speed_bps;
        info->eta_sec    = t->eta_sec;
        info->status     = (int)t->status;
        strncpy(info->filename, t->filename, sizeof(info->filename) - 1);
        info->filename[sizeof(info->filename) - 1] = '\0';
        strncpy(info->http_version, t->http_version, sizeof(info->http_version) - 1);
        info->http_version[sizeof(info->http_version) - 1] = '\0';
        /* 分片概况：clamp 到数组上限，避免越界读 */
        int cc = t->chunk_count;
        if (cc < 0) cc = 0;
        if (cc > MAX_CHUNKS_PER_TASK) cc = MAX_CHUNKS_PER_TASK;
        info->chunk_count = cc;
        int done = 0;
        for (int i = 0; i < cc; i++) {
            if (t->chunks[i].done) done++;
        }
        info->chunks_done = done;
        strncpy(info->error_msg, t->error_msg, sizeof(info->error_msg) - 1);
        info->error_msg[sizeof(info->error_msg) - 1] = '\0';
        /* 限流倒计时在这里读侧换算（写侧只记一次截止时刻）；向上取整，
         * 这样 Retry-After: 3 的第一格显示 3 而不是一上来就 2。 */
        info->throttle_http = t->throttle_http;
        info->throttle_sec  = 0;
        if (t->throttle_until_ms > 0) {
            int64_t left = t->throttle_until_ms - dl_time_ms();
            if (left > 0) info->throttle_sec = (int)((left + 999) / 1000);
            else { t->throttle_until_ms = 0; t->throttle_http = 0; }   /* 到点自清 */
        }
    }
    LeaveCriticalSection(&g_lock);
    return t ? 0 : -1;
}

/* 批量获取所有任务详情：单次持锁遍历 g_tasks[]，填充顺序与 dlmgr_list 返回的 ids 完全一致，
 * 调用方可按索引对齐直接使用，省去每任务一次 EnterCriticalSection + find_task 二分查找。
 * 这是 GUI 4Hz 刷新热路径（refreshFromEngine）的针对性优化——原来每 tick 为每个任务单独
 * 加锁并做二分查找，n 个任务即 n 次锁竞争，会与下载线程的进度回调抢锁。 */
int dlmgr_get_task_infos(TaskInfo *out, int max_count) {
    if (!out || !g_initialized) return 0;
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    /* 时钟只读一次、全批共用：限流倒计时的粒度是秒，犯不着每任务再取一次 */
    const int64_t now_ms = dl_time_ms();
    for (int i = 0; i < cnt; i++) {
        DownloadTask *t = &g_tasks[i];
        out[i].task_id    = t->task_id;
        out[i].downloaded = t->downloaded;
        out[i].file_size  = t->file_size;
        out[i].speed_bps  = t->speed_bps;
        out[i].eta_sec    = t->eta_sec;
        out[i].status     = (int)t->status;
        strncpy(out[i].filename, t->filename, sizeof(out[i].filename) - 1);
        out[i].filename[sizeof(out[i].filename) - 1] = '\0';
        strncpy(out[i].http_version, t->http_version, sizeof(out[i].http_version) - 1);
        out[i].http_version[sizeof(out[i].http_version) - 1] = '\0';
        /* 与 dlmgr_get_task_info 保持一致：这两个字段必须一并填充，
         * 否则调用方读到的是未初始化内存（TaskInfo 是栈上分配的） */
        int cc = t->chunk_count;
        if (cc < 0) cc = 0;
        if (cc > MAX_CHUNKS_PER_TASK) cc = MAX_CHUNKS_PER_TASK;
        out[i].chunk_count = cc;
        int done = 0;
        for (int c = 0; c < cc; c++) {
            if (t->chunks[c].done) done++;
        }
        out[i].chunks_done = done;
        strncpy(out[i].error_msg, t->error_msg, sizeof(out[i].error_msg) - 1);
        out[i].error_msg[sizeof(out[i].error_msg) - 1] = '\0';
        /* 必须与 dlmgr_get_task_info 同步填充：GUI 的 TaskInfo infos[128] 是栈数组，
         * 漏一个字段就是读未初始化内存。 */
        out[i].throttle_http = t->throttle_http;
        out[i].throttle_sec  = 0;
        if (t->throttle_until_ms > 0) {
            int64_t left = t->throttle_until_ms - now_ms;
            if (left > 0) out[i].throttle_sec = (int)((left + 999) / 1000);
            else { t->throttle_until_ms = 0; t->throttle_http = 0; }
        }
    }
    LeaveCriticalSection(&g_lock);
    return cnt;
}

int dlmgr_list(int *ids, int max_count) {
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    for (int i = 0; i < cnt; i++) ids[i] = g_tasks[i].task_id;
    LeaveCriticalSection(&g_lock);
    return cnt;
}

void dlmgr_set_speed_limit(int bytes_per_sec) {
    EnterCriticalSection(&g_lock);
    g_cfg.speed_limit_global = bytes_per_sec;
    LeaveCriticalSection(&g_lock);
    throttle_reset();   /* 速率变了就重置虚拟时钟，避免拿旧累计字节按新速率误判 */
}

void dlmgr_set_proxy(int type, const char *host, int port,
                     const char *user, const char *pass) {
    /* 关闭代理：恢复默认选项，沿用系统代理 */
    if (type == PROXY_NONE || !host || !host[0] || port <= 0) {
        g_net_opt = network_default_options();
        g_net_opt.proxy_addr = g_proxy_addr_buf;
        g_net_opt.proxy_user = NULL;
        g_net_opt.proxy_pass = NULL;
        g_net_opt.proxy_type = PROXY_NONE;
        g_proxy_addr_buf[0] = '\0';
        g_proxy_user_buf[0] = '\0';
        g_proxy_pass_buf[0] = '\0';
        return;
    }
    g_net_opt.proxy_type = (ProxyType)type;
    /* libcurl 语法：地址统一为 "host:port"，由 PROXYTYPE 区分 HTTP/SOCKS。
     * （旧版 WinHTTP 的 "socks=host:port" 前缀在 libcurl 下无法解析，会导致 SOCKS 代理失效） */
    snprintf(g_proxy_addr_buf, sizeof(g_proxy_addr_buf),
             "%s:%d", host, port);
    g_net_opt.proxy_addr = g_proxy_addr_buf;
    g_net_opt.proxy_user = NULL;
    g_net_opt.proxy_pass = NULL;
    if (user && user[0]) {
        strncpy(g_proxy_user_buf, user, sizeof(g_proxy_user_buf) - 1);
        g_proxy_user_buf[sizeof(g_proxy_user_buf) - 1] = '\0';
        g_net_opt.proxy_user = g_proxy_user_buf;
    }
    if (pass && pass[0]) {
        strncpy(g_proxy_pass_buf, pass, sizeof(g_proxy_pass_buf) - 1);
        g_proxy_pass_buf[sizeof(g_proxy_pass_buf) - 1] = '\0';
        g_net_opt.proxy_pass = g_proxy_pass_buf;
    }
}

/* ─────────────────────────────────────
 * 持久化（JSON 格式，cJSON 就绪前用简化格式）
 * ───────────────────────────────────── */
/* ─────────────────────────────────────
 * 状态文件（tasks.json）的字符串读写
 *
 * 为什么要成对处理：写的时候必须转义，读的时候必须还原，否则往返不闭环。
 * 而「读」还要兼容**旧版写出的未转义文件**（旧版直接把 Windows 路径原样写出，
 * 形如 "dir":"C:\Users\me"），所以用文件头的 "idmFormat":2 做版本判别：
 * 有标记才按 JSON 转义语义还原，没有就沿用原来的逐字符扫描。
 * 不做这个区分的话，"C:\temp" 里的 \t 会被还原成制表符，路径就悄悄坏了。
 * ───────────────────────────────────── */

/* 按 JSON 规则转义写入。必须处理 '\\' 和 '"'：
 * Windows 目录里全是反斜杠，原样写出去会得到 "\Users" 这种非法转义序列，
 * 严格解析器（QJsonDocument）会因一个字符读不出整份文件 —— 所有任务一起丢。
 * 控制字符转成 \u00XX，保证输出永远是合法 JSON。 */
static void json_write_escaped(FILE *fp, const char *s) {
    if (!s) return;
    for (const unsigned char *p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            /* UTF-8 多字节原样透传（0x80+），本身就是合法 JSON 字符 */
            default:
                if (*p < 0x20) fprintf(fp, "\\u%04x", (unsigned)*p);
                else           fputc((int)*p, fp);
        }
    }
}

/* 读一个 JSON 字符串字面量：p 指向开头的引号之后，结果写进 out。
 * 返回下一个待处理位置（越过收尾引号）。
 * 未知转义序列按「原样保留反斜杠」处理——宽松是关键，宁可少还原也不能吃掉字符。 */
static const char *json_read_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            char c = p[1];
            char dec = 0;
            switch (c) {
                case '"':  dec = '"';  break;
                case '\\': dec = '\\'; break;
                case '/':  dec = '/';  break;
                case 'n':  dec = '\n'; break;
                case 'r':  dec = '\r'; break;
                case 't':  dec = '\t'; break;
                case 'b':  dec = '\b'; break;
                case 'f':  dec = '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (sscanf(p + 2, "%4x", &cp) == 1 && cp > 0 && cp < 0x80) {
                        if (n + 1 < cap) out[n++] = (char)cp;
                        p += 6;
                        continue;
                    }
                    break;   /* \uXXXX 超出 ASCII：走下面的按未知转义保留 */
                }
                default: break;
            }
            if (dec) {
                if (n + 1 < cap) out[n++] = dec;
                p += 2;
                continue;
            }
            if (n + 2 < cap) { out[n++] = '\\'; out[n++] = c; }
            p += 2;
            continue;
        }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[(n < cap) ? n : (cap ? cap - 1 : 0)] = '\0';
    return (*p == '"') ? p + 1 : p;
}

/* 读出 "key":"值为字符串" 到 out；找不到返回 0。
 * 调用方需保证 p 落在本任务对象的范围内（沿用原来的 strstr 定位习惯）。 */
static int json_read_field(const char *p, const char *key, int escaped,
                           char *out, size_t cap) {
    const char *fld = strstr(p, key);
    if (!fld) return 0;
    fld += strlen(key);
    if (*fld != '"') return 0;
    fld++;
    if (escaped) {
        json_read_string(fld, out, cap);
    } else {
        const char *end = strchr(fld, '"');
        size_t len = end ? (size_t)(end - fld) : strlen(fld);
        if (len >= cap) len = cap - 1;
        memcpy(out, fld, len);
        out[len] = '\0';
    }
    return 1;
}

int dlmgr_save_state(const char *path) {
    if (!path) return -1;
    FILE *fp = utf8_fopen(path, "w");
    if (!fp) return -1;

    /* idmFormat 标记：读的时候据此判断该不该按 JSON 转义语义还原。
     * 老版本文件没有这一行，仍然走宽松扫描，不受影响。 */
    fprintf(fp, "{\n  \"idmFormat\": 2,\n  \"tasks\": [\n");
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_task_count; i++) {
        DownloadTask *t = &g_tasks[i];
        fprintf(fp, "    {\"id\":%d, \"url\":\"", t->task_id);
        json_write_escaped(fp, t->url);
        fprintf(fp, "\", \"file\":\"");
        json_write_escaped(fp, t->filename);
        fprintf(fp, "\", \"dir\":\"");
        json_write_escaped(fp, t->save_dir);
        fprintf(fp, "\", ");
        fprintf(fp,
            "\"size\":%lld, \"downloaded\":%lld, \"status\":%d,\n",
            (long long)t->file_size,
            (long long)t->downloaded,
            (int)t->status);

        fprintf(fp, "     \"chunks\":[");
        for (int j = 0; j < t->chunk_count; j++) {
            if (j > 0) fprintf(fp, ",");
            fprintf(fp,
                "{\"start\":%lld, \"end\":%lld, \"downloaded\":%lld, \"done\":%d}",
                (long long)t->chunks[j].start,
                (long long)t->chunks[j].end,
                (long long)t->chunks[j].downloaded,
                t->chunks[j].done);
        }
        fprintf(fp, "] }%s\n",
                (i < g_task_count - 1) ? "," : "");
    }
    LeaveCriticalSection(&g_lock);
    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    return 0;
}

/* 清空 g_tasks[]，供 dlmgr_load_state() 在载入前调用。
 *
 * 存在的理由：load 的语义是「用文件内容重建队列」，不是「往现有队列尾部追加」。
 * 旧实现直接往下追加，于是同一进程内 load 两次就会把任务翻倍。这不是理论问题：
 * 走查工具里连开两个 MainWindow（各自构造一次），真实用户目录里的 tasks.json
 * 就从 56 条变成 112 条，并在退出时被写回磁盘 —— 任务列表凭空空了一半的「重复项」，
 * 而且重复的 task_id 会让 find_task() 的二分查找在同一 id 上反复命中同一条。
 * 与其要求调用方「一辈子只 load 一次」，不如让 load 自己保证幂等。
 *
 * 清空的顺序有讲究：先把 RUNNING 标成 CANCELLED 让 worker 在下一个块边界退出，
 * 等它们真的退场后再 memset —— 否则线程还在往 g_tasks[i] 里写，抹完它继续写，
 * 下一个 dlmgr_add() 复用同一槽位就成了两个写入者。 */
static void reset_all_tasks(void) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_RUNNING)
            g_tasks[i].status = TASK_CANCELLED;
    }
    LeaveCriticalSection(&g_lock);

    /* 与 dlmgr_destroy() 同样的等待窗口：取消失效（网络卡住）时不无限等，
     * 3 秒后照常清空 —— 此时那些任务已被标记取消，不会再被当成有效任务。 */
    for (int waited = 0; waited < 3000; waited += 10) {
        int alive = 0;
        EnterCriticalSection(&g_lock);
        for (int i = 0; i < g_task_count; i++)
            alive += g_tasks[i].worker_alive;
        LeaveCriticalSection(&g_lock);
        if (alive == 0) break;
        Sleep(10);
    }

    EnterCriticalSection(&g_lock);
    memset(g_tasks, 0, sizeof(g_tasks));
    g_task_count = 0;
    g_next_id = 1;      /* 新任务从 1 重新编号，与清空后的空队列保持一致 */
    LeaveCriticalSection(&g_lock);
}

int dlmgr_load_state(const char *json_path) {
    if (!json_path) return -1;

    /* 载入前先清空既有任务（见 reset_all_tasks 上的说明）。
     * 放在读文件成功之后再调用会更省事，但那样「文件打不开」就会留下半新半旧的
     * 队列；这里是启动路径，清空后失败也只是回到空列表，语义更好预期。 */
    reset_all_tasks();

    /* 读取整个 JSON 文件 */
    FILE *fp = utf8_fopen(json_path, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0) { fclose(fp); return -1; }
    fseek(fp, 0, SEEK_SET);

    char *buf = (char*)malloc((size_t)fsize + 1);
    if (!buf) { fclose(fp); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, fp);
    if (nread > (size_t)fsize) nread = (size_t)fsize;  /* 防止 tainted index 越界 */
    buf[nread] = '\0';
    fclose(fp);

    EnterCriticalSection(&g_lock);

    /* 本次文件是否由「转义版」写入器产出（见 json_write_escaped 上的说明）。
     * 旧文件没有这个标记 → 沿用原来的逐字符扫描，避免把 "C:\temp" 的 \t 还原成制表符。 */
    const int escaped = (strstr(buf, "\"idmFormat\"") != NULL);
    if (escaped) TRACE("load_state: 检测到转义版状态文件，按 JSON 转义语义还原");

    /* 定位到 "tasks" 数组 */
    char *p = strstr(buf, "\"tasks\"");
    if (!p) goto done;
    p = strchr(p, '[');
    if (!p) goto done;
    p++;  /* 跳过 '[' */

    /* 循环解析每个任务对象 */
    while (*p) {
        /* 跳过空白和逗号，找 '{' 或 ']' */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        if (g_task_count >= MAX_TASKS) break;
        DownloadTask *t = &g_tasks[g_task_count];
        memset(t, 0, sizeof(DownloadTask));

        /* ── 解析 "id":数字 ── */
        char *fld = strstr(p, "\"id\":");
        if (!fld) break;
        fld += 5; while (*fld == ' ' || *fld == '\t') fld++;
        t->task_id = atoi(fld);
        if (t->task_id >= g_next_id) g_next_id = t->task_id + 1;

        /* ── 解析 "url"/"file"/"dir" ──
         * key 只给到冒号（不含值那侧的引号）：json_read_field 内部会校验并跳过它。
         * 若把引号也写进 key，函数会越过它再去要求一个引号，结果三个字段全读空。 */
        json_read_field(p, "\"url\":",  escaped, t->url,      sizeof(t->url));
        json_read_field(p, "\"file\":", escaped, t->filename, sizeof(t->filename));
        json_read_field(p, "\"dir\":",  escaped, t->save_dir, sizeof(t->save_dir));

        /* ── 解析 "size":数字 ── */
        fld = strstr(p, "\"size\":");
        if (fld) { fld += 7; while (*fld == ' ' || *fld == '\t') fld++; t->file_size = (int64_t)atoll(fld); }

        /* ── 解析 "downloaded":数字 ── */
        fld = strstr(p, "\"downloaded\":");
        if (fld) { fld += 13; while (*fld == ' ' || *fld == '\t') fld++; t->downloaded = (int64_t)atoll(fld); }

        /* ── 解析 "status":数字 ── */
        fld = strstr(p, "\"status\":");
        if (fld) {
            fld += 9; while (*fld == ' ' || *fld == '\t') fld++;
            t->status = (TaskStatus)atoi(fld);
            /* 重启后，RUNNING 任务变为 PAUSED（需要手动恢复） */
            if (t->status == TASK_RUNNING) t->status = TASK_PAUSED;
        }

        /* ── 解析 "chunks":[...] ── */
        char *chunk_arr = strstr(p, "\"chunks\":[");
        if (chunk_arr) {
            char *cp = strchr(chunk_arr, '[');
            if (cp) {
                cp++;  /* 跳过 '[' */
                t->chunk_count = 0;
                while (*cp && t->chunk_count < MAX_CHUNKS_PER_TASK) {
                    while (*cp == ' ' || *cp == '\t' || *cp == '\n' || *cp == '\r' || *cp == ',') cp++;
                    if (*cp == ']') break;
                    if (*cp != '{') break;

                    DownloadChunk *c = &t->chunks[t->chunk_count];
                    char *cf;

                    cf = strstr(cp, "\"start\":");
                    if (cf) { cf += 8; while (*cf == ' ' || *cf == '\t') cf++; c->start = atoll(cf); }

                    cf = strstr(cp, "\"end\":");
                    if (cf) { cf += 6; while (*cf == ' ' || *cf == '\t') cf++; c->end = atoll(cf); }

                    cf = strstr(cp, "\"downloaded\":");
                    if (cf) { cf += 13; while (*cf == ' ' || *cf == '\t') cf++; c->downloaded = atoll(cf); }

                    cf = strstr(cp, "\"done\":");
                    if (cf) { cf += 7; while (*cf == ' ' || *cf == '\t') cf++; c->done = atoi(cf); }

                    c->chunk_id = t->chunk_count;
                    c->retry_count = 0;
                    t->chunk_count++;

                    /* 跳到下一个 chunk 或结束 */
                    cp = strchr(cp, '}');
                    if (cp) cp++;
                }
                /* chunks 数组解析完成，将 p 更新到 ']' 之后 */
                char *bracket_close = strchr(cp, ']');
                if (bracket_close) p = bracket_close + 1;
            }
        }

        /* 恢复其他字段 */
        snprintf(t->tmp_path, sizeof(t->tmp_path), "%s\\%s.idmtmp",
                 t->save_dir[0] ? t->save_dir : g_cfg.default_save_dir,
                 t->filename);
        t->progress_cb  = NULL;
        t->progress_ud  = NULL;
        t->complete_cb  = NULL;
        t->complete_ud  = NULL;
        t->speed_bps    = 0;
        t->eta_sec      = 0;
        t->start_time   = 0;
        t->last_progress_cb_ms = 0;
        /* 从状态文件恢复的任务也要过一遍并发上限（状态里存的是历史值） */
        t->thread_count = effective_threads(g_cfg.thread_pool_size);

        g_task_count++;
        p = strchr(p, '}');
        if (p) p++;
    }

done:
    /* 按 task_id 升序排序后再交出去（放在 done 之后：解析中途放弃也要建立不变量）。
     * find_task() 是二分查找，依赖「g_tasks[] 按 task_id 升序」这条隐式约定；
     * 新建/摘除路径都保序，但状态文件是外部可编辑的输入 —— 顺序一旦不是升序，
     * 二分就会「找不到明明存在的任务」，表现为任务凭空消失、暂停/删除全部失效。
     * 与其要求文件永远有序，不如在入口处把不变量建立起来。 */
    for (int i = 1; i < g_task_count; i++) {
        DownloadTask tmp = g_tasks[i];
        int j = i - 1;
        while (j >= 0 && g_tasks[j].task_id > tmp.task_id) {
            g_tasks[j + 1] = g_tasks[j];
            j--;
        }
        g_tasks[j + 1] = tmp;
    }
    LeaveCriticalSection(&g_lock);
    free(buf);
    return 0;
}

/* ─────────────────────────────────────
 * GUI 需要的额外 API（main_window / task_detail_dialog 调用）
 * ───────────────────────────────────── */

/* 获取任务摘要列表（用于刷新 ListView） */
int dlmgr_get_task_summary(TaskSummary *out, int max_count) {
    if (!out || !g_initialized) return 0;
    EnterCriticalSection(&g_lock);
    int cnt = g_task_count < max_count ? g_task_count : max_count;
    for (int i = 0; i < cnt; i++) {
        DownloadTask *t = &g_tasks[i];
        out[i].task_id     = t->task_id;
        strncpy(out[i].filename, t->filename, sizeof(out[i].filename)-1);
        strncpy(out[i].save_dir, t->save_dir, sizeof(out[i].save_dir)-1);
        strncpy(out[i].url, t->url, sizeof(out[i].url)-1);
        out[i].size       = t->file_size;
        out[i].status      = (int)t->status;
        out[i].speed       = t->speed_bps;
    }
    LeaveCriticalSection(&g_lock);
    return cnt;
}

/* 获取当前配置 */
DownloadConfig dlmgr_get_config(void) {
    return g_cfg;
}

/* 更新配置（运行时生效）*/
void dlmgr_set_config(const DownloadConfig *cfg) {
    if (!cfg || !g_initialized) return;
    EnterCriticalSection(&g_lock);
    g_cfg.thread_pool_size = cfg->thread_pool_size;
    g_cfg.max_concurrent  = cfg->max_concurrent;
    g_cfg.retry_max       = cfg->retry_max;
    g_cfg.speed_limit_global = cfg->speed_limit_global;
    g_cfg.proxy_type = cfg->proxy_type;
    strncpy(g_cfg.proxy_host, cfg->proxy_host, sizeof(g_cfg.proxy_host) - 1);
    g_cfg.proxy_port = cfg->proxy_port;
    strncpy(g_cfg.proxy_user, cfg->proxy_user, sizeof(g_cfg.proxy_user) - 1);
    strncpy(g_cfg.proxy_pass, cfg->proxy_pass, sizeof(g_cfg.proxy_pass) - 1);
    strncpy(g_cfg.user_agent, cfg->user_agent, sizeof(g_cfg.user_agent) - 1);
    g_cfg.user_agent[sizeof(g_cfg.user_agent) - 1] = '\0';
    g_cfg.connect_timeout_sec = cfg->connect_timeout_sec;
    /* 每服务器连接数上限：负数视为无效（当 0 处理 = 不封顶） */
    g_cfg.max_conn_per_server = cfg->max_conn_per_server > 0
                                    ? cfg->max_conn_per_server : 0;
    /* 同名文件：默认自动重命名；只有显式非 0 才覆盖删除 */
    g_cfg.overwrite_existing = cfg->overwrite_existing ? 1 : 0;
    /* 站点登录凭据：整表下发，超上限的条目丢弃 */
    int sc = cfg->site_login_count;
    if (sc < 0) sc = 0;
    if (sc > MAX_SITE_LOGINS) sc = MAX_SITE_LOGINS;
    for (int i = 0; i < sc; i++) g_cfg.site_logins[i] = cfg->site_logins[i];
    g_cfg.site_login_count = sc;
    if (cfg->default_save_dir[0])
        strncpy(g_cfg.default_save_dir, cfg->default_save_dir,
                sizeof(g_cfg.default_save_dir) - 1);
    LeaveCriticalSection(&g_lock);
    throttle_reset();   /* 限速可能随本次配置变更，重置虚拟时钟 */
    /* 代理同步到全局请求选项 */
    dlmgr_set_proxy(g_cfg.proxy_type, g_cfg.proxy_host, g_cfg.proxy_port,
                    g_cfg.proxy_user, g_cfg.proxy_pass);
    /* dlmgr_set_proxy 会重置 g_net_opt（含 UA / 超时回默认），此处把用户在设置中
     * 配置的 UA 与连接超时重新套用，使「用户代理」「连接超时」两项设置真正生效。 */
    g_net_opt.user_agent = g_ua_buf;
    if (g_cfg.user_agent[0]) {
        strncpy(g_ua_buf, g_cfg.user_agent, sizeof(g_ua_buf) - 1);
        g_ua_buf[sizeof(g_ua_buf) - 1] = '\0';
    } else {
        strncpy(g_ua_buf, kDefaultUA, sizeof(g_ua_buf) - 1);
        g_ua_buf[sizeof(g_ua_buf) - 1] = '\0';
    }
    g_net_opt.connect_timeout_sec = (g_cfg.connect_timeout_sec > 0)
                                      ? g_cfg.connect_timeout_sec : 15;
}

/* 开始所有排队中的任务 */
int dlmgr_start_all(void) {
    if (!g_initialized) return -1;
    int started = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_PENDING || g_tasks[i].status == TASK_PAUSED) {
            dlmgr_start(g_tasks[i].task_id);
            started++;
        }
    }
    return started;
}

/* 停止所有运行中的任务 */
int dlmgr_stop_all(void) {
    if (!g_initialized) return -1;
    int stopped = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].status == TASK_RUNNING) {
            g_tasks[i].status = TASK_PAUSED;
            stopped++;
        }
    }
    return stopped;
}

/* 删除所有已完成的任务 */
int dlmgr_remove_completed(void) {
    if (!g_initialized) return -1;
    int removed = 0;
    EnterCriticalSection(&g_lock);
    for (int i = g_task_count - 1; i >= 0; i--) {
        if (g_tasks[i].status == TASK_COMPLETED || g_tasks[i].status == TASK_FAILED) {
            memmove(&g_tasks[i], &g_tasks[i+1],
                    (size_t)(g_task_count - i - 1) * sizeof(DownloadTask));
            g_task_count--;
            removed++;
        }
    }
    LeaveCriticalSection(&g_lock);
    return removed;
}

