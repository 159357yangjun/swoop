/*
 * engine_selftest.c — 下载引擎自测（可选目标，默认不构建）
 *
 * 为什么要它：本项目的教训是「编译通过 ≠ 功能生效」——出现过多个设置项只存盘、
 * 引擎从不下发的死设置。这个自测用**真实本地 HTTP 服务**验证设置链路端到端可用。
 *
 * 覆盖：
 *   1. 负向对照：未配置站点登录时 401 站点必须失败（证明第 2 项不是假阳性）
 *   2. 站点登录：配置凭据后同一 URL 必须下载成功，且落盘字节数一致
 *   3. 每服务器连接数：真实封顶并发分片数
 *   4. 中文文件名端到端落盘正确（UTF-8 路径）
 *   5. 「不使用代理」压得住环境变量代理
 *   6. 服务器宣称支持 Range 却无视它 → 必须自动降级为单连接，且内容逐字节正确
 *   7. >2GB 偏移写盘（32 位 long 截断探针：3 GiB 处写 4 KiB 再读回比对）
 *
 * 用法（先起服务，再跑自测）：
 *   python tools/engine_selftest_server.py 18080 idmuser idmpass &
 *   cmake -DIDM_BUILD_ENGINE_SELFTEST=ON -B build && cmake --build build --target engine_selftest
 *   ./build/engine_selftest.exe 18080 idmuser idmpass
 *
 * 退出码 = 失败项数量（0 表示全部通过）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>    /* time / gmtime / strftime（[20] Retry-After 的 HTTP-date 用例） */
#include <wchar.h>   /* wmemmove / wcsncpy（长路径前缀拼接用） */
#include <windows.h>

#include "download_core.h"

static int g_pass = 0, g_fail = 0;

static void check(const char *what, int ok, const char *detail)
{
    if (ok) {
        printf("  [PASS] %s\n", what);
        g_pass++;
    } else {
        printf("  [FAIL] %s%s%s\n", what,
               (detail && detail[0]) ? "  -> " : "",
               (detail && detail[0]) ? detail : "");
        g_fail++;
    }
}

/* 轮询等待任务结束（0 等待 / 1 下载中 / 2 暂停 / 3 完成 / 4 失败 / 5 取消） */
static int wait_task(int id, int timeout_ms, int *out_status, int64_t *out_downloaded)
{
    int waited = 0;
    while (waited < timeout_ms) {
        TaskInfo info;
        if (dlmgr_get_task_info(id, &info) != 0) return -1;
        if (info.status != 0 && info.status != 1) {
            if (out_status)     *out_status = info.status;
            if (out_downloaded) *out_downloaded = info.downloaded;
            return 0;
        }
        Sleep(100);
        waited += 100;
    }
    if (out_status) *out_status = -1;
    return -1;
}

/* ⚠️ 必须走 _wfopen + `\\?\` 长路径前缀：Windows 的 ANSI fopen 吃 UTF-8 路径会失败/乱码，
 * 这正是本项目修过的坑，测试自己也不能犯（否则会误报「文件不存在」）。
 * 长路径前缀逻辑与引擎一致：仅当绝对本地路径 ≥ 260 时才加 \\?\（单分量仍受 255 限制）。 */
static int test_to_wpath(const char *path, wchar_t *wpath, int wcap)
{
    wchar_t tmp[MAX_PATH * 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, tmp, (int)(sizeof(tmp) / sizeof(tmp[0])));
    if (n <= 0) return 0;
    size_t len = (size_t)(n - 1);
    if (len >= 260 && wcap > (int)len + 8 && tmp[1] == L':') {
        for (wchar_t *q = tmp; *q; q++) if (*q == L'/') *q = L'\\';
        wmemmove(tmp + 4, tmp, len + 1);
        tmp[0] = L'\\'; tmp[1] = L'\\'; tmp[2] = L'?'; tmp[3] = L'\\';
    }
    wcsncpy(wpath, tmp, (size_t)wcap - 1);
    wpath[wcap - 1] = L'\0';
    return 1;
}

static FILE *utf8_fopen_rb(const char *path)
{
    wchar_t wpath[MAX_PATH * 2];
    if (!test_to_wpath(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) return NULL;
    return _wfopen(wpath, L"rb");
}

/* 写文件（同样走 _wfopen），返回写入字节数，失败返回 -1 */
static long utf8_write_file(const char *path, const char *data)
{
    wchar_t wpath[MAX_PATH * 2];
    if (!test_to_wpath(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) return -1;
    FILE *f = _wfopen(wpath, L"wb");
    if (!f) return -1;
    size_t len = strlen(data);
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return (wr == len) ? (long)wr : -1;
}

/* 把整个文件读进 buf（已经保证 NUL 结尾），返回读到的字节数，失败返回 -1 */
static long utf8_read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = utf8_fopen_rb(path);
    if (!f) { if (cap) buf[0] = '\0'; return -1; }
    size_t rd = fread(buf, 1, cap - 1, f);
    buf[rd] = '\0';
    fclose(f);
    return (long)rd;
}

static long file_size_of(const char *path)
{
    FILE *f = utf8_fopen_rb(path);
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* 64 位版本：>2GB 的文件用 ftell(long) 会溢出，那正是本项目要测的缺陷之一，
 * 测试自己不能踩（否则「长度断言」永远失灵）。 */
static long long file_size_i64_of(const char *path)
{
    FILE *f = utf8_fopen_rb(path);
    if (!f) return -1;
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long long n = _ftelli64(f);
    fclose(f);
    return n;
}

/* 校验文件内容是否与测试服务端的确定性负载逐字节一致：byte[i] == i % 251。
 * 返回 1=一致，0=内容不一致，-1=长度不对，-2=读不到文件。
 * ⚠️ 为什么非要比对内容：本项目修过的缺陷正是「大小对、状态显示已完成、内容却错位」——
 * 只看长度或只看 status 都抓不到它。 */
static int content_matches_pattern(const char *path, long long expect_size)
{
    FILE *f = utf8_fopen_rb(path);
    if (!f) return -2;
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return -2; }
    long long n = _ftelli64(f);
    if (n != expect_size) { fclose(f); return -1; }
    if (_fseeki64(f, 0, SEEK_SET) != 0) { fclose(f); return -2; }

    unsigned char buf[65536];
    long long off = 0;
    int ok = 1;
    while (off < n) {
        long long left = n - off;
        size_t want = (size_t)(left > (long long)sizeof(buf) ? (long long)sizeof(buf) : left);
        size_t got = fread(buf, 1, want, f);
        if (got != want) { ok = 0; break; }
        for (size_t k = 0; k < got; k++) {
            if (buf[k] != (unsigned char)((off + (long long)k) % 251)) { ok = 0; break; }
        }
        if (!ok) break;
        off += (long long)got;
    }
    fclose(f);
    return ok ? 1 : 0;
}

/* 等待任务进入**终态**（完成 3 / 失败 4）。
 * ⚠️ 不能用 wait_task：不支持 Range 的降级重下会瞬时把任务置为「已取消」(5)，
 * 那个中间态会被 wait_task 当成结束，用例就会误判为失败。 */
static int wait_final(int id, int timeout_ms, int *out_status, int64_t *out_downloaded)
{
    int waited = 0;
    while (waited < timeout_ms) {
        TaskInfo info;
        if (dlmgr_get_task_info(id, &info) != 0) return -1;
        if (info.status == 3 || info.status == 4) {
            if (out_status)     *out_status = info.status;
            if (out_downloaded) *out_downloaded = info.downloaded;
            return 0;
        }
        Sleep(100);
        waited += 100;
    }
    if (out_status) *out_status = -1;
    return -1;
}

/* 删除测试产物（含 .idmtmp）。必须走宽字符 API，理由同 utf8_fopen_rb。
 * ⚠️ 不做这一步的话，上一轮遗留的产物会让「落盘文件与下载字节一致」在
 * 本轮完全失败的情况下依然通过——测试自己制造假阳性，比没有测试更糟。 */
static void utf8_delete(const char *path)
{
    wchar_t wpath[MAX_PATH * 2];
    if (!test_to_wpath(path, wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))))
        return;
    DeleteFileW(wpath);
}

/* 清除本轮会写入的所有产物，返回清理前残留的非空文件数（>0 说明上轮有遗留） */
static int clean_outputs(const char *outdir)
{
    static const char *names[] = { "noauth.bin", "withauth.bin", "nocap.bin",
                                   "cap.bin", "shared.bin", "norange.bin",
                                   "bigoffset.bin", "ra.bin", "lp.bin",
                                   "cut.bin", "cut2.bin", "cutnr.bin",
                                   "\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d\xe6\x96\x87\xe4\xbb\xb6.bin" };
    int leftover = 0;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        char p[MAX_PATH + 64];
        snprintf(p, sizeof(p), "%s\\%s", outdir, names[i]);
        if (file_size_of(p) > 0) leftover++;
        utf8_delete(p);
        snprintf(p, sizeof(p), "%s\\%s.idmtmp", outdir, names[i]);
        utf8_delete(p);
    }
    return leftover;
}

/* ── 测试服务的并发统计（免认证接口 /_stats、/_reset）── */
/* 清零服务端统计，使每次测得的并发峰值只反映本轮任务 */
static void server_reset(int port)
{
    char u[256];
    snprintf(u, sizeof(u), "http://127.0.0.1:%d/_reset", port);
    NetResponse r = network_get_html(u, NULL);
    free(r.data);
}

/* 取 /_stats 里的整数字段，如 "max_cur"。取不到返回 -1。 */
static int server_stat(int port, const char *key)
{
    char u[256];
    snprintf(u, sizeof(u), "http://127.0.0.1:%d/_stats", port);
    NetResponse r = network_get_html(u, NULL);
    if (!r.data) return -1;
    int val = -1;
    const char *p = strstr(r.data, key);
    if (p) {
        p = strchr(p, ':');
        if (p) val = atoi(p + 1);
    }
    free(r.data);
    return val;
}

/* 打印完整 /_stats（诊断用） */
static void server_dump_stats(int port, const char *tag)
{
    char u[256];
    snprintf(u, sizeof(u), "http://127.0.0.1:%d/_stats", port);
    NetResponse r = network_get_html(u, NULL);
    printf("      [stats:%s] %s\n", tag, r.data ? r.data : "(null)");
    free(r.data);
}

