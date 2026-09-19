#include "aria2_daemon.h"
#include "torrent_downloader.h"   // 复用路径解析 / isAvailable 静态方法
#include "logger.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

const char* const Aria2Daemon::RPC_SECRET = "idm-next-bt-rpc-secret";

Aria2Daemon* Aria2Daemon::instance()
{
    static Aria2Daemon s;
    return &s;
}

Aria2Daemon::Aria2Daemon()
{
    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &Aria2Daemon::onRpcReply);
    // 空闲自退定时器：任务全部清空后空载一段时间再关 aria2c（省内存）
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(IDLE_TIMEOUT_MS);
    connect(m_idleTimer, &QTimer::timeout, this, &Aria2Daemon::onIdleTimeout);
    // 应用退出时干净关闭共享进程（不视为异常）
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &Aria2Daemon::shutdown);
    }
}

bool Aria2Daemon::isAvailable() const
{
    return TorrentDownloader::isAvailable();
}

void Aria2Daemon::startProcess()
{
    if (m_proc)
        return;
    m_stopping = false;   // 无论是首启还是空闲自退后重启，都视为“运行中”（允许后续崩溃自愈）
    m_proc = new QProcess(this);
    QStringList args = {
        QStringLiteral("--enable-rpc"),
        QStringLiteral("--rpc-listen-port=%1").arg(RPC_PORT),
        QStringLiteral("--rpc-secret=%1").arg(QString::fromUtf8(RPC_SECRET)),
        QStringLiteral("--rpc-allow-origin-all"),
        QStringLiteral("--seed-time=0"),        // 下载完即停，不做做种
        QStringLiteral("--continue=true"),
        QStringLiteral("--max-connection-per-server=8"),
    };
    if (m_globalLimitKbps > 0)
        args.append(QStringLiteral("--max-overall-download-limit=%1")
                        .arg(m_globalLimitKbps * 1024));

    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Aria2Daemon::onFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &Aria2Daemon::onError);

    Log::info(QStringLiteral("Aria2Daemon 启动共享 aria2c: %1 %2")
                  .arg(TorrentDownloader::aria2Path()).arg(args.join(' ')));
    m_proc->start(TorrentDownloader::aria2Path(), args);
    if (!m_proc->waitForStarted(5000)) {
        Log::error(QStringLiteral("Aria2Daemon 无法启动 aria2c（路径: %1）")
                       .arg(TorrentDownloader::aria2Path()));
        m_proc->deleteLater();
        m_proc = nullptr;
        return;
    }
    // (重新)启动后需重新探明特性并下发限速（新进程状态是干净的）
    m_versionChecked = false;
    m_limitApplied   = false;
    m_taskGid.clear();          // 旧进程已逝，GID 全部失效
    // 仅创建轮询定时器对象（保持停止）；真正启动推迟到首个任务 registerTask，
    // 这样无任务时（纯 HTTP 下载或任务全部完成后）不会有一个常驻 500ms 定时器空转
    if (!m_poll) {
        m_poll = new QTimer(this);
        m_poll->setInterval(500);
        connect(m_poll, &QTimer::timeout, this, &Aria2Daemon::onPollTimer);
    }
}

bool Aria2Daemon::ensureRunning()
{
    if (m_idleTimer && m_idleTimer->isActive())
        m_idleTimer->stop();   // 有任务要用守护进程 → 取消空闲自退
    if (m_proc && m_proc->state() == QProcess::Running)
        return true;
    if (!isAvailable())
        return false;
    startProcess();
    return m_proc && m_proc->state() == QProcess::Running;
}

