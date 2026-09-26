#include "settings.h"
#include "app_paths.h"
#include "download_core.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <cstring>

Settings::Settings()
    : m_defaultSaveDir(defaultDownloadDir())
{
}

QString Settings::defaultDownloadDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/Downloads");
    return dir;
}

void Settings::load()
{
    QSettings s = AppPaths::settings();
    m_defaultSaveDir  = s.value(QStringLiteral("defaultSaveDir"), m_defaultSaveDir).toString();
    m_maxThreads     = s.value(QStringLiteral("maxThreads"), 8).toInt();
    m_maxConcurrent  = s.value(QStringLiteral("maxConcurrent"), 4).toInt();
    m_speedLimitKBps = s.value(QStringLiteral("speedLimitKBps"), 0).toInt();
    m_http2Enabled   = s.value(QStringLiteral("http2Enabled"), true).toBool();
    // 流量档位：优先读显式字段；旧版（无该字段的升级用户）按 speedLimitKBps 反推
    if (s.contains(QStringLiteral("trafficMode")))
        m_trafficMode = s.value(QStringLiteral("trafficMode"), 0).toInt();
    else
        m_trafficMode = (m_speedLimitKBps == 0) ? 0 : -1;  // 0=自动, 非0=自定义
    m_theme          = s.value(QStringLiteral("theme"), QStringLiteral("light")).toString();
    m_groupMode      = s.value(QStringLiteral("groupMode"), 0).toInt();
    m_clipboardMonitor = s.value(QStringLiteral("clipboardMonitor"), true).toBool();
    m_closeToTray    = s.value(QStringLiteral("closeToTray"), false).toBool();
    m_autoDownloadYtDlp = s.value(QStringLiteral("autoDownloadYtDlp"), true).toBool();
    m_autoArchiveByType = s.value(QStringLiteral("autoArchiveByType"), false).toBool();
    m_overwriteExisting = s.value(QStringLiteral("overwriteExisting"), false).toBool();
    m_ytDlpCustomPath   = s.value(QStringLiteral("ytDlpCustomPath"), QString()).toString();
    m_ffmpegCustomPath  = s.value(QStringLiteral("ffmpegCustomPath"), QString()).toString();
    m_aria2CustomPath   = s.value(QStringLiteral("aria2CustomPath"), QString()).toString();
    m_autoDownloadAria2 = s.value(QStringLiteral("autoDownloadAria2"), true).toBool();
    m_autoDownloadFfmpeg = s.value(QStringLiteral("autoDownloadFfmpeg"), true).toBool();
    m_webEnabled        = s.value(QStringLiteral("webEnabled"), false).toBool();
    m_webPort           = s.value(QStringLiteral("webPort"), 8080).toInt();
    m_webToken          = s.value(QStringLiteral("webToken"), QString()).toString();
    if (m_webToken.isEmpty())  // 首次运行：生成随机令牌并随本次 save 持久化
        m_webToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_shutdownAction    = s.value(QStringLiteral("shutdownAction"), QStringLiteral("none")).toString();
    m_shutdownGraceSec  = s.value(QStringLiteral("shutdownGraceSec"), 60).toInt();
    m_connectionsPerServer = s.value(QStringLiteral("connectionsPerServer"), 8).toInt();
    m_retryCount     = s.value(QStringLiteral("retryCount"), 5).toInt();
    m_userAgent      = s.value(QStringLiteral("userAgent"), QStringLiteral("IDM-Next/0.1")).toString();
    m_connectTimeoutSec = s.value(QStringLiteral("connectTimeoutSec"), 30).toInt();

    m_capturedExtensions = s.value(QStringLiteral("capturedExtensions")).toStringList();
    if (m_capturedExtensions.isEmpty()) {
        // 默认一组常见下载扩展名
        m_capturedExtensions << QStringLiteral("zip") << QStringLiteral("rar")
                             << QStringLiteral("7z")  << QStringLiteral("tar")
                             << QStringLiteral("gz")  << QStringLiteral("mp4")
                             << QStringLiteral("mkv") << QStringLiteral("mov")
                             << QStringLiteral("avi") << QStringLiteral("mp3")
                             << QStringLiteral("flac")<< QStringLiteral("wav")
                             << QStringLiteral("exe") << QStringLiteral("msi")
                             << QStringLiteral("dmg") << QStringLiteral("apk")
                             << QStringLiteral("pdf") << QStringLiteral("doc")
                             << QStringLiteral("docx")<< QStringLiteral("xls")
                             << QStringLiteral("xlsx")<< QStringLiteral("iso")
                             << QStringLiteral("img");
    }

    m_proxyType = s.value(QStringLiteral("proxyType"), QStringLiteral("none")).toString();
    m_proxyHost = s.value(QStringLiteral("proxyHost"), QString()).toString();
    m_proxyPort = s.value(QStringLiteral("proxyPort"), 0).toInt();
    m_proxyUser = s.value(QStringLiteral("proxyUser"), QString()).toString();
    m_proxyPass = s.value(QStringLiteral("proxyPass"), QString()).toString();

    int n = s.beginReadArray(QStringLiteral("siteLogins"));
    m_siteLogins.clear();
    m_siteLogins.reserve(n);
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SiteLogin login;
        login.url      = s.value(QStringLiteral("url")).toString();
        login.username = s.value(QStringLiteral("user")).toString();
        login.password = s.value(QStringLiteral("pass")).toString();
        m_siteLogins.append(login);
    }
    s.endArray();
}

