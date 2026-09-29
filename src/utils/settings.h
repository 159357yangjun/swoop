#ifndef SETTINGS_H
#define SETTINGS_H

#include <QString>
#include <QStringList>
#include <QList>

// 站点登录凭据：命中 URL 的下载任务自动带 HTTP 认证（引擎侧走 libcurl CURLAUTH_ANY，
// 由服务端 WWW-Authenticate 决定 Basic/Digest/NTLM）。匹配规则见 apply_site_auth()。
struct SiteLogin {
    QString url;       // 站点匹配规则（域名或 URL 前缀）
    QString username;
    QString password;
};

// 全局应用设置：默认下载目录、最大线程数、全局限速、主题、文件类型、代理、站点登录等
// 用 QSettings 持久化到系统配置（Windows 注册表 / 其他平台 Ini）
class Settings {
public:
    Settings();

    void load();          // 从磁盘读取
    void save() const;    // 写入磁盘
    void applyToEngine() const;  // 同步到下载引擎全局配置

    static QString defaultDownloadDir();  // 首次运行时的默认目录

    // ── 常规 ──
    const QString& defaultSaveDir() const { return m_defaultSaveDir; }
    void setDefaultSaveDir(const QString& d) { m_defaultSaveDir = d; }

    const QString& theme() const { return m_theme; }
    void setTheme(const QString& t) { m_theme = t; }

    bool closeToTray() const { return m_closeToTray; }
    void setCloseToTray(bool v) { m_closeToTray = v; }

    bool clipboardMonitor() const { return m_clipboardMonitor; }
    void setClipboardMonitor(bool v) { m_clipboardMonitor = v; }

    // ── 视频组件 / 自动归档 ──
    bool autoDownloadYtDlp() const { return m_autoDownloadYtDlp; }
    void setAutoDownloadYtDlp(bool v) { m_autoDownloadYtDlp = v; }

    bool autoArchiveByType() const { return m_autoArchiveByType; }
    void setAutoArchiveByType(bool v) { m_autoArchiveByType = v; }

    // ── 同名文件 ──
    // true = 覆盖同名文件；false（默认）= 自动重命名为「名字 (1).ext」。
    // 由 Settings::applyToEngine 下发到引擎的 cfg.overwrite_existing。
    bool overwriteExisting() const { return m_overwriteExisting; }
    void setOverwriteExisting(bool v) { m_overwriteExisting = v; }

    // ── 下载完成后电源动作 ──
    // 取值："none" | "shutdown" | "sleep"（休眠）
    const QString& shutdownAction() const { return m_shutdownAction; }
    void setShutdownAction(const QString& a) { m_shutdownAction = a; }

    int shutdownGraceSec() const { return m_shutdownGraceSec; }
    void setShutdownGraceSec(int s) { m_shutdownGraceSec = s; }

    const QString& ytDlpCustomPath() const { return m_ytDlpCustomPath; }
    void setYtDlpCustomPath(const QString& p) { m_ytDlpCustomPath = p; }

    // ffmpeg 路径（HLS 合并/解密用，空=自动查找 PATH / 捆绑目录）
    const QString& ffmpegCustomPath() const { return m_ffmpegCustomPath; }
    void setFfmpegCustomPath(const QString& p) { m_ffmpegCustomPath = p; }

    // aria2c 路径（BT/磁力后端，空=自动查找 PATH / 捆绑目录）
    const QString& aria2CustomPath() const { return m_aria2CustomPath; }
    void setAria2CustomPath(const QString& p) { m_aria2CustomPath = p; }

    // aria2c 缺失时自动下载（开箱即用，BT/磁力免手动安装）
    bool autoDownloadAria2() const { return m_autoDownloadAria2; }
    void setAutoDownloadAria2(bool v) { m_autoDownloadAria2 = v; }

    // ffmpeg 缺失时自动下载（开箱即用，HLS TS→MP4 转封装免手动安装）
    bool autoDownloadFfmpeg() const { return m_autoDownloadFfmpeg; }
    void setAutoDownloadFfmpeg(bool v) { m_autoDownloadFfmpeg = v; }

    // ── Web 远程管理界面 ──
    bool webEnabled() const { return m_webEnabled; }
    void setWebEnabled(bool v) { m_webEnabled = v; }

    int webPort() const { return m_webPort; }
    void setWebPort(int p) { m_webPort = p; }

    const QString& webToken() const { return m_webToken; }
    void setWebToken(const QString& t) { m_webToken = t; }

    // ── 文件类型（剪贴板 / 嗅探自动捕获的扩展名）──
    const QStringList& capturedExtensions() const { return m_capturedExtensions; }
    void setCapturedExtensions(const QStringList& v) { m_capturedExtensions = v; }

    // ── 下载 ──
    int maxThreads() const { return m_maxThreads; }
    void setMaxThreads(int n) { m_maxThreads = n; }

    int maxConcurrent() const { return m_maxConcurrent; }
    void setMaxConcurrent(int n) { m_maxConcurrent = n; }