void Aria2Daemon::shutdown()
{
    m_stopping = true;
    if (m_idleTimer) { m_idleTimer->stop(); }
    if (m_poll) { m_poll->stop(); m_poll->deleteLater(); m_poll = nullptr; }
    if (m_proc) {
        if (m_proc->state() == QProcess::Running) {
            m_proc->terminate();
            if (!m_proc->waitForFinished(2000))
                m_proc->kill();
        }
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

void Aria2Daemon::registerTask(int taskId, const QString& gid)
{
    if (m_idleTimer) m_idleTimer->stop();   // 有在途任务 → 取消空闲自退
    m_taskGid[taskId] = gid;
    ensurePolling();   // 有在途任务 → 启动/恢复轮询（无任务时空转的定时器已停）
}

void Aria2Daemon::unregisterTask(int taskId)
{
    m_taskGid.remove(taskId);
    if (m_taskGid.isEmpty()) {
        // 任务全部清空 → 停止常驻轮询，并启动空闲自退（空载 IDLE_TIMEOUT_MS 后关进程）
        if (m_poll && m_poll->isActive())
            m_poll->stop();
        if (m_idleTimer && !m_idleTimer->isActive())
            m_idleTimer->start();
    }
}

void Aria2Daemon::onIdleTimeout()
{
    // 空载超时：仅当确实没有任务时才关闭 aria2c（省内存）；新任务会经 ensureRunning 即时拉起
    if (!m_taskGid.isEmpty())
        return;
    Log::info(QStringLiteral("Aria2Daemon 空闲 %1ms，关闭共享 aria2c 进程").arg(IDLE_TIMEOUT_MS));
    shutdown();
}

void Aria2Daemon::ensurePolling()
{
    if (!m_poll)
        return;                 // 进程未启动（startProcess 才会建对象）
    if (!m_poll->isActive())
        m_poll->start();
}

void Aria2Daemon::addUri(int taskId, const QString& uri, const QString& dir, const QString& out)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    QJsonArray uris; uris.append(uri);
    params.append(uris);
    QJsonObject opts;
    if (!dir.isEmpty())  opts.insert(QStringLiteral("dir"), dir);
    if (!out.isEmpty())  opts.insert(QStringLiteral("out"), out);
    params.append(opts);
    sendRpc(QStringLiteral("aria2.addUri"), params,
            QStringLiteral("add|") + QString::number(taskId));
}

void Aria2Daemon::addTorrent(int taskId, const QByteArray& base64,
                             const QString& dir, const QString& out)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    params.append(QString::fromLatin1(base64.toBase64()));
    params.append(QJsonArray());
    QJsonObject opts;
    if (!dir.isEmpty())  opts.insert(QStringLiteral("dir"), dir);
    if (!out.isEmpty())  opts.insert(QStringLiteral("out"), out);
    params.append(opts);
    sendRpc(QStringLiteral("aria2.addTorrent"), params,
            QStringLiteral("add|") + QString::number(taskId));
}

void Aria2Daemon::tellStatus(int taskId, const QString& gid)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    params.append(gid);
    QJsonArray keys;
    keys.append(QStringLiteral("totalLength"));
    keys.append(QStringLiteral("completedLength"));
    keys.append(QStringLiteral("downloadSpeed"));
    keys.append(QStringLiteral("status"));
    params.append(keys);
    sendRpc(QStringLiteral("aria2.tellStatus"), params,
            QStringLiteral("stat|") + QString::number(taskId));
}

void Aria2Daemon::pause(int, const QString& gid)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    params.append(gid);
    sendRpc(QStringLiteral("aria2.pause"), params, QStringLiteral("pause"));
}

void Aria2Daemon::unpause(int, const QString& gid)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    params.append(gid);
    sendRpc(QStringLiteral("aria2.unpause"), params, QStringLiteral("unpause"));
}

void Aria2Daemon::remove(int, const QString& gid)
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    params.append(gid);
    sendRpc(QStringLiteral("aria2.remove"), params, QStringLiteral("remove"));
}

void Aria2Daemon::setGlobalDownloadLimit(int kbps)
{
    m_globalLimitKbps = kbps;
    m_limitApplied = false;   // 触发下次轮询重新下发（或在下方立即下发）
    if (m_proc && m_proc->state() == QProcess::Running)
        applyGlobalOption();
}

void Aria2Daemon::sendVersion()
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    sendRpc(QStringLiteral("aria2.getVersion"), params, QStringLiteral("ver"));
}

void Aria2Daemon::applyGlobalOption()
{
    if (!m_proc || m_proc->state() != QProcess::Running)
        return;
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    QJsonObject opt;
    opt.insert(QStringLiteral("max-overall-download-limit"),
               QString::number(m_globalLimitKbps > 0 ? m_globalLimitKbps * 1024 : 0));
    params.append(opt);
    sendRpc(QStringLiteral("aria2.changeGlobalOption"), params, QStringLiteral("gopt"));
    m_limitApplied = true;
    Log::info(QStringLiteral("Aria2Daemon 应用全局限速: %1 KB/s").arg(m_globalLimitKbps));
}