void Settings::save() const
{
    QSettings s = AppPaths::settings();
    s.setValue(QStringLiteral("defaultSaveDir"), m_defaultSaveDir);
    s.setValue(QStringLiteral("maxThreads"), m_maxThreads);
    s.setValue(QStringLiteral("maxConcurrent"), m_maxConcurrent);
    s.setValue(QStringLiteral("speedLimitKBps"), m_speedLimitKBps);
    s.setValue(QStringLiteral("http2Enabled"), m_http2Enabled);
    s.setValue(QStringLiteral("trafficMode"), m_trafficMode);
    s.setValue(QStringLiteral("groupMode"), m_groupMode);
    s.setValue(QStringLiteral("theme"), m_theme);
    s.setValue(QStringLiteral("clipboardMonitor"), m_clipboardMonitor);
    s.setValue(QStringLiteral("closeToTray"), m_closeToTray);
    s.setValue(QStringLiteral("autoDownloadYtDlp"), m_autoDownloadYtDlp);
    s.setValue(QStringLiteral("autoArchiveByType"), m_autoArchiveByType);
    s.setValue(QStringLiteral("overwriteExisting"), m_overwriteExisting);
    s.setValue(QStringLiteral("ytDlpCustomPath"), m_ytDlpCustomPath);
    s.setValue(QStringLiteral("ffmpegCustomPath"), m_ffmpegCustomPath);
    s.setValue(QStringLiteral("aria2CustomPath"), m_aria2CustomPath);
    s.setValue(QStringLiteral("autoDownloadAria2"), m_autoDownloadAria2);
    s.setValue(QStringLiteral("autoDownloadFfmpeg"), m_autoDownloadFfmpeg);
    s.setValue(QStringLiteral("webEnabled"), m_webEnabled);
    s.setValue(QStringLiteral("webPort"), m_webPort);
    s.setValue(QStringLiteral("webToken"), m_webToken);
    s.setValue(QStringLiteral("shutdownAction"), m_shutdownAction);
    s.setValue(QStringLiteral("shutdownGraceSec"), m_shutdownGraceSec);
    s.setValue(QStringLiteral("connectionsPerServer"), m_connectionsPerServer);
    s.setValue(QStringLiteral("retryCount"), m_retryCount);
    s.setValue(QStringLiteral("userAgent"), m_userAgent);
    s.setValue(QStringLiteral("connectTimeoutSec"), m_connectTimeoutSec);
    s.setValue(QStringLiteral("capturedExtensions"), m_capturedExtensions);

    s.setValue(QStringLiteral("proxyType"), m_proxyType);
    s.setValue(QStringLiteral("proxyHost"), m_proxyHost);
    s.setValue(QStringLiteral("proxyPort"), m_proxyPort);
    s.setValue(QStringLiteral("proxyUser"), m_proxyUser);
    s.setValue(QStringLiteral("proxyPass"), m_proxyPass);

    s.beginWriteArray(QStringLiteral("siteLogins"), m_siteLogins.size());
    for (int i = 0; i < m_siteLogins.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("url"),  m_siteLogins[i].url);
        s.setValue(QStringLiteral("user"), m_siteLogins[i].username);
        s.setValue(QStringLiteral("pass"), m_siteLogins[i].password);
    }
    s.endArray();
}