int main(int argc, char **argv)
{
    /* 关掉 stdout 缓冲：自测若在收尾阶段崩掉，全缓冲会把最后一批输出整体丢掉，
     * 现场只剩半句断言文字，完全看不出后面发生了什么。
     * （真实踩过：输出停在「多级目录」，实际是缓冲未 flush。） */
    setvbuf(stdout, NULL, _IONBF, 0);

    int port = (argc > 1) ? atoi(argv[1]) : 18080;
    const char *user = (argc > 2) ? argv[2] : "idmuser";
    const char *pass = (argc > 3) ? argv[3] : "idmpass";

    char url[512], outdir[MAX_PATH];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/secret.bin", port);
    snprintf(outdir, sizeof(outdir), "%s\\idm_selftest_out",
             getenv("TEMP") ? getenv("TEMP") : ".");
    CreateDirectoryA(outdir, NULL);

    printf("== 下载引擎自测（本地带 Basic 认证的 HTTP 服务 127.0.0.1:%d）==\n", port);
    dlmgr_init(NULL);

    /* 先等服务端真的开始监听再开测。
     * ⚠️ 这不是保险起见：服务端是 Python，进程起来了但 socket 还没 listen 的那一两百毫秒里
     * 发请求会得到「连不上」，而 [0] HEAD 探测会把它记成 file_size=-1 —— 一条与代码无关的
     * 假失败；更坏的是后面「无凭据必须失败」那类用例反而会因此假通过。
     * 用 /_stats 轮询到有响应为止，把启动竞态从结果里彻底剔掉。 */
    {
        int ready = 0;
        for (int i = 0; i < 100 && !ready; i++) {
            if (server_stat(port, "auth_ok") >= 0) { ready = 1; break; }
            Sleep(100);
        }
        printf("    服务端就绪检查：%s\n", ready ? "已响应 /_stats" : "★超时，后续结果不可信");
    }

    {
        int leftover = clean_outputs(outdir);
        printf("    输出目录 %s（清理了 %d 个上轮遗留产物）\n", outdir, leftover);
    }

    /* ── 0. HEAD 探测：大小 / Range 支持 ──
     * 这是分段下载的前提：探测拿不到大小 → piece 数只能是 1 → 多线程引擎完全不参与。
     * 所以单独断言一次，避免「下载成功」掩盖「根本没分段」。 */
    printf("\n[0] HEAD 探测：文件大小与 Range 支持\n");
    {
        char huge[512];
        snprintf(huge, sizeof(huge), "http://127.0.0.1:%d/huge.bin", port);
        NetOptions opt = network_default_options();
        char au[128], ap[128];
        strncpy(au, user, sizeof(au) - 1); au[sizeof(au) - 1] = '\0';
        strncpy(ap, pass, sizeof(ap) - 1); ap[sizeof(ap) - 1] = '\0';
        opt.auth_user = au;
        opt.auth_pass = ap;

        NetworkProbe pr = network_probe(huge, &opt);
        printf("      success=%d file_size=%lld supports_range=%d http=%ld ver=%s\n",
               pr.success, (long long)pr.file_size, pr.supports_range,
               pr.http_code, pr.http_version);
        char d[200];
        snprintf(d, sizeof(d), "期望 %d，实得 %lld", 40 * 1024 * 1024, (long long)pr.file_size);
        check("探测到正确的文件大小", pr.file_size == 40 * 1024 * 1024, d);
        snprintf(d, sizeof(d), "supports_range=%d", pr.supports_range);
        check("探测到服务器支持 Range", pr.supports_range == 1, d);
    }

    /* ── 1. 负向对照：无凭据必须失败 ── */
    printf("\n[1] 未配置站点登录 → 401 站点应当失败\n");
    {
        int id = dlmgr_add(url, outdir, "noauth.bin", 4, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 15000, &st, &dl);
            char d[160];
            snprintf(d, sizeof(d), "status=%d downloaded=%lld", st, (long long)dl);
            check("无凭据时判定为失败（status=4）", st == 4, d);
            dlmgr_remove(id);
        }
    }

    /* ── 2. 站点登录：必须成功 ── */
    printf("\n[2] 配置站点登录 → 同一 URL 应当成功\n");
    {
        DownloadConfig cfg = dlmgr_get_config();
        memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
        cfg.site_login_count = 1;
        strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
        strncpy(cfg.site_logins[0].user,  user,      sizeof(cfg.site_logins[0].user) - 1);
        strncpy(cfg.site_logins[0].pass,  pass,      sizeof(cfg.site_logins[0].pass) - 1);
        dlmgr_set_config(&cfg);

        int id = dlmgr_add(url, outdir, "withauth.bin", 4, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            char d[160];
            snprintf(d, sizeof(d), "status=%d downloaded=%lld", st, (long long)dl);
            check("带凭据下载完成（status=3）", st == 3 && dl > 0, d);

            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\withauth.bin", outdir);
            long fsz = file_size_of(path);
            snprintf(d, sizeof(d), "file=%ld downloaded=%lld", fsz, (long long)dl);
            check("落盘文件与下载字节一致", fsz > 0 && fsz == (long)dl, d);
            dlmgr_remove(id);
        }
    }

    /* ── 3. 每服务器连接数：验证**真实并发连接数**被封顶 ──
     * /huge.bin 是 40 MiB，DEFAULT_CHUNK_SIZE 4 MiB → by_size = 10 片。
     * 请求线程数 8：不封顶时并发应为 min(8, 10) = 8；
     * 设 max_conn_per_server = 2 后并发必须 ≤ 2。
     * 分片数只是弱代理（3 片时无论封不封顶都 ≤3），所以这里直接看服务端的并发峰值。 */
    printf("\n[3] 每服务器连接数：真实并发连接数封顶（服务端统计并发峰值）\n");
    {
        char huge[512];
        snprintf(huge, sizeof(huge), "http://127.0.0.1:%d/huge.bin", port);

        /* 3a. 不封顶（对照） */
        {
            DownloadConfig cfg = dlmgr_get_config();
            cfg.max_conn_per_server = 0;
            cfg.site_login_count = 1;
            dlmgr_set_config(&cfg);
            server_reset(port);

            int id = dlmgr_add(huge, outdir, "nocap.bin", 8, NULL, NULL, NULL, NULL);
            check("任务创建成功", id >= 0, "");
            if (id >= 0) {
                dlmgr_start(id);
                int st = 0; int64_t dl = 0;
                wait_task(id, 120000, &st, &dl);
                int peak = server_stat(port, "max_cur");
                server_dump_stats(port, "3a 不封顶");
                TaskInfo ti;
                if (dlmgr_get_task_info(id, &ti) == 0)
                    printf("      chunk_count=%d（40MiB/4MiB 期望 10）\n", ti.chunk_count);
                char d[200];
                snprintf(d, sizeof(d), "status=%d downloaded=%lld 并发峰值=%d（期望 >=4）",
                         st, (long long)dl, peak);
                check("不封顶时确实并发（对照有效）", st == 3 && peak >= 4, d);
                dlmgr_remove(id);
            }
        }

        /* 3b. 封顶 2 */
        {
            DownloadConfig cfg = dlmgr_get_config();
            cfg.max_conn_per_server = 2;
            cfg.site_login_count = 1;
            dlmgr_set_config(&cfg);
            server_reset(port);

            int id = dlmgr_add(huge, outdir, "cap.bin", 8, NULL, NULL, NULL, NULL);
            check("任务创建成功", id >= 0, "");
            if (id >= 0) {
                dlmgr_start(id);
                int st = 0; int64_t dl = 0;
                wait_task(id, 120000, &st, &dl);
                int peak = server_stat(port, "max_cur");
                server_dump_stats(port, "3b 封顶2");
                char d[200];
                snprintf(d, sizeof(d), "status=%d downloaded=%lld 并发峰值=%d（期望 <=2）",
                         st, (long long)dl, peak);
                check("并发被 max_conn_per_server 封顶到 2", st == 3 && peak > 0 && peak <= 2, d);
                dlmgr_remove(id);
            }
        }
    }

    /* ── 4. 中文文件名端到端 ── */
    printf("\n[4] 中文文件名落盘\n");
    {
        /* UTF-8 字节：中文名文件.bin */
        const char *cn = "\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d\xe6\x96\x87\xe4\xbb\xb6.bin";
        int id = dlmgr_add(url, outdir, cn, 2, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\%s", outdir, cn);
            long fsz = file_size_of(path);
            char d[MAX_PATH + 96];
            snprintf(d, sizeof(d), "status=%d file=%ld path=%s", st, fsz, path);
            check("中文文件名文件存在于磁盘且非空", st == 3 && fsz > 0, d);
            dlmgr_remove(id);
        }
    }

    /* ── 5. 「不使用代理」必须压得住环境变量代理 ──
     * libcurl 默认会读 http_proxy / HTTPS_PROXY / all_proxy。若不显式下发
     * CURLOPT_PROXY，用户界面上选的「直连」会被环境变量悄悄推翻：请求被送进
     * 代理，本机服务收到的请求行变成绝对 URL → 404；或者直接被代理拦成 401。
     * 这是真实踩过的坑（不是假想），所以固定成回归项。 */
    printf("\n[5] 「不使用代理」不得被环境变量代理覆盖\n");
    {
        char small[512];
        snprintf(small, sizeof(small), "http://127.0.0.1:%d/small.bin", port);

        /* ⚠️ 不能只「看看进程里有没有 http_proxy」然后照样 PASS——环境变量本来就没设时，
         *    这个用例什么也没验证，却永远是绿的（不会失败的测试等于没测）。
         *    必须自己塞一个**必然连不通**的代理进去：只有引擎真的显式下发了
         *    CURLOPT_PROXY=""，直连才会成功；若引擎放任 libcurl 去读环境变量，
         *    请求就会被打到 127.0.0.1:9（discard 端口，没人监听）上、以连接失败告终。
         *    这样本用例在「环境本来干净」和「环境本来有代理」两种机器上都真的在测东西。 */
        static char envbuf[512];
        char saved[512] = {0};
        const char *hp = getenv("http_proxy");
        const int had_hp = (hp && hp[0]) ? 1 : 0;
        if (had_hp) snprintf(saved, sizeof(saved), "%s", hp);
        snprintf(envbuf, sizeof(envbuf), "http_proxy=http://127.0.0.1:9");
        _putenv(envbuf);
        const char *now = getenv("http_proxy");
        printf("      强制注入 http_proxy=%s（原有值：%s）\n",
               (now && now[0]) ? now : "(注入失败)", had_hp ? saved : "(未设置)");
        /* 注入失败就必须判失败，否则本用例又会退化成永远绿的摆设。 */
        check("已注入一个必然连不通的代理（确保本用例真的在测东西）",
              now && now[0] != '\0', "");

        DownloadConfig cfg = dlmgr_get_config();
        cfg.proxy_type = PROXY_NONE;
        cfg.site_login_count = 1;
        dlmgr_set_config(&cfg);

        int id = dlmgr_add(small, outdir, "shared.bin", 2, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            char path[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\shared.bin", outdir);
            long fsz = file_size_of(path);
            char d[200];
            snprintf(d, sizeof(d), "status=%d downloaded=%lld file=%ld",
                     st, (long long)dl, fsz);
            check("界面选「不使用代理」时，环境里的 http_proxy 被压住、真直连成功",
                  st == 3 && fsz == 1024 * 1024, d);
            dlmgr_remove(id);
        }

        /* 还原环境，别把副作用带给后面的用例 */
        snprintf(envbuf, sizeof(envbuf), "http_proxy=%s", had_hp ? saved : "");
        _putenv(envbuf);
    }

    /* ── 6. 服务器宣称支持 Range、实际无视它 ──
     * /norange.bin 的 HEAD 回 Accept-Ranges: bytes，GET 时却永远 200 + 整份文件。
     * 引擎若按「分片偏移顺序写」处理，就会把从 0 开始的全量数据灌进分片位置，
     * 产出一个大小看着对、内容整体错位、状态还显示「已完成」的文件。
     * 期望：识别出服务器不吃 Range，自动降级为单连接重下，落盘内容逐字节正确。 */
    printf("\n[6] 服务器无视 Range → 必须自动降级为单连接，且内容正确\n");
    {
        char nu[512], path[MAX_PATH + 64];
        snprintf(nu, sizeof(nu), "http://127.0.0.1:%d/norange.bin", port);
        snprintf(path, sizeof(path), "%s\\norange.bin", outdir);
        utf8_delete(path);
        {   char t[MAX_PATH + 64];
            snprintf(t, sizeof(t), "%s.idmtmp", path); utf8_delete(t); }

        server_reset(port);
        /* 站点登录仍按上一节配置？[5] 只改了 proxy_type，site_login_count 保持 1，无需重设 */

        int id = dlmgr_add(nu, outdir, "norange.bin", 4, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_final(id, 120000, &st, &dl);

            int ranged = server_stat(port, "norange_range");
            char d[240];
            snprintf(d, sizeof(d), "服务端收到的带 Range 请求数=%d", ranged);
            /* 没有这条断言，本用例可能在「引擎压根没分段」的情况下永远绿——那是假阳性 */
            check("确实向服务端发过 Range 请求（确保本用例真的触发了该场景）", ranged > 0, d);

            snprintf(d, sizeof(d), "status=%d downloaded=%lld", st, (long long)dl);
            check("降级重下后完成（status=3）", st == 3, d);

            int mc = content_matches_pattern(path, 12LL * 1024 * 1024);
            snprintf(d, sizeof(d), "file_size=%lld 比对结果=%d（1=一致 0=内容错位 -1=长度不对 -2=无文件）",
                     file_size_i64_of(path), mc);
            check("落盘内容与源文件逐字节一致（不是「大小对但内容错位」）", mc == 1, d);
            dlmgr_remove(id);
        }
    }

    /* ── 7. >2GB 偏移写盘：32 位 long 截断的直接探针 ──
     * 在 3 GiB（跨越 2^31）处请求 4 KiB。Windows 上 long 是 32 位，
     * 若偏移被 `(long)` 截断，数据就会写到别的offset上，文件长度随之变成
     * 「被截断后的偏移 + 4096」——长度断言即可抓到；再补一次内容比对堵死角。
     * 只传 4 KiB，所以不需要真的下 3 GiB。 */
    printf("\n[7] >2GB 偏移写盘（32 位 long 截断探针）\n");
    {
        const long long BIG = 3LL * 1024 * 1024 * 1024;   /* 3 GiB */
        const long long LEN = 4096;
        char bu[512], path[MAX_PATH + 64];
        snprintf(bu, sizeof(bu), "http://127.0.0.1:%d/bigoffset.bin", port);
        snprintf(path, sizeof(path), "%s\\bigoffset.bin", outdir);
        utf8_delete(path);

        NetOptions opt = network_default_options();
        char au[128], ap[128];
        strncpy(au, user, sizeof(au) - 1); au[sizeof(au) - 1] = '\0';
        strncpy(ap, pass, sizeof(ap) - 1); ap[sizeof(ap) - 1] = '\0';
        opt.auth_user = au;
        opt.auth_pass = ap;

        NetDownloadTask nd;
        memset(&nd, 0, sizeof(nd));
        nd.url         = bu;
        nd.save_path   = path;
        nd.range_start = BIG;
        nd.range_end   = BIG + LEN - 1;
        nd.opt         = &opt;

        NetDownloadResult r = network_download_range(&nd);
        char d[240];
        snprintf(d, sizeof(d), "success=%d http=%ld written=%lld err=%s",
                 r.success, r.http_code, (long long)r.bytes_written, r.error_msg);
        check("3 GiB 偏移处的 Range 请求成功（HTTP 206）",
              r.success && r.http_code == 206, d);

        long long fsz = file_size_i64_of(path);
        snprintf(d, sizeof(d), "file=%lld 期望=%lld（偏移+长度）", fsz, BIG + LEN);
        check("文件长度 = 3GiB + 4KiB（偏移没有被截断到低位）", fsz == BIG + LEN, d);

        /* 内容比对：把 3 GiB 处那 4 KiB 读回来，逐字节核对 */
        int mc = 0;
        {
            FILE *f = utf8_fopen_rb(path);
            if (f && _fseeki64(f, BIG, SEEK_SET) == 0) {
                unsigned char buf[4096];
                size_t got = fread(buf, 1, sizeof(buf), f);
                mc = (got == sizeof(buf)) ? 1 : 0;
                for (size_t k = 0; mc == 1 && k < got; k++) {
                    if (buf[k] != (unsigned char)((BIG + (long long)k) % 251)) mc = 0;
                }
            }
            if (f) fclose(f);
        }
        snprintf(d, sizeof(d), "比对结果=%d（1=一致）", mc);
        check("3 GiB 处读回的字节与源数据一致（数据没写错位置）", mc == 1, d);

        utf8_delete(path);
    }

    /* ── 8. 暂停排队中的任务：状态必须真的变成「已暂停」──
     * 原实现只在 RUNNING 时改状态、对 PENDING 静默返回 0，于是 GUI 把行标成「已暂停」
     * 而引擎里还是 PENDING —— 退出后以 PENDING 落盘，下次启动队列调度器照样拉起来下载。
     * 这条同时是「暂停→继续」的回归闸：need_size 若仍按「状态==PENDING」判断，
     * 继续时就会跳过探测分段，chunk_count 停在 0，线程拿着 0 个分片空转到超时。 */
    printf("\n[8] 暂停排队中的任务（PENDING → PAUSED）与暂停后继续\n");
    {
        char su[512], sp[MAX_PATH + 64];
        snprintf(su, sizeof(su), "http://127.0.0.1:%d/small.bin", port);
        snprintf(sp, sizeof(sp), "%s\\pausetest.bin", outdir);
        utf8_delete(sp);

        int id = dlmgr_add(su, outdir, "pausetest.bin", 4, NULL, NULL, NULL, NULL);
        TaskInfo ti;
        char d[240];

        memset(&ti, 0, sizeof(ti));
        int got0 = (dlmgr_get_task_info(id, &ti) == 0);
        snprintf(d, sizeof(d), "id=%d status=%d（0=等待下载）", id, got0 ? ti.status : -1);
        check("新建任务是排队中（PENDING）而不是已在下载", got0 && ti.status == 0, d);

        int prc = dlmgr_pause(id);
        memset(&ti, 0, sizeof(ti));
        int got1 = (dlmgr_get_task_info(id, &ti) == 0);
        snprintf(d, sizeof(d), "pause 返回=%d，status=%d（2=已暂停）", prc, got1 ? ti.status : -1);
        check("暂停排队中的任务后状态为「已暂停」", prc == 0 && got1 && ti.status == 2, d);

        /* 落盘再读回：暂停必须写进状态文件，否则重启后它会自己开始 */
        char stPath[MAX_PATH + 64];
        snprintf(stPath, sizeof(stPath), "%s\\pausetest.json", outdir);
        int sv = dlmgr_save_state(stPath);
        char want[64];
        snprintf(want, sizeof(want), "\"id\":%d", id);
        char body[16384];
        body[0] = '\0';
        utf8_read_file(stPath, body, sizeof(body));
        char *obj = strstr(body, want);
        int persisted = 0;
        if (obj) {
            char *st = strstr(obj, "\"status\":");
            if (st) persisted = (atoi(st + 9) == 2);
        }
        snprintf(d, sizeof(d), "save 返回=%d，文件里该任务 status=%s",
                 sv, persisted ? "2" : "(不是 2)");
        check("暂停状态已落盘（下次启动不会自动开始）", sv == 0 && persisted, d);

        /* 继续：这里才是 need_size 判据的回归点 */
        dlmgr_start(id);
        int st = -1; int64_t dl = 0;
        int fin = wait_final(id, 15000, &st, &dl);
        memset(&ti, 0, sizeof(ti));
        dlmgr_get_task_info(id, &ti);
        snprintf(d, sizeof(d), "wait=%d status=%d chunk_count=%d downloaded=%lld",
                 fin, st, ti.chunk_count, (long long)dl);
        /* chunk_count > 0 是关键：暂停后继续若跳过探测，这里会是 0（线程空转） */
        /* wait_final 成功返回 0（沿用既有约定），别写成 `if (fin && ...)` 反过来 */
        check("暂停后继续能真正下完（status=3）", fin == 0 && st == 3, d);
        check("继续时确实做了分段（chunk_count>0，不是空转）", ti.chunk_count > 0, d);

        int mc = content_matches_pattern(sp, 1024 * 1024);
        snprintf(d, sizeof(d), "file_size=%lld 比对=%d（1=逐字节一致）",
                 file_size_i64_of(sp), mc);
        check("继续后落盘内容逐字节正确", mc == 1, d);

        dlmgr_remove(id);
        utf8_delete(sp);
        utf8_delete(stPath);
    }

    /* ── 9. 状态文件转义：写→读→再写，字符串一字不差 ──
     * 原实现直接 %s 把路径写进 JSON。Windows 目录里全是反斜杠，产出的是 "\Users" 这种
     * 非法转义序列 —— 严格解析器（QJsonDocument）会因一个字符读不出整份文件，所有任务
     * 一起丢。这里手工造一份「转义版」状态文件，加载后再存一次，两份里该任务的
     * url/file/dir 三元组必须完全相同（能发现「转义了但没还原」这类半吊子修法）。 */
    printf("\n[9] 状态文件 JSON 转义：含反斜杠与引号的路径往返\n");
    {
        char fA[MAX_PATH + 64], fB[MAX_PATH + 64];
        snprintf(fA, sizeof(fA), "%s\\esc_a.json", outdir);
        snprintf(fB, sizeof(fB), "%s\\esc_b.json", outdir);

        /* 与写入器完全同构的布局；字符串部分已按 JSON 规则转义：
         *   dir  = D:\idm test\子目录   → "D:\\idm test\\子目录"
         *   file = say "hi".bin        → "say \"hi\".bin"
         *   url  = https://x/a?q=1\2   → "https://x/a?q=1\\2" */
        const char *content =
            "{\n  \"idmFormat\": 2,\n  \"tasks\": [\n"
            "    {\"id\":9001, \"url\":\"https://ex.test/a?q=1\\\\2\", "
            "\"file\":\"say \\\"hi\\\".bin\", \"dir\":\"D:\\\\idm test\\\\sub\", "
            "\"size\":1024, \"downloaded\":0, \"status\":2,\n"
            "     \"chunks\":[] }\n"
            "  ]\n}\n";
        {
            long wr = utf8_write_file(fA, content);
            (void)wr;
        }

        int ld = dlmgr_load_state(fA);
        TaskInfo ti;
        memset(&ti, 0, sizeof(ti));
        int got = (dlmgr_get_task_info(9001, &ti) == 0);
        char d[240];
        snprintf(d, sizeof(d), "load 返回=%d 能否按 id 找到=%d status=%d",
                 ld, got, got ? ti.status : -1);
        check("转义版状态文件能被加载（id=9001 可查）", ld == 0 && got, d);

        /* 文件名里的引号必须原样还原，不能把 \" 之后的半截当成结束 */
        snprintf(d, sizeof(d), "filename=「%s」期望「say \"hi\".bin」", ti.filename);
        check("文件名中的引号被正确还原", strcmp(ti.filename, "say \"hi\".bin") == 0, d);

        int sv = dlmgr_save_state(fB);
        /* 同一个任务对象里的 "url"/"file"/"dir" 三元组，两份文件应逐字符相同。
         * 这是「转义 + 还原」闭环的直接证据：只转义不还原的话第二次写出的内容会变。 */
        char ta[1024] = "", tb[1024] = "";
        const char *keys = "\"url\":";
        const char *stop = "\", \"size\":";
        char wholeA[16384], wholeB[16384];
        wholeA[0] = wholeB[0] = '\0';
        utf8_read_file(fA, wholeA, sizeof(wholeA));
        utf8_read_file(fB, wholeB, sizeof(wholeB));
        char *a = strstr(wholeA, keys); char *z = a ? strstr(a, stop) : NULL;
        if (a && z) { size_t len = (size_t)(z - a); if (len < sizeof(ta)) { memcpy(ta, a, len); ta[len] = '\0'; } }
        char *b = strstr(wholeB, keys); char *z2 = b ? strstr(b, stop) : NULL;
        if (b && z2) { size_t len2 = (size_t)(z2 - b); if (len2 < sizeof(tb)) { memcpy(tb, b, len2); tb[len2] = '\0'; } }
        snprintf(d, sizeof(d), "写回=%d\n      原: %s\n      新: %s", sv, ta, tb);
        check("加载后再存一次，url/file/dir 三元组逐字符不变",
              sv == 0 && ta[0] && strcmp(ta, tb) == 0, d);

        dlmgr_remove(9001);
        utf8_delete(fA);
        utf8_delete(fB);
    }

    /* ── 10. 乱序 task_id 的状态文件：加载后仍能按 id 找到 ──
     * find_task 是二分查找，依赖数组按 task_id 升序。文件顺序 = 数组顺序（未修时），
     * 这里故意把 9001 放在最后：从 [9002, 9003, 9001] 里找 9001，二分会在前半段
     * 反复收窄后判定「不存在」。修好之后（加载末尾统一排序）三个 id 都必须可查。 */
    printf("\n[10] 乱序 task_id 的状态文件（二分查找不变量）\n");
    {
        char fp[MAX_PATH + 64];
        snprintf(fp, sizeof(fp), "%s\\unordered.json", outdir);
        const char *content =
            "{\n  \"idmFormat\": 2,\n  \"tasks\": [\n"
            "    {\"id\":9002, \"url\":\"https://ex.test/b2\", \"file\":\"b2\", \"dir\":\"D:\\\\u\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] },\n"
            "    {\"id\":9003, \"url\":\"https://ex.test/b3\", \"file\":\"b3\", \"dir\":\"D:\\\\u\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] },\n"
            "    {\"id\":9001, \"url\":\"https://ex.test/b1\", \"file\":\"b1\", \"dir\":\"D:\\\\u\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] }\n"
            "  ]\n}\n";
        {
            long wr = utf8_write_file(fp, content);
            (void)wr;
        }

        int ld = dlmgr_load_state(fp);
        TaskInfo ti;
        char d[240];
        int ok2 = 0, ok3 = 0, ok1 = 0;
        memset(&ti, 0, sizeof(ti)); if (dlmgr_get_task_info(9002, &ti) == 0) ok2 = (ti.task_id == 9002);
        memset(&ti, 0, sizeof(ti)); if (dlmgr_get_task_info(9003, &ti) == 0) ok3 = (ti.task_id == 9003);
        memset(&ti, 0, sizeof(ti)); if (dlmgr_get_task_info(9001, &ti) == 0) ok1 = (ti.task_id == 9001);
        snprintf(d, sizeof(d), "load=%d 可查: 9001=%d 9002=%d 9003=%d", ld, ok1, ok2, ok3);
        check("乱序文件加载后三个 id 都能查到（9001 是修复前必失的那个）",
              ld == 0 && ok1 && ok2 && ok3, d);

        /* 顺带确认「暂停/删除」这类按 id 操作也真的生效（都走 find_task） */
        int prc = dlmgr_pause(9001);
        memset(&ti, 0, sizeof(ti));
        dlmgr_get_task_info(9001, &ti);
        snprintf(d, sizeof(d), "pause 返回=%d status=%d", prc, ti.status);
        check("乱序文件里的任务同样能被暂停", prc == 0 && ti.status == 2, d);

        dlmgr_remove(9001);
        dlmgr_remove(9002);
        dlmgr_remove(9003);
        utf8_delete(fp);
    }

    /* ── 11. 重复 load 的幂等性：同一进程第二次 load 不得让队列翻倍 ──
     * 原实现只往 g_tasks[] 尾部追加，所以「文件里 3 条」加载两次得到 6 条，
     * 而且 task_id 成对重复 —— find_task 的二分会在同一条上反复命中，
     * 「任务数莫名变多 / 删了一条还剩一条」这类现象都由此而来。
     * 真实触发路径不是理论推演：走查工具里连开两个 MainWindow（各构造一次），
     * 用户目录里的 tasks.json 就从 61 条涨到 117 条并被写回磁盘。
     * 这里直接连 load 两次，第二次必须仍是 3 条且 id 不重复。 */
    printf("\n[11] 重复 dlmgr_load_state 的幂等性（队列不得翻倍）\n");
    {
        char fp[MAX_PATH + 64];
        snprintf(fp, sizeof(fp), "%s\\idempotent.json", outdir);
        const char *content =
            "{\n  \"idmFormat\": 2,\n  \"tasks\": [\n"
            "    {\"id\":9101, \"url\":\"https://ex.test/i1\", \"file\":\"i1\", \"dir\":\"D:\\\\i\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] },\n"
            "    {\"id\":9102, \"url\":\"https://ex.test/i2\", \"file\":\"i2\", \"dir\":\"D:\\\\i\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] },\n"
            "    {\"id\":9103, \"url\":\"https://ex.test/i3\", \"file\":\"i3\", \"dir\":\"D:\\\\i\", "
            "\"size\":10, \"downloaded\":0, \"status\":2,\n     \"chunks\":[] }\n"
            "  ]\n}\n";
        { long wr = utf8_write_file(fp, content); (void)wr; }

        int ids[64];
        char d[240];

        int ld1 = dlmgr_load_state(fp);
        int n1 = dlmgr_list(ids, 64);

        /* 再来一次：模拟「同一进程里又构造了一次主窗口」 */
        int ld2 = dlmgr_load_state(fp);
        int n2 = dlmgr_list(ids, 64);

        snprintf(d, sizeof(d), "第一次 load=%d 得到 %d 条；第二次 load=%d 得到 %d 条（期望 3/3）",
                 ld1, n1, ld2, n2);
        check("同一进程重复 load 后任务数不翻倍", ld1 == 0 && ld2 == 0 && n1 == 3 && n2 == 3, d);

        /* 数量对了还不够：重复 id 会让二分查找退化，逐个确认三条都在 */
        int c1 = 0, c2 = 0, c3 = 0;
        for (int i = 0; i < n2; i++) {
            if (ids[i] == 9101) c1++;
            else if (ids[i] == 9102) c2++;
            else if (ids[i] == 9103) c3++;
        }
        snprintf(d, sizeof(d), "id 出现次数: 9101×%d 9102×%d 9103×%d（各应为 1 次）", c1, c2, c3);
        check("重复 load 后 task_id 不重复、三条任务都在", c1 == 1 && c2 == 1 && c3 == 1, d);

        /* 按 id 操作仍要命中唯一那条 */
        TaskInfo ti;
        memset(&ti, 0, sizeof(ti));
        int ok = (dlmgr_get_task_info(9102, &ti) == 0) && ti.task_id == 9102;
        snprintf(d, sizeof(d), "get_task_info(9102) 成功=%d", ok);
        check("重复 load 后按 id 查任务仍然唯一且正确", ok, d);

        dlmgr_remove(9101);
        dlmgr_remove(9102);
        dlmgr_remove(9103);
        utf8_delete(fp);
    }

    /* ── 12. 全局限速：节流必须真的作用在吞吐上 ──
     * 1 MiB 文件 + 256 KB/s，理论耗时 4000 ms，断言 >= 2400 ms；再配一条不限速对照，
     * 防止「机器就是慢」蒙混过关。
     *
     * 实测（本机）：新虚拟时钟 4188 ms、旧 100ms 滑窗 4140 ms —— 两者精度相当，
     * 所以本项**不是**用来区分新旧实现的，而是「限速器被改坏」的回归闸门：
     * 把 throttle_limit 改成直接 return（模拟限速失效）后实测 109 ms，
     * 本项立刻变红（40 通过变 38 通过、2 失败，退出码 0 变 2）。 */
    printf("\n[12] 全局限速 256 KB/s：耗时必须接近理论值\n");
    {
        char small[512], d[240];
        snprintf(small, sizeof(small), "http://127.0.0.1:%d/small.bin", port);

        const int     LIMIT   = 256 * 1024;                              /* 256 KB/s */
        const int64_t WANT_MS = (int64_t)(1024 * 1024) * 1000 / LIMIT;   /* 1 MiB ⇒ 4000 ms */

        dlmgr_set_speed_limit(LIMIT);
        int64_t t0 = dl_time_ms();
        int id = dlmgr_add(small, outdir, "throttle_limited.bin", 2, NULL, NULL, NULL, NULL);
        check("限速任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            int64_t dt = dl_time_ms() - t0;

            snprintf(d, sizeof(d),
                     "256 KB/s 下 1 MiB 耗时 %lld ms（理论 %lld ms，判定下限 %lld ms）",
                     (long long)dt, (long long)WANT_MS, (long long)(WANT_MS * 6 / 10));
            printf("    · 实测 %lld ms / 理论 %lld ms（%d%%）\n",
                   (long long)dt, (long long)WANT_MS,
                   (int)(WANT_MS > 0 ? dt * 100 / WANT_MS : 0));
            check("限速下载完成且耗时 ≥ 理论值 60%（未走块放行的空子）",
                  st == 3 && dt >= WANT_MS * 6 / 10, d);

            /* 不限速对照：同样的文件必须明显更快，否则上面的「慢」可能另有原因 */
            dlmgr_set_speed_limit(0);
            int64_t t1 = dl_time_ms();
            int id2 = dlmgr_add(small, outdir, "throttle_free.bin", 2, NULL, NULL, NULL, NULL);
            if (id2 >= 0) {
                dlmgr_start(id2);
                int st2 = 0; int64_t dl2 = 0;
                wait_task(id2, 60000, &st2, &dl2);
                int64_t dt2 = dl_time_ms() - t1;
                snprintf(d, sizeof(d), "不限速 %lld ms vs 限速 %lld ms",
                         (long long)dt2, (long long)dt);
                check("取消限速后明显更快（证明限速确实在起作用）",
                      st2 == 3 && dt2 * 2 < dt, d);
                dlmgr_remove(id2);
            }
            dlmgr_remove(id);
        }
        dlmgr_set_speed_limit(0);
    }

    /* ── 13. 保存目录不存在（多级）→ 引擎必须自己建出来 ──
     * 真实场景：用户删掉了「下载/视频」这类归档子目录，或从状态文件恢复的任务指向
     * 一个已被删掉的目录。目录缺失时 utf8_fopen(tmp_path) 直接失败，用户只看到
     * 「任务失败」，完全看不出是路径问题。
     * GUI 的 task_controller 会 mkpath，但引擎不能依赖调用方 —— 恢复路径、CLI、
     * 以及把 download_core 当库用的调用方全都绕过它。所以引擎自己在
     * start_task_threads 里兜底。
     * 这里特意用**两级**目录：CreateDirectoryW 一次只能建一级，父目录不存在就失败，
     * 只写单级目录的实现在本项会红。 */
    printf("\n[13] 保存目录不存在（多级）→ 必须自动创建并下载成功\n");
    {
        char d1[MAX_PATH + 64], d2[MAX_PATH + 64], got_path[MAX_PATH + 128], d[260];
        char small2[512];
        snprintf(small2, sizeof(small2), "http://127.0.0.1:%d/small.bin", port);
        snprintf(d1, sizeof(d1), "%s\\dirfix_missing", outdir);
        snprintf(d2, sizeof(d2), "%s\\sub", d1);
        snprintf(got_path, sizeof(got_path), "%s\\dirfix.bin", d2);

        /* 清掉上次残留，保证目标目录**真的**不存在（否则本项什么都没验证） */
        utf8_delete(got_path);
        RemoveDirectoryA(d2);
        RemoveDirectoryA(d1);
        snprintf(d, sizeof(d), "目标目录 = %s", d2);
        check("目标多级目录预先确实不存在",
              GetFileAttributesA(d2) == INVALID_FILE_ATTRIBUTES, d);

        int id = dlmgr_add(small2, d2, "dirfix.bin", 2, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            snprintf(d, sizeof(d), "status=%d downloaded=%lld", st, (long long)dl);
            check("保存目录不存在时下载仍然成功（status=3）", st == 3, d);

            long fsz = file_size_of(got_path);
            snprintf(d, sizeof(d), "落盘 %s = %ld 字节", got_path, fsz);
            check("文件确实落在被自动创建的多级目录里", fsz > 0, d);

            dlmgr_remove(id);
            utf8_delete(got_path);
        }
    }

    /* ── 14. 文件名含 Windows 非法字符 → 必须清理后下载成功 ──
     * 三个真实来源：URL 路径里带冒号（老站点很常见）、服务器 Content-Disposition
     * 给出 "re:port?.pdf"、用户手输带 '*' 的名字。CreateFileW 碰到这些字符直接失败，
     * 用户只看到「任务失败」，根本看不出是文件名的问题。
     * 断言不绑定具体替换规则，只要求两件事：下载成功 + 最终文件名不含非法字符。 */
    printf("\n[14] 文件名含 Windows 非法字符（: ? * | < > \"）→ 清理后仍成功\n");
    {
        char d[400], small3[512];
        snprintf(small3, sizeof(small3), "http://127.0.0.1:%d/small.bin", port);

        int id = dlmgr_add(small3, outdir, "bad:na*me?.bin", 2, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);

            TaskInfo ti;
            memset(&ti, 0, sizeof(ti));
            dlmgr_get_task_info(id, &ti);

            int bad = 0;
            for (const char *p = ti.filename; *p; p++)
                if (strchr("<>:\"/\\|?*", *p) || (unsigned char)*p < 0x20) { bad = 1; break; }

            snprintf(d, sizeof(d), "status=%d 最终文件名=\"%s\"", st, ti.filename);
            check("含非法字符的文件名被清理且下载成功",
                  st == 3 && ti.filename[0] != '\0' && !bad, d);

            char fpath[MAX_PATH + 320];
            snprintf(fpath, sizeof(fpath), "%s\\%s", outdir, ti.filename);
            long fsz = file_size_of(fpath);
            snprintf(d, sizeof(d), "%s = %ld 字节", fpath, fsz);
            check("清理后的文件确实落盘（不是只改了个名字）", fsz > 0, d);

            dlmgr_remove(id);
            utf8_delete(fpath);
        }
    }

    /* ── [15] dl_infer_filename 纯函数单元测试（不需要网络）──
     * 这里钉的是两个曾经真实存在的行为：
     *   ① 带查询串的 URL 以前会把 "?x=1" 一起当文件名（长度按 URL 起点算，
     *      却从路径最后一段起拷）；
     *   ② 任何非法字符以前原样落盘。 */
    printf("\n[15] dl_infer_filename 纯函数：查询串/%%XX/保留设备名\n");
    {
        struct { const char *url; const char *want; } cases[] = {
            { "http://h/a/file.zip",            "file.zip" },
            { "http://h/a/file.zip?token=1&x=2","file.zip" },   /* ← ①：不得含 ? */
            { "http://h/a/file.zip#frag",       "file.zip" },   /* ← fragment */
            { "http://h/My%20Big%20File.zip",   "My Big File.zip" },
            { "http://h/a/",                    "download.bin" },
            { "http://h/dl?name=x",             "dl" },
            { "http://h/a/t:es*t|.bin",         "t_es_t_.bin" },
            { "http://h/a/CON.txt",             "_CON.txt" },   /* ← 保留设备名 */
            { "http://h/a/尾部空格 .bin ",      "尾部空格 .bin" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char got[300];
            dl_infer_filename(cases[i].url, got, (int)sizeof(got));
            char d[600];
            snprintf(d, sizeof(d), "url=\"%s\" → \"%s\"（期望 \"%s\"）",
                     cases[i].url, got, cases[i].want);
            check("推断名正确", strcmp(got, cases[i].want) == 0, d);
        }
    }

    /* ── [16] 同名文件策略：默认自动重命名，绝不静默删除 ──
     * 旧实现是无条件 utf8_delete(final_path) 再 move：用户磁盘上已有的同名文件
     * 会被直接抹掉，界面上毫无提示。这里用一个「内容可辨认」的占位文件来验：
     * 下载同名文件后，原来那个文件必须还在、内容一字不变。 */
    printf("\n[16] 同名文件：默认自动重命名（不覆盖、不删除）\n");
    {
        char url2[512];
        snprintf(url2, sizeof(url2), "http://127.0.0.1:%d/small.bin", port);

        /* 造一个「用户已有文件」：固定内容 */
        char exist_path[MAX_PATH + 320];
        snprintf(exist_path, sizeof(exist_path), "%s\\same.bin", outdir);
        const char *kKeep = "USER-ORIGINAL-DATA-KEEP-ME";   /* 26 字节 */
        {
            long w = utf8_write_file(exist_path, kKeep);
            char d[600];
            snprintf(d, sizeof(d), "%s = %ld 字节", exist_path, w);
            check("占位文件写入成功", w == (long)strlen(kKeep), d);
        }

        int id = dlmgr_add(url2, outdir, "same.bin", 2, NULL, NULL, NULL, NULL);
        check("同名任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            TaskInfo ti; memset(&ti, 0, sizeof(ti));
            dlmgr_get_task_info(id, &ti);

            char d[600];
            snprintf(d, sizeof(d), "status=%d 落盘名=\"%s\"", st, ti.filename);
            check("同名下载仍然成功", st == 3, d);

            snprintf(d, sizeof(d), "\"%s\"（期望含 \"(1)\"）", ti.filename);
            check("落盘名被自动改成「名字 (1).ext」", strstr(ti.filename, "(1)") != NULL, d);

            /* 命门：原文件必须在，且内容一字未改 */
            long keep_sz = file_size_of(exist_path);
            snprintf(d, sizeof(d), "\"%s\" = %ld 字节（期望 %zu）",
                     exist_path, keep_sz, strlen(kKeep));
            check("原来那个同名文件仍然存在", keep_sz == (long)strlen(kKeep), d);

            char buf[64]; memset(buf, 0, sizeof(buf));
            long rd = utf8_read_file(exist_path, buf, sizeof(buf) - 1);
            snprintf(d, sizeof(d), "读回 \"%s\"（%ld 字节）", buf, rd);
            check("原文件内容一字未改（没有被覆盖/截断）",
                  rd == (long)strlen(kKeep) && strcmp(buf, kKeep) == 0, d);

            /* 新文件确实落在 (1) 上 */
            char new_path[MAX_PATH + 320];
            snprintf(new_path, sizeof(new_path), "%s\\%s", outdir, ti.filename);
            snprintf(d, sizeof(d), "%s = %ld 字节", new_path, file_size_of(new_path));
            check("新文件落在改名后的路径上", file_size_of(new_path) > 0, d);

            dlmgr_remove(id);
            utf8_delete(new_path);
        }

        /* 反向：显式开启 overwrite_existing 时才允许覆盖 */
        {
            DownloadConfig c = dlmgr_get_config();
            c.overwrite_existing = 1;
            dlmgr_set_config(&c);
            int id = dlmgr_add(url2, outdir, "same.bin", 2, NULL, NULL, NULL, NULL);
            if (id >= 0) {
                dlmgr_start(id);
                int st = 0; int64_t dl = 0;
                wait_task(id, 60000, &st, &dl);
                TaskInfo ti; memset(&ti, 0, sizeof(ti));
                dlmgr_get_task_info(id, &ti);
                char d[600];
                snprintf(d, sizeof(d), "status=%d 落盘名=\"%s\"", st, ti.filename);
                check("勾选「覆盖同名文件」后按原名覆盖",
                      st == 3 && strcmp(ti.filename, "same.bin") == 0, d);
                dlmgr_remove(id);
            }
            c.overwrite_existing = 0;      /* 还原，别影响后续用例 */
            dlmgr_set_config(&c);
        }
        utf8_delete(exist_path);
    }

    /* ── [17] 打开失败必须说人话（以前只有一句「无法打开文件: <路径>」）──
     * 两个确定性场景：
     *   ① 保存目录其实是个**普通文件** → 路径不成立；
     *   ② 临时文件被设成**只读** → 没有写入权限。
     * 断言点不是「任务失败」（那本来就该失败），而是**错误文案里有没有原因**。 */
    printf("\n[17] 打开失败的错误文案必须带原因\n");
    {
        char u[512];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/small.bin", port);

        /* ① 保存目录是一个普通文件 */
        char fake_dir[MAX_PATH + 320];
        snprintf(fake_dir, sizeof(fake_dir), "%s\\not_a_dir", outdir);
        utf8_write_file(fake_dir, "x");           /* 建一个同名普通文件 */
        int id = dlmgr_add(u, fake_dir, "a.bin", 2, NULL, NULL, NULL, NULL);
        check("路径不成立时任务仍能创建（错误在启动后暴露）", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            TaskInfo ti; memset(&ti, 0, sizeof(ti));
            dlmgr_get_task_info(id, &ti);
            char d[600];
            snprintf(d, sizeof(d), "status=%d msg=\"%s\"", st, ti.error_msg);
            check("保存目录是文件 → 任务失败", st == 4, d);
            check("文案带「打开临时文件失败」前缀（不是光一个路径）",
                  strstr(ti.error_msg, "打开临时文件失败") != NULL, d);
            check("文案给出了具体原因（没落到「未知原因」）",
                  strstr(ti.error_msg, "未知原因") == NULL
                  && strstr(ti.error_msg, "失败：") != NULL, d);
            check("不再是旧的无信息量文案", strncmp(ti.error_msg, "无法打开文件: ", 13) != 0, d);
            dlmgr_remove(id);
        }
        utf8_delete(fake_dir);

        /* ② 临时文件只读 */
        char ro_tmp[MAX_PATH + 320];
        snprintf(ro_tmp, sizeof(ro_tmp), "%s\\rofile.bin.idmtmp", outdir);
        utf8_write_file(ro_tmp, "seed");
        {
            wchar_t w[1024];
            int attrSet = 0;
            if (MultiByteToWideChar(CP_UTF8, 0, ro_tmp, -1, w, 1024) > 0)
                attrSet = SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY) ? 1 : 0;
            check("临时文件已设为只读", attrSet == 1, ro_tmp);
        }
        id = dlmgr_add(u, outdir, "rofile.bin", 2, NULL, NULL, NULL, NULL);
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);
            TaskInfo ti; memset(&ti, 0, sizeof(ti));
            dlmgr_get_task_info(id, &ti);
            char d[600];
            snprintf(d, sizeof(d), "status=%d msg=\"%s\"", st, ti.error_msg);
            check("只读临时文件 → 任务失败", st == 4, d);
            check("文案指出是权限/占用问题",
                  strstr(ti.error_msg, "权限") != NULL
                  || strstr(ti.error_msg, "占用") != NULL
                  || strstr(ti.error_msg, "只读") != NULL, d);
            dlmgr_remove(id);
        }
        {   /* 清只读属性再删，否则删不掉 */
            wchar_t w[1024];
            if (MultiByteToWideChar(CP_UTF8, 0, ro_tmp, -1, w, 1024) > 0)
                SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
        }
        utf8_delete(ro_tmp);
    }

    /* ── [18] 写盘失败不得伪装成「网络错误」/「HTTP 错误: 206」──
     * 直接把一个**只读句柄**交给分片下载函数（这是公开 API 允许的用法），
     * fwrite 必然短写 → libcurl 回 CURLE_WRITE_ERROR。修好之前这里会返回
     * success=1（只看到 code=206 与少量已写字节），或者报英文
     * "网络错误: Failed writing received data to disk/application"。 */
    printf("\n[18] 写盘失败 → 必须是「写入磁盘失败：<中文原因>」\n");
    {
        char u[512];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/small.bin", port);

        char seed[MAX_PATH + 320];
        snprintf(seed, sizeof(seed), "%s\\ro_handle.bin", outdir);
        utf8_write_file(seed, "seed");

        FILE *ro = utf8_fopen_rb(seed);      /* 只读打开 */
        check("只读句柄打开成功", ro != NULL, seed);
        if (ro) {
            NetOptions opt = network_default_options();
            /* 本地自测服务器对 /small.bin 要求 Basic 认证；不带上会先拿 401，
             * 根本走不到写盘那一步。 */
            opt.auth_user = user;
            opt.auth_pass = pass;
            NetDownloadTask nd; memset(&nd, 0, sizeof(nd));
            nd.url = u; nd.save_path = seed;
            nd.range_start = 0; nd.range_end = -1;
            nd.opt = &opt;

            NetDownloadResult r = network_download_range_fp(&nd, ro);
            fclose(ro);

            char d[600];
            snprintf(d, sizeof(d), "success=%d no_retry=%d http=%ld msg=\"%s\"",
                     r.success, r.no_retry, r.http_code, r.error_msg);
            check("写盘失败必须判为失败", r.success == 0, d);
            check("标记为不可重试（磁盘/权限问题重试没意义）", r.no_retry == 1, d);
            check("文案是「写入磁盘失败：<原因>」", strstr(r.error_msg, "写入磁盘失败") != NULL, d);
            check("不再暴露英文的 Failed writing / CURLE 文案",
                  strstr(r.error_msg, "Failed writing") == NULL
                  && strstr(r.error_msg, "网络错误") == NULL, d);
        }
        utf8_delete(seed);
    }

    /* ── [19] 超长保存路径（>260 字符）→ 必须靠 \\?\ 前缀才能落盘 ──
     * Windows 默认 260 字符上限：保存目录很深、或单目录名很长时，完整路径会超出，
     * CreateFileW / MoveFileW / CreateDirectoryW 直接失败（ERROR_FILENAME_EXCED_RANGE）。
     * 这里造一个「单分量 ≤255（合法）、但总路径 >260」的保存目录（一个 240 字符的
     * 目录名分量），下载到它里面，断言任务完成、文件逐字节正确。
     * ⚠️ 不能只用一个 >255 的巨文件名去测：那本来就非法，加了 \\?\ 也救不回来，
     *      那样测的是「引擎有没有为非法名报错」而不是「\\?\ 有没有生效」，会偏离本意。
     * ⚠️ 测试自己的读/删也必须走 \\?\（test_to_wpath 已加），否则会假失败。 */
    printf("\n[19] 超长保存路径（>260 字符）→ 必须落盘成功\n");
    {
        char longdir[MAX_PATH + 512], u[512];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/small.bin", port);

        /* 单分量 240 字符（<255，合法），总路径因此 >260（TEMP 下约 290+ 字符） */
        char comp[256];
        memset(comp, 'A', 240); comp[240] = '\0';
        snprintf(longdir, sizeof(longdir), "%s\\%s", outdir, comp);

        /* 先确保它真的不存在（正常路径 API 读不了 >260，用前缀删除兜底） */
        {
            wchar_t w[1024];
            if (test_to_wpath(longdir, w, 1024)) {
                DeleteFileW(w); RemoveDirectoryW(w);
            }
        }

        /* 配置站点登录（small.bin 需 Basic 认证） */
        {
            DownloadConfig cfg = dlmgr_get_config();
            memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
            cfg.site_login_count = 1;
            strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
            strncpy(cfg.site_logins[0].user,  user,      sizeof(cfg.site_logins[0].user)  - 1);
            strncpy(cfg.site_logins[0].pass,  pass,      sizeof(cfg.site_logins[0].pass)  - 1);
            dlmgr_set_config(&cfg);
        }

        int id = dlmgr_add(u, longdir, "lp.bin", 2, NULL, NULL, NULL, NULL);
        check("长路径任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_task(id, 60000, &st, &dl);

            char path[MAX_PATH + 512], tmp2[MAX_PATH + 512];
            snprintf(path, sizeof(path), "%s\\lp.bin", longdir);
            snprintf(tmp2, sizeof(tmp2), "%s.idmtmp", path);
            long fsz = file_size_of(path);
            char d[300];
            snprintf(d, sizeof(d), "status=%d file=%ld bytes", st, fsz);
            check("超长路径下下载仍然完成（status=3）", st == 3, d);
            check("文件确实落在超长路径目录里（非空）", fsz > 0, d);

            int mc = content_matches_pattern(path, 1024 * 1024);
            snprintf(d, sizeof(d), "file_size=%lld 比对=%d（1=逐字节一致）",
                     file_size_i64_of(path), mc);
            check("超长路径落盘内容逐字节正确", mc == 1, d);

            utf8_delete(path);
            utf8_delete(tmp2);   /* 失败残留的 .idmtmp 也要清掉 */
            {
                wchar_t w[1024];
                if (test_to_wpath(longdir, w, 1024)) RemoveDirectoryW(w);
            }
            dlmgr_remove(id);
        }
    }

    /* ── 20. parse_retry_after 纯函数（整数秒 / HTTP-date / 非法）──
     * 这是「尊重 Retry-After」的基础：解析本身必须正确，否则后面的重试等待算错。
     * 纯函数、无网络、确定性 —— 单测比端到端更稳，突变（注掉整数分支→回退）必红。 */
    printf("\n[20] parse_retry_after 纯函数：整数秒 / HTTP-date / 非法\n");
    {
        char d[96];
        long r;

        r = parse_retry_after("120", 1000);
        snprintf(d, sizeof(d), "120 -> %ldms（期望 120000）", r);
        check("整数秒 \"120\" -> 120000ms", r == 120000, d);

        r = parse_retry_after(" 30 ", 1000);   /* 前后带空白，必须容忍 */
        snprintf(d, sizeof(d), "\" 30 \" -> %ldms（期望 30000）", r);
        check("整数秒带空格 \" 30 \" -> 30000ms", r == 30000, d);

        r = parse_retry_after("0", 1000);
        snprintf(d, sizeof(d), "0 -> %ldms（期望 0，立即重试）", r);
        check("整数秒 \"0\" -> 0ms（立即重试）", r == 0, d);

        r = parse_retry_after("abc", 1000);    /* 非法 → 回退固定退避 */
        snprintf(d, sizeof(d), "abc -> %ldms（期望回退 1000）", r);
        check("非法字符串 -> 回退 fallback(1000ms)", r == 1000, d);

        r = parse_retry_after(NULL, 1000);
        check("NULL -> 回退 fallback(1000ms)", r == 1000, "NULL 必须回退");

        /* HTTP-date：构造一个「未来 120 秒」的 RFC 1123 时间串，断言落在 [110000,130000] */
        {
            time_t future = time(NULL) + 120;
            char ds[64];
            struct tm *g = gmtime(&future);
            strftime(ds, sizeof(ds), "%a, %d %b %Y %H:%M:%S GMT", g);
            r = parse_retry_after(ds, 1000);
            snprintf(d, sizeof(d), "HTTP-date 未来120s -> %ldms（期望 110000~130000）", r);
            check("HTTP-date 未来120s -> 约120000ms", r >= 110000 && r <= 130000, d);
        }
        /* HTTP-date 在过去 → 0（立即重试，而不是退回固定退避） */
        {
            time_t past = time(NULL) - 120;
            char ds[64];
            struct tm *g = gmtime(&past);
            strftime(ds, sizeof(ds), "%a, %d %b %Y %H:%M:%S GMT", g);
            r = parse_retry_after(ds, 1000);
            snprintf(d, sizeof(d), "HTTP-date 过去 -> %ldms（期望 0）", r);
            check("HTTP-date 在过去 -> 0ms（立即重试）", r == 0, d);
        }
    }

    /* ── 21. 重试尊重 Retry-After（端到端）──
     * 服务端 /retryafter.bin：首 GET 回 429 + Retry-After: 3，后续 GET 回 200。
     * 引擎若尊重 Retry-After，重试等待≈3s；若只按固定 1s 退避猛撞，等待≈1s。
     * 用实际耗时区分两者 —— 这是「尊重 Retry-After」唯一可观测的证据，
     * 也是突变测试（注掉读头逻辑→退回固定 1s）会变红的断言。 */
    printf("\n[21] 重试尊重 Retry-After（429 + Retry-After:3 → 实际等待≈3s 而非固定 1s）\n");
    {
        char u[512];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/retryafter.bin", port);
        /* retryafter.bin 需 Basic 认证 */
        {
            DownloadConfig cfg = dlmgr_get_config();
            memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
            cfg.site_login_count = 1;
            strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
            strncpy(cfg.site_logins[0].user,  user,      sizeof(cfg.site_logins[0].user)  - 1);
            strncpy(cfg.site_logins[0].pass,  pass,      sizeof(cfg.site_logins[0].pass)  - 1);
            dlmgr_set_config(&cfg);
        }
        int id = dlmgr_add(u, outdir, "ra.bin", 1, NULL, NULL, NULL, NULL);
        check("任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            DWORD t0 = GetTickCount();
            int st = 0; int64_t dl = 0;
            wait_task(id, 30000, &st, &dl);
            DWORD el = GetTickCount() - t0;

            char path[MAX_PATH + 64], tmp2[MAX_PATH + 64];
            snprintf(path, sizeof(path), "%s\\ra.bin", outdir);
            snprintf(tmp2, sizeof(tmp2), "%s.idmtmp", path);
            long fsz = file_size_of(path);
            int mc = content_matches_pattern(path, 4096);
            char d[220];
            snprintf(d, sizeof(d),
                     "status=%d file=%ld 内容=%d 耗时=%ums（期望≥2000，证明等到3s而非固定1s）",
                     st, fsz, mc, el);
            check("尊重 Retry-After：重试后下载成功", st == 3 && fsz > 0 && mc == 1, d);
            /* 实际等待明显长于固定退避（1s）→ 证明读到了 Retry-After: 3 并按它退避 */
            check("尊重 Retry-After：实际等待≈3s（非固定1s退避）", el >= 2000, d);
            utf8_delete(path);
            utf8_delete(tmp2);
            dlmgr_remove(id);
        }
    }

    /* ── 22. 服务器提前断流：截断的文件绝不能被当成「已完成」──
     * 服务端 /cut.bin：HEAD 不给 Content-Length（探测得到 file_size=-1，任务只能走
     * 「长度未知、一次请求流式取完整个响应体」的路径），GET 声明 200000 字节却只发
     * 8000 就关闭连接。
     * 为什么单独测这条路径：已知总长度时 while (c->downloaded < total_in_chunk) 天然会
     * 兜住（差多少补多少），而长度未知**没有任何字节数可对账** —— 旧判据只看
     * 「有 200/206 + 收到过字节」，于是断流照样 success=1 → chunk done → 用户界面上
     * 是「已完成」、打开却是个半截文件（与既往「大小对、内容坏」同族，这次连大小都短）。
     * 断言分两层：① 网络层如实上报 truncated（上层无从判断的前提是下层不撒谎）；
     * ② 引擎保留已收字节、带 Range 从断点续传补全 —— 最终 200000 字节且逐字节正确，
     *   且服务端确实收到过续传用的 Range 请求。
     * ③ 反面：/cutnr.bin 断流且不认 Range → 续不上就必须诚实失败，不许产出成品文件。
     * 突变测试：注掉 download_core.c 里 truncated 分支 → 第一轮就 done=1，
     * 文件停在 8000 字节而状态仍是「已完成」，②③ 两条断言同时变红。 */
    printf("\n[22] 服务器提前断流 → 必须续传补全，不许把半截文件判成已完成\n");
    {
        char cu[512], cpath[MAX_PATH + 64];
        snprintf(cu, sizeof(cu), "http://127.0.0.1:%d/cut.bin", port);
        snprintf(cpath, sizeof(cpath), "%s\\cut.bin", outdir);
        utf8_delete(cpath);
        server_reset(port);

        NetOptions opt = network_default_options();
        char au[128], ap[128];
        strncpy(au, user, sizeof(au) - 1); au[sizeof(au) - 1] = '\0';
        strncpy(ap, pass, sizeof(ap) - 1); ap[sizeof(ap) - 1] = '\0';
        opt.auth_user = au;
        opt.auth_pass = ap;

        /* ① 网络层：断流的响应必须带上 truncated 标志 */
        NetDownloadTask nd; memset(&nd, 0, sizeof(nd));
        nd.url         = cu;
        nd.save_path   = cpath;
        nd.range_start = 0;
        nd.range_end   = -1;      /* 长度未知的单流取法：不发 Range 头 */
        nd.opt         = &opt;
        NetDownloadResult r = network_download_range(&nd);
        char d[260];
        snprintf(d, sizeof(d), "success=%d http=%ld written=%lld truncated=%d err=%s",
                 r.success, r.http_code, (long long)r.bytes_written, r.truncated,
                 r.error_msg);
        check("断流响应：收到字节但被标记为 truncated",
              r.success == 1 && r.http_code == 200 &&
              r.bytes_written == 8000 && r.truncated == 1, d);
        long long part = file_size_i64_of(cpath);
        snprintf(d, sizeof(d), "落盘=%lld 期望=8000", part);
        check("断流响应：已收字节确实落盘（供断点续传）", part == 8000, d);
        utf8_delete(cpath);

        /* ② 引擎：长度未知的任务必须续传补全 */
        {
            DownloadConfig cfg = dlmgr_get_config();
            memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
            cfg.site_login_count = 1;
            strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
            strncpy(cfg.site_logins[0].user,  user,      sizeof(cfg.site_logins[0].user)  - 1);
            strncpy(cfg.site_logins[0].pass,  pass,      sizeof(cfg.site_logins[0].pass)  - 1);
            dlmgr_set_config(&cfg);
        }
        char e2[512], p2[MAX_PATH + 64], p2t[MAX_PATH + 80];
        snprintf(e2, sizeof(e2), "http://127.0.0.1:%d/cut.bin", port);
        snprintf(p2, sizeof(p2), "%s\\cut2.bin", outdir);
        snprintf(p2t, sizeof(p2t), "%s.idmtmp", p2);
        utf8_delete(p2); utf8_delete(p2t);
        int id = dlmgr_add(e2, outdir, "cut2.bin", 1, NULL, NULL, NULL, NULL);
        check("断流续传任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_final(id, 25000, &st, &dl);
            long long fsz = file_size_i64_of(p2);
            int mc = content_matches_pattern(p2, 200000);
            int nrange = server_stat(port, "cut_range");
            snprintf(d, sizeof(d),
                     "status=%d downloaded=%lld file=%lld 内容=%d 续传Range数=%d",
                     st, (long long)dl, fsz, mc, nrange);
            check("断流后补齐到完整长度（不是 8000 字节的假完成）",
                  st == 3 && fsz == 200000 && mc == 1, d);
            check("断流后确实带 Range 续传过（而不是整份重下）", nrange >= 1, d);
            utf8_delete(p2); utf8_delete(p2t);
            dlmgr_remove(id);
        }

        /* ③ 反面：断流 + 不认 Range → 只能诚实失败，不许产出成品文件 */
        char e3[512], p3[MAX_PATH + 64], p3t[MAX_PATH + 80];
        snprintf(e3, sizeof(e3), "http://127.0.0.1:%d/cutnr.bin", port);
        snprintf(p3, sizeof(p3), "%s\\cutnr.bin", outdir);
        snprintf(p3t, sizeof(p3t), "%s.idmtmp", p3);
        utf8_delete(p3); utf8_delete(p3t);
        id = dlmgr_add(e3, outdir, "cutnr.bin", 1, NULL, NULL, NULL, NULL);
        check("断流不可续传任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int st = 0; int64_t dl = 0;
            wait_final(id, 30000, &st, &dl);
            char finfo[200];
            snprintf(finfo, sizeof(finfo), "成品文件是否存在=%d（.idmtmp 允许残留以便重试）",
                     file_size_i64_of(p3) >= 0 ? 1 : 0);
            snprintf(d, sizeof(d), "status=%d downloaded=%lld", st, (long long)dl);
            check("断流且续不上时判为失败（status=4）", st == 4, d);
            check("失败任务没有把截断数据改名成成品文件",
                  file_size_i64_of(p3) < 0, finfo);
            utf8_delete(p3); utf8_delete(p3t);
            dlmgr_remove(id);
        }
    }

    /* ── 23. HEAD 探测重试：瞬时 429 后仍要拿到文件大小 ──
     * 服务端 /probe429.bin：首 HEAD 回 429 + Retry-After: 2，之后回正常的 200 + 大小。
     * 探测原先一次都没有重试：一次限流就把 file_size 打成 -1 → 不分段 → 整个下载
     * 退化成单连接，用户只看到「能下，但只有一条连接」，且完全不知道原因。
     * 断言：① 探测最终成功且大小/Range 正确；② 服务端确实被 HEAD 过两次以上
     * （证明是我们补发的，而不是服务端一次就放行）；③ 等待按 Retry-After: 2 走
     * 而不是固定 1s 退避；④⑤ 反面哨兵：确定性失败（401）与连接层失败（连不上）
     * 必须**立刻**返回——dlmgr_start 是在 GUI 线程里同步探测的，对白等重试来说就是卡窗口。
     * 突变测试：探测退回「只发一次」→ ①②③ 红；把连不上/4xx 也算作可重试 → ④⑤ 红。 */
    printf("\n[23] HEAD 探测重试：瞬时 429 + Retry-After 后仍拿到大小（401 不白等）\n");
    {
        char pu[512];
        snprintf(pu, sizeof(pu), "http://127.0.0.1:%d/probe429.bin", port);
        NetOptions opt = network_default_options();
        char au[128], ap[128];
        strncpy(au, user, sizeof(au) - 1); au[sizeof(au) - 1] = '\0';
        strncpy(ap, pass, sizeof(ap) - 1); ap[sizeof(ap) - 1] = '\0';
        opt.auth_user = au;
        opt.auth_pass = ap;

        server_reset(port);
        DWORD t0 = GetTickCount();
        NetworkProbe pr = network_probe(pu, &opt);
        DWORD el = GetTickCount() - t0;
        int heads = server_stat(port, "probe429_heads");

        char d[260];
        snprintf(d, sizeof(d),
                 "success=%d http=%ld size=%lld range=%d HEAD次数=%d 耗时=%ums",
                 pr.success, pr.http_code, (long long)pr.file_size,
                 pr.supports_range, heads, el);
        check("探测在 429 之后重试成功并拿到文件大小",
              pr.success == 1 && pr.http_code == 200 &&
              pr.file_size == 4096 && pr.supports_range == 1, d);
        check("探测确实补发了 HEAD（重试真的发生过）", heads >= 2, d);
        check("探测重试尊重 Retry-After: 2（而非固定 1s 退避）", el >= 1700, d);

        /* ④ 哨兵：确定性失败不该进重试 */
        NetOptions bad = network_default_options();
        char nu[32], np[32];
        strncpy(nu, "nobody", sizeof(nu) - 1); nu[sizeof(nu) - 1] = '\0';
        strncpy(np, "nopass", sizeof(np) - 1); np[sizeof(np) - 1] = '\0';
        bad.auth_user = nu;
        bad.auth_pass = np;
        DWORD t1 = GetTickCount();
        NetworkProbe pr401 = network_probe(pu, &bad);
        DWORD el401 = GetTickCount() - t1;
        snprintf(d, sizeof(d), "success=%d http=%ld 耗时=%ums（期望 401 且 <1500ms）",
                 pr401.success, pr401.http_code, el401);
        check("401 这类确定性失败立即返回，不占用探测重试",
              pr401.success == 0 && pr401.http_code == 401 && el401 < 1500, d);

        /* ⑤ 连接层失败也必须「立刻放弃探测」：探测跑在 GUI 线程上，
         * 对连不上的主机反复重试 = 反复冻住窗口，而且多问一次也问不出信息。 */
        {
            char du[512];
            snprintf(du, sizeof(du), "http://127.0.0.1:%d/dead.bin", port + 1);  /* 无人监听 */
            DWORD t2 = GetTickCount();
            NetworkProbe prDead = network_probe(du, &opt);
            DWORD elDead = GetTickCount() - t2;
            snprintf(d, sizeof(d), "success=%d http=%ld 耗时=%ums（期望立刻失败且 <800ms）",
                     prDead.success, prDead.http_code, elDead);
            check("连不上的主机不重试探测（不拖住 GUI 线程）",
                  prDead.success == 0 && elDead < 800, d);
        }
    }

    /* ── 24. 限流等待可见：429 + Retry-After 期间要能读到倒计时 ──
     * 服务端 /retryafter.bin 首 GET 回 429 + Retry-After: 3。那 3 秒里下载线程正在
     * Sleep，界面上原先只有三个纹丝不动的字「下载中」——用户分不清是慢、是卡、还是死。
     * 现在网络层在睡之前把「等到什么时候」报给引擎，读侧按引擎时钟换算剩余秒数。
     * 断言：① 等待期间读到 sec ≥ 2；② 知道是 429；③ 暂停能**立刻**摘掉倒计时
     * （此刻截止时刻还在约 2 秒之后，靠「到点自动失效」根本来不及）；④ 窗口过后
     * 与全程都不残留。
     * 突变测试：去掉 range_write 里的 notice_wait 调用 → ①② 红；
     * 去掉 dlmgr_pause 里的清零 → ③ 红。
     * 界面侧（状态列真的换成那句中文、撤掉后回落）见 ui_snapshot 的 IDM_NOTICE_PROBE。 */
    printf("\n[24] 限流等待可见：429 期间读到倒计时，暂停立刻摘掉、过后不残留\n");
    {
        char u24[512], p24[MAX_PATH + 64], p24t[MAX_PATH + 80];
        snprintf(u24, sizeof(u24), "http://127.0.0.1:%d/retryafter.bin", port);
        snprintf(p24, sizeof(p24), "%s\\ra.bin", outdir);
        snprintf(p24t, sizeof(p24t), "%s.idmtmp", p24);
        utf8_delete(p24); utf8_delete(p24t);
        /* 把服务端的「首次 GET」状态还回来（[21] 已经消费掉一次 429） */
        server_reset(port);
        {
            DownloadConfig cfg = dlmgr_get_config();
            memset(cfg.site_logins, 0, sizeof(cfg.site_logins));
            cfg.site_login_count = 1;
            strncpy(cfg.site_logins[0].match, "127.0.0.1", sizeof(cfg.site_logins[0].match) - 1);
            strncpy(cfg.site_logins[0].user,  user,      sizeof(cfg.site_logins[0].user)  - 1);
            strncpy(cfg.site_logins[0].pass,  pass,      sizeof(cfg.site_logins[0].pass)  - 1);
            dlmgr_set_config(&cfg);
        }
        int id = dlmgr_add(u24, outdir, "ra.bin", 1, NULL, NULL, NULL, NULL);
        check("限流可见任务创建成功", id >= 0, "");
        if (id >= 0) {
            dlmgr_start(id);
            int max_sec = 0, http_seen = 0;
            TaskInfo info;
            for (int i = 0; i < 80; i++) {          /* 最多观察 8s，覆盖整段 3s 等待 */
                if (dlmgr_get_task_info(id, &info) != 0) break;
                if (info.throttle_sec > max_sec) {
                    max_sec   = info.throttle_sec;
                    http_seen = info.throttle_http;
                }
                if (info.status == 3 || info.status == 4) break;   /* 提前收尾也正常 */
                if (max_sec >= 2) break;            /* 观察到就够了，别等它自然过期 */
                Sleep(100);
            }
            char d24[240];
            snprintf(d24, sizeof(d24), "观察到的最大剩余秒数=%d http=%d", max_sec, http_seen);
            check("429 等待期间能读到剩余秒数（Retry-After: 3 → 观察到 ≥2）",
                  max_sec >= 2, d24);
            snprintf(d24, sizeof(d24), "触发等待的状态码=%d（期望 429）", http_seen);
            check("倒计时知道自己是为什么在等（429）", http_seen == 429, d24);

            /* 用户一按暂停，倒计时必须**立刻**消失：此刻截止时刻还在约 2 秒之后，
             * 不显式清零的话界面上就是「已暂停 · 3 秒后重试」这种自相矛盾的话。
             * 这条也是「引擎侧真能清掉这个状态」的活证据——成功分支的清零在
             * Retry-After 很短时会被「到点自动失效」掩盖掉，测不出差别。 */
            if (max_sec >= 2) {
                dlmgr_pause(id);
                TaskInfo paused;
                int psec = -1;
                if (dlmgr_get_task_info(id, &paused) == 0) psec = paused.throttle_sec;
                snprintf(d24, sizeof(d24), "暂停后剩余秒数=%d（期望 0）status=%d",
                         psec, paused.status);
                check("暂停立刻摘掉倒计时", psec == 0, d24);

                /* 等原本的 Retry-After 窗口过去：倒计时必须是 0，且不能因为
                 * 「到点」被重新点亮。这里刻意**不**调 dlmgr_start 续下——
                 * 等待期间起第二组线程会与第一组的收尾动作抢同一份落盘，
                 * 那是另一个（既有的、与本次改动无关的）问题，见报告。 */
                Sleep(3500);
                TaskInfo later;
                int lsec = -1, lst = -1;
                if (dlmgr_get_task_info(id, &later) == 0) {
                    lsec = later.throttle_sec;
                    lst  = later.status;
                }
                snprintf(d24, sizeof(d24), "窗口过后剩余秒数=%d（期望 0）status=%d", lsec, lst);
                check("等待窗口过后倒计时保持为 0", lsec == 0, d24);
            }
            int64_t dl = 0;
            TaskInfo fin;
            if (dlmgr_get_task_info(id, &fin) == 0) {
                dl = fin.downloaded;
                snprintf(d24, sizeof(d24), "status=%d downloaded=%lld 剩余秒数=%d err=「%s」",
                         fin.status, (long long)dl, fin.throttle_sec, fin.error_msg);
                check("倒计时全程不再残留", fin.throttle_sec == 0, d24);
            } else {
                check("倒计时全程不再残留", 0, "任务已查不到");
            }
            utf8_delete(p24); utf8_delete(p24t);
            dlmgr_remove(id);
        }
    }

    dlmgr_destroy();
    printf("\n== 结果：%d 通过，%d 失败 ==\n", g_pass, g_fail);
    return g_fail;
}
