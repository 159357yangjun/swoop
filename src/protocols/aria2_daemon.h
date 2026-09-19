#ifndef ARIA2_DAEMON_H
#define ARIA2_DAEMON_H

#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QMap>
#include <QJsonObject>

// 共享 aria2c 守护进程（单例）。
//
// 设计动机（对标 FDM 的“单引擎统一进程”思路）：
//   早期实现为每个 BT/FTP 任务各 fork 一个 aria2c 子进程并抢同一个固定 RPC 端口，
//   导致①并发任务端口冲突必然失败；②N 个进程占用 N× 内存/句柄/线程。
//   本守护进程全程只维护【一个】aria2c 实例，所有 BT/FTP 任务经 GID 复用它，
//   全局限速也终于成为真正的“跨任务聚合限速”（而非过去的每任务总限速）。
class Aria2Daemon : public QObject {
    Q_OBJECT
public:
    static Aria2Daemon* instance();

    bool ensureRunning();      // 启动共享 aria2c（幂等）；返回是否可用
    void shutdown();           // 终止共享进程（应用退出时调用）

    bool isAvailable() const;  // 探测 aria2c 是否可用（--version）
    bool featuresKnown() const { return m_versionChecked; }
    bool supportsBitTorrent() const {
        return m_features.contains(QStringLiteral("BitTorrent"));
    }

    // 任务注册：taskId 唯一；gid 为该任务在 aria2 中的全局标识（add 成功后才可知）
    void registerTask(int taskId, const QString& gid);
    void unregisterTask(int taskId);

    // RPC 封装（结果经 rpcReply(tag,obj) 按 tag 路由回对应任务）
    void addUri(int taskId, const QString& uri, const QString& dir, const QString& out);
    void addTorrent(int taskId, const QByteArray& base64, const QString& dir, const QString& out);
    void tellStatus(int taskId, const QString& gid);
    void pause(int taskId, const QString& gid);
    void unpause(int taskId, const QString& gid);
    void remove(int taskId, const QString& gid);

    void setGlobalDownloadLimit(int kbps);   // 0 = 不限速；立即生效

signals:
    // 把 RPC 回包按 tag 转发给对应下载器（tag 形如 "add|5" / "stat|5"）
    void rpcReply(const QString& tag, const QJsonObject& obj);
    void daemonStopped();   // 共享进程异常退出（所有任务应判失败）

private slots:
    void onPollTimer();
    void onRpcReply(QNetworkReply* reply);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    void onIdleTimeout();       // 无任务空闲超时 → 关闭 aria2c 进程（省内存）

private:
    Aria2Daemon();
    void startProcess();
    void ensurePolling();       // 有任务时才启动轮询；无任务时停止（省常驻定时器）
    void handleProcessGone();   // aria2c 异常退出/失败：清空并允许按需重启
    void sendRpc(const QString& method, const QJsonArray& params, const QString& tag);
    void sendVersion();
    void applyGlobalOption();
    void pollGlobalStat();

    QProcess*          m_proc = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer*            m_poll = nullptr;
    QTimer*            m_idleTimer = nullptr;   // 空闲自退定时器（单次）
    QMap<int, QString> m_taskGid;    // taskId -> gid
    int                m_reqId = 0;
    bool               m_versionChecked = false;
    bool               m_limitApplied   = false;
    bool               m_stopping       = false;
    int                m_globalLimitKbps = 0;
    int                m_gstatTick = 0;
    QStringList        m_features;
    static constexpr quint16 RPC_PORT = 16800;
    static constexpr int IDLE_TIMEOUT_MS = 15000;   // 无任务空载 15s 后自退
    static const char* const RPC_SECRET;
};

#endif // ARIA2_DAEMON_H