void Settings::applyToEngine() const
{
    DownloadConfig cfg = dlmgr_get_config();
    cfg.thread_pool_size   = m_maxThreads;
    cfg.max_concurrent     = m_maxConcurrent;
    cfg.retry_max          = m_retryCount;
    cfg.speed_limit_global = (m_speedLimitKBps > 0) ? (m_speedLimitKBps * 1024) : 0;
    /* 每服务器连接数：作为每任务并发连接数的上限（引擎里与 thread_pool_size 取小） */
    cfg.max_conn_per_server = m_connectionsPerServer;
    /* 同名文件：默认 0=自动重命名（引擎侧 pick_free_filename），勾选后 1=覆盖删除 */
    cfg.overwrite_existing  = m_overwriteExisting ? 1 : 0;

    // 代理：将设置页的 "none"/"http"/"socks" 映射为引擎 ProxyType
    int ptype = 0;  // PROXY_NONE
    if (m_proxyType == QStringLiteral("http"))
        ptype = 1;  // PROXY_HTTP
    else if (m_proxyType == QStringLiteral("socks"))
        ptype = 3;  // PROXY_SOCKS5
    cfg.proxy_type = ptype;
    std::strncpy(cfg.proxy_host, m_proxyHost.toUtf8().constData(),
                 sizeof(cfg.proxy_host) - 1);
    cfg.proxy_host[sizeof(cfg.proxy_host) - 1] = '\0';
    cfg.proxy_port = m_proxyPort;

    // 代理认证凭据（HTTP/SOCKS 代理均需时下发到 libcurl）
    std::strncpy(cfg.proxy_user, m_proxyUser.toUtf8().constData(),
                 sizeof(cfg.proxy_user) - 1);
    cfg.proxy_user[sizeof(cfg.proxy_user) - 1] = '\0';
    std::strncpy(cfg.proxy_pass, m_proxyPass.toUtf8().constData(),
                 sizeof(cfg.proxy_pass) - 1);
    cfg.proxy_pass[sizeof(cfg.proxy_pass) - 1] = '\0';

    // 用户代理（UA）：空则引擎使用默认 UA
    std::strncpy(cfg.user_agent, m_userAgent.toUtf8().constData(),
                 sizeof(cfg.user_agent) - 1);
    cfg.user_agent[sizeof(cfg.user_agent) - 1] = '\0';

    // 连接超时（秒）
    cfg.connect_timeout_sec = m_connectTimeoutSec;

    // 默认保存目录
    std::strncpy(cfg.default_save_dir,
                 m_defaultSaveDir.toUtf8().constData(),
                 sizeof(cfg.default_save_dir) - 1);
    cfg.default_save_dir[sizeof(cfg.default_save_dir) - 1] = '\0';

    // 站点登录凭据：下发到引擎，命中 URL 的请求自动带 HTTP 认证（libcurl CURLAUTH_ANY）
    int sc = m_siteLogins.size();
    if (sc > MAX_SITE_LOGINS) sc = MAX_SITE_LOGINS;
    for (int i = 0; i < sc; ++i) {
        const SiteLogin& l = m_siteLogins[i];
        auto put = [](char* dst, size_t cap, const QString& src) {
            std::strncpy(dst, src.trimmed().toUtf8().constData(), cap - 1);
            dst[cap - 1] = '\0';
        };
        put(cfg.site_logins[i].match, sizeof(cfg.site_logins[i].match), l.url);
        put(cfg.site_logins[i].user,  sizeof(cfg.site_logins[i].user),  l.username);
        put(cfg.site_logins[i].pass,  sizeof(cfg.site_logins[i].pass),  l.password);
    }
    cfg.site_login_count = sc;

    dlmgr_set_config(&cfg);
    dlmgr_set_speed_limit(cfg.speed_limit_global);  // 立即生效（运行时限速）
}
