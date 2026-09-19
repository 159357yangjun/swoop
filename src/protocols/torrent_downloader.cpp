#include "torrent_downloader.h"
#include "aria2_daemon.h"
#include "app_paths.h"
#include "logger.h"

#include <QFile>
#include <QDir>
#include <QUrl>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QCoreApplication>

// ── 静态成员 ──────────────────────────────────
QString TorrentDownloader::s_aria2Path;

TorrentDownloader::TorrentDownloader(QObject* parent)
    : IDownloader(parent)
{
    // 接入共享守护进程：回包按 tag 路由到本任务
    auto* d = Aria2Daemon::instance();
    connect(d, &Aria2Daemon::rpcReply, this, &TorrentDownloader::onDaemonReply);
    connect(d, &Aria2Daemon::daemonStopped, this, &TorrentDownloader::onDaemonStopped);

    // 守护进程崩溃后自动重投（非阻塞，单次触发）
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(2000);   // 给共享 aria2c 重启留时间
    connect(m_retryTimer, &QTimer::timeout, this, &TorrentDownloader::attemptRetry);

    m_watchdog = new QTimer(this);
    m_watchdog->setSingleShot(true);
    m_watchdog->setInterval(6000);     // add 超过 6s 未确认 → 视为 RPC 尚未就绪，再试
    connect(m_watchdog, &QTimer::timeout, this, &TorrentDownloader::onWatchdog);
}

TorrentDownloader::~TorrentDownloader()
{
    if (m_retryTimer) m_retryTimer->stop();
    if (m_watchdog)   m_watchdog->stop();
    if (!m_gid.isEmpty())
        Aria2Daemon::instance()->unregisterTask(m_taskId);
}

// ── 全局限速：转发给共享守护进程（真正跨任务聚合）──
void TorrentDownloader::setGlobalSpeedLimit(int kbps)
{
    Aria2Daemon::instance()->setGlobalDownloadLimit(kbps);
}

// ── 静态方法（路径解析 / 探测，保持与 TorrentBackend 的接口兼容）──
void TorrentDownloader::setAria2Path(const QString& path)
{
    s_aria2Path = path;
}

QString TorrentDownloader::aria2Path()
{
    if (!s_aria2Path.isEmpty())
        return s_aria2Path;
    QString bundled = bundledAria2Path();
    if (!bundled.isEmpty())
        return bundled;
    return QStringLiteral("aria2c");
}

QString TorrentDownloader::bundledAria2Path()
{
    // 1. 应用同目录下的 aria2/ 子目录（随安装包捆绑 / 便携模式）
    QString candidate = QCoreApplication::applicationDirPath()
                        + QStringLiteral("/aria2/aria2c.exe");
    if (QFile::exists(candidate))
        return candidate;
    // 2. AppPaths 数据目录（便携模式 = exe 目录，否则 AppData/Local；
    //    自动下载落点经 AppPaths::dataDir()，与此对齐）
    QString local = AppPaths::dataDir();
    if (!local.isEmpty()) {
        candidate = local + QStringLiteral("/aria2/aria2c.exe");
        if (QFile::exists(candidate))
            return candidate;
    }
    return QString();
}

bool TorrentDownloader::isAvailable()
{
    QProcess proc;
    proc.start(aria2Path(), { QStringLiteral("--version") });
    if (!proc.waitForStarted(3000))
        return false;
    if (!proc.waitForFinished(5000))
        return false;
    return proc.exitCode() == 0;
}

bool TorrentDownloader::isTorrentUrl(const QString& url)
{
    QString u = url.trimmed();
    if (u.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive))
        return true;
    if (u.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive))
        return true;
    return false;
}

bool TorrentDownloader::isAria2Url(const QString& url)
{
    QString u = url.trimmed();
    if (isTorrentUrl(u))
        return true;
    if (u.startsWith(QStringLiteral("ftp://"), Qt::CaseInsensitive))
        return true;
    if (u.startsWith(QStringLiteral("ftps://"), Qt::CaseInsensitive))
        return true;
    return false;
}

// IDownloader 路由判定：本后端能处理磁力 / 种子 / FTP(s) 链接
bool TorrentDownloader::canHandle(const QString& url) const
{
    return isAria2Url(url);
}

void TorrentDownloader::setTaskId(int id)
{
    m_taskId = id;
}