void Aria2Daemon::pollGlobalStat()
{
    QJsonArray params;
    params.append(QStringLiteral("token:") + QString::fromUtf8(RPC_SECRET));
    sendRpc(QStringLiteral("aria2.getGlobalStat"), params, QStringLiteral("gstat"));
}

void Aria2Daemon::sendRpc(const QString& method, const QJsonArray& params, const QString& tag)
{
    if (!m_nam)
        return;
    QJsonObject req;
    req.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    // id = tag + "|" + reqId，避免 "stat|5"+"12" 与任务 512 混淆
    req.insert(QStringLiteral("id"), tag + QStringLiteral("|") + QString::number(++m_reqId));
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);

    QUrl url(QStringLiteral("http://127.0.0.1:%1/jsonrpc").arg(RPC_PORT));
    QNetworkRequest nr(url);
    nr.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    nr.setRawHeader("Accept", "application/json");
    QByteArray body = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_nam->post(nr, body);
    Q_UNUSED(reply);
}

void Aria2Daemon::onPollTimer()
{
    if (!m_versionChecked) { sendVersion(); m_versionChecked = true; }
    if (!m_limitApplied)   applyGlobalOption();
    // 为所有已注册（未完成）任务拉取状态——一个定时器替代 N 个
    for (auto it = m_taskGid.begin(); it != m_taskGid.end(); ++it) {
        if (!it.value().isEmpty())
            tellStatus(it.key(), it.value());
    }
    if (++m_gstatTick % 20 == 0)
        pollGlobalStat();   // 诊断：校验限速是否真正生效
}

void Aria2Daemon::onRpcReply(QNetworkReply* reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
        return;   // RPC 未就绪或瞬时连接异常：等待下一轮轮询
    QByteArray data = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return;
    QJsonObject obj = doc.object();
    QString id = obj.value(QStringLiteral("id")).toString();
    // id = baseTag + "|" + reqId，去掉末尾的请求序号得到 baseTag 用于路由
    QString base = id;
    int sep = base.lastIndexOf(QStringLiteral("|"));
    if (sep > 0)
        base = base.left(sep);

    if (base.startsWith(QStringLiteral("add|")) ||
        base.startsWith(QStringLiteral("stat|"))) {
        emit rpcReply(base, obj);   // 路由回对应任务
        return;
    }
    if (base == QStringLiteral("ver")) {
        if (!obj.contains(QStringLiteral("error"))) {
            QJsonObject r = obj.value(QStringLiteral("result")).toObject();
            m_features = r.value(QStringLiteral("enabledFeatures")).toVariant().toStringList();
            Log::info(QStringLiteral("Aria2Daemon aria2c 特性: %1")
                          .arg(m_features.join(QStringLiteral(", "))));
        }
        return;
    }
    if (base == QStringLiteral("gstat")) {
        if (!obj.contains(QStringLiteral("error"))) {
            QJsonObject r = obj.value(QStringLiteral("result")).toObject();
            qint64 down = r.value(QStringLiteral("downloadSpeed")).toString().toLongLong();
            Log::debug(QStringLiteral("Aria2Daemon 全局统计: 活动=%1 下载=%2B/s")
                           .arg(r.value(QStringLiteral("numActive")).toString()).arg(down));
        }
        return;
    }
    // gopt / pause / unpause / remove 无需回包处理
}

void Aria2Daemon::handleProcessGone()
{
    if (m_stopping)
        return;                 // 主动关闭（应用退出），不视为异常
    if (!m_proc)
        return;                 // 已处理过（onError/onFinished 可能双重触发）
    if (m_poll) { m_poll->stop(); m_poll = nullptr; }
    Log::error(QStringLiteral("Aria2Daemon 共享 aria2c 进程异常退出，下次需要时自动重启"));
    m_proc->deleteLater();
    m_proc = nullptr;           // 关键：允许下次 ensureRunning 重新拉起（否则被 if(m_proc) 挡住）
    emit daemonStopped();       // 通知在途任务判失败
}

void Aria2Daemon::onFinished(int, QProcess::ExitStatus)
{
    handleProcessGone();
}

void Aria2Daemon::onError(QProcess::ProcessError)
{
    handleProcessGone();
}