    int speedLimitKBps() const { return m_speedLimitKBps; }
    void setSpeedLimitKBps(int k) { m_speedLimitKBps = k; }

    bool http2Enabled() const { return m_http2Enabled; }
    void setHttp2Enabled(bool b) { m_http2Enabled = b; }

    // 流量档位（对标 FDM 工具栏流量使用模式）：0=自动 1=轻量 2=中等 3=重量 -1=自定义
    // 选择档位时会把对应限速值同步进 speedLimitKBps；手动在设置里改限速值则记为 -1（自定义）
    int trafficMode() const { return m_trafficMode; }
    void setTrafficMode(int m) { m_trafficMode = m; }

    // 任务列表分组模式：0=不分组 1=按类型 2=按队列（持久化，重启后恢复）
    int groupMode() const { return m_groupMode; }
    void setGroupMode(int m) { m_groupMode = m; }

    int connectionsPerServer() const { return m_connectionsPerServer; }
    void setConnectionsPerServer(int n) { m_connectionsPerServer = n; }

    int retryCount() const { return m_retryCount; }
    void setRetryCount(int n) { m_retryCount = n; }

    const QString& userAgent() const { return m_userAgent; }
    void setUserAgent(const QString& u) { m_userAgent = u; }

    // ── 连接 ──
    int connectTimeoutSec() const { return m_connectTimeoutSec; }
    void setConnectTimeoutSec(int n) { m_connectTimeoutSec = n; }

    // ── 代理 ──
    const QString& proxyType() const { return m_proxyType; }  // "none" | "http" | "socks"
    void setProxyType(const QString& t) { m_proxyType = t; }

    const QString& proxyHost() const { return m_proxyHost; }
    void setProxyHost(const QString& h) { m_proxyHost = h; }

    int proxyPort() const { return m_proxyPort; }
    void setProxyPort(int p) { m_proxyPort = p; }

    const QString& proxyUser() const { return m_proxyUser; }
    void setProxyUser(const QString& u) { m_proxyUser = u; }

    const QString& proxyPass() const { return m_proxyPass; }
    void setProxyPass(const QString& p) { m_proxyPass = p; }

    // ── 站点登录 ──
    const QList<SiteLogin>& siteLogins() const { return m_siteLogins; }
    void setSiteLogins(const QList<SiteLogin>& v) { m_siteLogins = v; }

private:
    QString     m_defaultSaveDir;
    QString     m_theme          = QStringLiteral("light");
    bool        m_closeToTray    = false;   // 关闭窗口时最小化到托盘
    bool        m_clipboardMonitor = true;  // 剪贴板监听开关（默认开启）
    bool        m_autoDownloadYtDlp = true; // yt-dlp 缺失时自动下载（开箱即用）
    bool        m_autoArchiveByType = false;// 按文件类型归档到子目录
    bool        m_overwriteExisting = false;// 同名文件：默认不覆盖，自动改名
    QString     m_ytDlpCustomPath;          // 用户指定的 yt-dlp 路径（空=自动）
    QString     m_ffmpegCustomPath;         // 用户指定的 ffmpeg 路径（空=自动）
    QString     m_aria2CustomPath;          // 用户指定的 aria2c 路径（空=自动）
    bool        m_autoDownloadAria2 = true; // aria2c 缺失时自动下载（开箱即用）
    bool        m_autoDownloadFfmpeg = true;// ffmpeg 缺失时自动下载（开箱即用）
    bool        m_webEnabled     = false;   // Web 远程管理界面开关
    int         m_webPort        = 8080;    // Web 监听端口
    QString     m_webToken;                 // Web 访问令牌（首次运行随机生成并持久化）
    QString     m_shutdownAction   = QStringLiteral("none"); // 下载完成电源动作
    int         m_shutdownGraceSec = 60;    // 关机/休眠前倒计时秒数
    QStringList m_capturedExtensions;       // 自动捕获的扩展名
    int         m_maxThreads     = 8;
    int         m_maxConcurrent  = 4;       // 同时下载的任务数
    int         m_speedLimitKBps = 0;       // 0 = 不限速
    int         m_trafficMode    = 0;       // 流量档位：0=自动 1=轻量 2=中等 3=重量 -1=自定义
    int         m_groupMode      = 0;       // 任务列表分组：0=不分组 1=按类型 2=按队列
    bool        m_http2Enabled   = true;    // 启用 HTTP/2 协商（HTTPS/ALPN 自动提速）
    int         m_connectionsPerServer = 8; // 每服务器连接数：封顶单个任务并发到同一主机的连接数
                                            // （引擎侧与 maxThreads 取较小值生效，见 effective_threads()）
    int         m_retryCount     = 5;
    QString     m_userAgent      = QStringLiteral("IDM-Next/0.1");
    int         m_connectTimeoutSec = 30;
    QString     m_proxyType      = QStringLiteral("none");
    QString     m_proxyHost;
    int         m_proxyPort      = 0;
    QString     m_proxyUser;
    QString     m_proxyPass;
    QList<SiteLogin> m_siteLogins;
};

#endif // SETTINGS_H