// ── IDownloader 接口 ──────────────────────────
void TorrentDownloader::start(const DownloadRequest& req)
{
    m_url       = req.url.trimmed();
    m_savePath  = req.savePath;
    m_fileName  = req.fileName;
    m_cancelled = false;
    m_paused    = false;
    m_added     = false;
    m_gid.clear();
    m_total     = -1;
    m_downloaded = 0;
    m_speedBps  = 0;
    m_retries   = 0;
    if (m_retryTimer) m_retryTimer->stop();
    if (m_watchdog)   m_watchdog->stop();

    if (!isAria2Url(m_url)) {
        emit completed(m_taskId, false, QStringLiteral("非磁力/种子/FTP 链接"));
        return;
    }

    auto* d = Aria2Daemon::instance();
    if (!d->ensureRunning()) {
        emit completed(m_taskId, false,
                       QStringLiteral("未找到 aria2c，请在设置中指定路径或开启自动下载"));
        emit stateChanged(m_taskId, 4);  // FAILED
        return;
    }
    // 能力校验：缺 BitTorrent 时磁力/种子必败，提前提示（仅当特性已探明）
    if (isTorrentUrl(m_url) && d->featuresKnown() && !d->supportsBitTorrent()) {
        emit completed(m_taskId, false,
                       QStringLiteral("当前 aria2c 未编译 BitTorrent 支持，无法下载磁力/种子"));
        emit stateChanged(m_taskId, 4);
        return;
    }

    QDir().mkpath(m_savePath);
    m_active = true;                 // 标记在途，供崩溃时判失败（避免幽灵任务）
    emit stateChanged(m_taskId, 1);  // RUNNING
    addDownload();
}

void TorrentDownloader::addDownload()
{
    auto* d = Aria2Daemon::instance();
    if (m_url.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive)) {
        d->addUri(m_taskId, m_url, m_savePath, m_fileName);
        return;
    }
    if (isLocalTorrent(m_url)) {
        QString local = QUrl(m_url).isLocalFile()
                            ? QUrl(m_url).toLocalFile() : m_url;
        QFile f(local);
        if (!f.open(QIODevice::ReadOnly)) {
            emit completed(m_taskId, false, QStringLiteral("无法读取种子文件: %1").arg(local));
            emit stateChanged(m_taskId, 4);
            return;
        }
        QByteArray data = f.readAll();
        f.close();
        d->addTorrent(m_taskId, data, m_savePath, m_fileName);
        return;
    }
    // http(s) 上的 .torrent 文件：交给 aria2 自动抓取并加载
    d->addUri(m_taskId, m_url, m_savePath, m_fileName);
}

void TorrentDownloader::pause()
{
    m_paused = true;
    emit stateChanged(m_taskId, 2);  // PAUSED
    if (!m_gid.isEmpty())
        Aria2Daemon::instance()->pause(m_taskId, m_gid);
}

void TorrentDownloader::resume()
{
    m_paused = false;
    emit stateChanged(m_taskId, 1);  // RUNNING
    if (!m_gid.isEmpty())
        Aria2Daemon::instance()->unpause(m_taskId, m_gid);
}

void TorrentDownloader::cancel()
{
    m_cancelled = true;
    m_active    = false;
    if (m_retryTimer) m_retryTimer->stop();
    if (m_watchdog)   m_watchdog->stop();
    if (!m_gid.isEmpty())
        Aria2Daemon::instance()->remove(m_taskId, m_gid);
    Aria2Daemon::instance()->unregisterTask(m_taskId);
    emit stateChanged(m_taskId, 5);  // CANCELLED
}

// ── 守护进程回包路由 ──────────────────────────
void TorrentDownloader::onDaemonReply(const QString& tag, const QJsonObject& obj)
{
    if (tag == QStringLiteral("add|") + QString::number(m_taskId))
        handleAddResult(obj);
    else if (tag == QStringLiteral("stat|") + QString::number(m_taskId))
        handleStatusResult(obj);
}

void TorrentDownloader::onDaemonStopped()
{
    // 守护进程崩溃：在途任务不再立即判失败，而是自动重投（最多 MAX_RETRIES 次）。
    // 已完成/取消/失败的任务（m_active=false）不受影响；取消中的也不重投。
    if (m_cancelled || !m_active)
        return;
    scheduleRetry();
}

void TorrentDownloader::scheduleRetry()
{
    if (m_retries >= MAX_RETRIES) {
        m_active = false;
        emit completed(m_taskId, false,
                       QStringLiteral("aria2c 反复崩溃，已停止自动重试（共 %1 次）").arg(MAX_RETRIES));
        emit stateChanged(m_taskId, 4);  // FAILED
        return;
    }
    if (m_retryTimer && !m_retryTimer->isActive())
        m_retryTimer->start();
}

