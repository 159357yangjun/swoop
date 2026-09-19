#ifndef TORRENT_DOWNLOADER_H
#define TORRENT_DOWNLOADER_H

#include "idownloader.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class Aria2Daemon;  // 前向声明，避免引入守护进程头

// BT/磁力/FTP 下载器：复用共享的 aria2c 守护进程（Aria2Daemon 单例）。
//
// 本类只负责“单个任务”的生命周期与进度解析，不再各自持有 aria2c 进程——
// 所有任务经 GID 复用同一个 aria2c 实例（对标 FDM 单引擎，显著降低占用，
// 并真正支持多 BT/FTP 任务并发）。
class TorrentDownloader : public IDownloader {
    Q_OBJECT
public:
    explicit TorrentDownloader(QObject* parent = nullptr);
    ~TorrentDownloader() override;

    bool canHandle(const QString& url) const override;
    void start(const DownloadRequest& req) override;
    void pause() override;
    void resume() override;
    void cancel() override;

    // 设置 aria2c 可执行文件路径（默认从 PATH / 捆绑目录查找）
    static void setAria2Path(const QString& path);
    static QString aria2Path();

    // 已捆绑/缓存路径（应用目录或 AppData，不含 PATH 回退）
    static QString bundledAria2Path();

    // 检测系统是否安装了 aria2c
    static bool isAvailable();

    // 判断 URL 是否为磁力链接 / .torrent 文件（可直接交给本下载器）
    static bool isTorrentUrl(const QString& url);

    // 本下载器（aria2 后端）能处理的全部 URL：magnet / .torrent / ftp(s)://
    static bool isAria2Url(const QString& url);

    // 设置本下载器关联的任务 ID（信号中携带，接入主窗口任务列表）
    void setTaskId(int id);

    // ── 全局限速（覆盖 BT/磁力/FTP 任务，弥补纯 C 引擎限速管不到 aria2 后端的缺口）──
    // 0 = 不限速。转发给共享守护进程，成为真正的跨任务聚合限速。
    static void setGlobalSpeedLimit(int kbps);

private slots:
    void onDaemonReply(const QString& tag, const QJsonObject& obj);
    void onDaemonStopped();
    void attemptRetry();         // 守护进程崩溃后的重投执行
    void onWatchdog();           // 看门狗：add 确认超时（RPC 未就绪）则再试

private:
    void addDownload();          // 首轮/重投下发任务（addUri / addTorrent）
    void handleAddResult(const QJsonObject& res);
    void handleStatusResult(const QJsonObject& res);
    void scheduleRetry();        // 安排一次退避后的重投
    bool isLocalTorrent(const QString& url) const;

    static constexpr int MAX_RETRIES = 2;   // 原 1 次 + 重投 2 次 = 最多 3 次

    int         m_taskId   = -1;
    QString     m_url;
    QString     m_savePath;
    QString     m_fileName;
    QString     m_gid;           // aria2 任务全局标识
    bool        m_added    = false;
    qint64      m_total    = -1;
    qint64      m_downloaded = 0;
    int         m_speedBps = 0;
    bool        m_cancelled = false;
    bool        m_paused   = false;
    bool        m_active   = false;   // 任务是否在途（start 后置 true，完成/取消/失败后清）
    int         m_retries  = 0;       // 已重投次数（守护进程崩溃后自动重投用）
    QTimer*     m_retryTimer = nullptr; // 退避定时器（单次）
    QTimer*     m_watchdog   = nullptr; // add 确认看门狗（单次）

    static QString s_aria2Path;
};

#endif // TORRENT_DOWNLOADER_H