void TorrentDownloader::attemptRetry()
{
    if (m_cancelled || !m_active)
        return;
    m_retries++;
    auto* d = Aria2Daemon::instance();
    if (!d->ensureRunning()) {
        m_active = false;
        emit completed(m_taskId, false, QStringLiteral("aria2c 无法重启，下载失败"));
        emit stateChanged(m_taskId, 4);  // FAILED
        return;
    }
    // 重置本轮提交状态，重新下发（torrent 文件会按原路径重新读取）
    m_gid.clear();
    m_added = false;
    Log::info(QStringLiteral("TorrentDownloader 任务 #%1 自动重投（第 %2/%3 次）")
                  .arg(m_taskId).arg(m_retries).arg(MAX_RETRIES));
    emit stateChanged(m_taskId, 1);  // RUNNING（重试中）
    addDownload();
    if (m_watchdog) m_watchdog->start();   // 防 add 静默丢弃：RPC 未就绪时再触发重投
}

void TorrentDownloader::onWatchdog()
{
    // add 确认超时：RPC 尚未就绪或回包丢失。仍无 gid 且任务在途 → 再投一次。
    if (!m_active || m_cancelled)
        return;
    if (!m_gid.isEmpty())
        return;                 // 已拿到 gid，无需处理
    scheduleRetry();
}

void TorrentDownloader::handleAddResult(const QJsonObject& res)
{
    auto* d = Aria2Daemon::instance();
    if (m_added)
        return;   // 防止重试/迟到回包重复处理
    if (res.contains(QStringLiteral("error"))) {
        QString msg = res.value(QStringLiteral("error"))
                          .toObject().value(QStringLiteral("message")).toString();
        if (m_watchdog) m_watchdog->stop();
        if (m_active && !m_cancelled && m_retries < MAX_RETRIES) {
            Log::warn(QStringLiteral("TorrentDownloader 任务 #%1 添加失败，准备重投: %2")
                          .arg(m_taskId).arg(msg));
            scheduleRetry();
        } else {
            m_active = false;
            emit completed(m_taskId, false, QStringLiteral("aria2 添加任务失败: %1").arg(msg));
            emit stateChanged(m_taskId, 4);
        }
        return;
    }
    if (m_watchdog) m_watchdog->stop();
    m_gid = res.value(QStringLiteral("result")).toString();
    m_added = true;
    d->registerTask(m_taskId, m_gid);   // 注册即触发守护进程轮询恢复
    Log::info(QStringLiteral("TorrentDownloader 任务已提交 gid=%1").arg(m_gid));
    // 在任务添加完成前收到暂停指令
    if (m_paused && !m_gid.isEmpty())
        d->pause(m_taskId, m_gid);
}

void TorrentDownloader::handleStatusResult(const QJsonObject& res)
{
    auto* d = Aria2Daemon::instance();
    if (res.contains(QStringLiteral("error"))) {
        QString msg = res.value(QStringLiteral("error"))
                          .toObject().value(QStringLiteral("message")).toString();
        emit completed(m_taskId, false, QStringLiteral("aria2 查询失败: %1").arg(msg));
        emit stateChanged(m_taskId, 4);
        d->unregisterTask(m_taskId);
        return;
    }
    QJsonObject r = res.value(QStringLiteral("result")).toObject();
    QString status = r.value(QStringLiteral("status")).toString();
    qint64 total = r.value(QStringLiteral("totalLength")).toString().toLongLong();
    qint64 done  = r.value(QStringLiteral("completedLength")).toString().toLongLong();
    int speed    = r.value(QStringLiteral("downloadSpeed")).toString().toInt();

    if (total > 0)  m_total = total;
    m_downloaded = done;
    m_speedBps  = speed;

    if (status == QStringLiteral("complete")) {
        m_active = false;
        emit progressChanged(m_taskId, m_total > 0 ? m_total : done,
                             m_total > 0 ? m_total : done, 0);
        emit completed(m_taskId, true, QString());
        emit stateChanged(m_taskId, 3);  // COMPLETED
        d->unregisterTask(m_taskId);
        return;
    }
    if (status == QStringLiteral("error") || status == QStringLiteral("removed")) {
        m_active = false;
        QString msg = (status == QStringLiteral("removed"))
                          ? QStringLiteral("已取消") : QStringLiteral("aria2 下载错误");
        emit completed(m_taskId, false, msg);
        emit stateChanged(m_taskId, 4);
        d->unregisterTask(m_taskId);
        return;
    }

    emit progressChanged(m_taskId, done, m_total > 0 ? m_total : done, speed);
}

bool TorrentDownloader::isLocalTorrent(const QString& url) const
{
    if (url.startsWith(QStringLiteral("file://"), Qt::CaseInsensitive))
        return true;
    if (url.contains(QStringLiteral("://")))
        return false;  // http(s)/ftp 等远程
    return QFile::exists(url);
}
