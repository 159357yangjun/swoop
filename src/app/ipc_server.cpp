#include "ipc_server.h"
#include "logger.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>

static const char* IPC_SERVER_NAME = "idm-next-ipc";

IpcServer::IpcServer(QObject* parent)
    : QObject(parent)
{
}

void IpcServer::setAddTaskCallback(
    std::function<void(const QString&, const QString&, const QString&, int, const QString&, const QString&)> cb)
{
    m_addTask = std::move(cb);
}

void IpcServer::setTaskCountProvider(std::function<int()> cb)
{
    m_taskCount = std::move(cb);
}

bool IpcServer::start()
{
    m_server = new QLocalServer(this);

    // 如果有残留的旧服务端（上次崩溃），先移除
    QLocalServer::removeServer(QLatin1String(IPC_SERVER_NAME));

    if (!m_server->listen(QLatin1String(IPC_SERVER_NAME))) {
        Log::warn(QStringLiteral("IPC server 启动失败: %1").arg(m_server->errorString()));
        return false;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &IpcServer::onNewConnection);

    Log::info(QStringLiteral("IPC server 已启动，监听 %1").arg(QLatin1String(IPC_SERVER_NAME)));
    return true;
}

void IpcServer::onNewConnection()
{
    QLocalSocket* client = m_server->nextPendingConnection();
    if (!client) return;
    handleClient(client);
}

void IpcServer::handleClient(QLocalSocket* client)
{
    // 读取客户端发来的 JSON 命令：原生消息以单次写入整体送达，但可能跨多次
    // readyRead 分片到达；此处用有界循环累积，直到得到可解析的完整 JSON 对象，
    // 避免单次 readAll 截断导致命令解析失败（原实现会退化成「不支持的操作」）。
    QByteArray data;
    for (int i = 0; i < 10; ++i) {
        if (client->bytesAvailable() == 0)
            client->waitForReadyRead(200);   // 最多 ~2s，正常情况首轮即有数据
        QByteArray chunk = client->readAll();
        if (!chunk.isEmpty())
            data.append(chunk);
        if (!data.isEmpty() && QJsonDocument::fromJson(data).isObject())
            break;   // 已收到完整命令
        if (client->state() != QLocalSocket::ConnectedState)
            break;   // 对端已断开
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject msg = doc.object();

    QString command = msg.value("command").toString();
    QJsonObject response;

    if (command == "add") {
        QString url      = msg.value("url").toString();
        QString filename = msg.value("filename").toString();
        QString dir      = msg.value("dir").toString();
        QString queue    = msg.value("queue").toString();    // 非空=加入队列
        QString format   = msg.value("format").toString();   // 视频画质（yt-dlp -f 选择串）
        int    threads   = msg.value("threads").toInt();

        if (url.isEmpty()) {
            response["success"] = false;
            response["message"] = QStringLiteral("URL 为空");
        } else {
            if (m_addTask) m_addTask(url, filename, dir, threads, queue, format);
            response["success"] = true;
            response["message"] = QStringLiteral("已添加下载: %1").arg(filename.isEmpty() ? url : filename);
        }
    } else if (command == "list") {
        // 简化：返回任务数量
        const int n = m_taskCount ? m_taskCount() : 0;
        response["success"] = true;
        response["message"] = QStringLiteral("当前 %1 个任务").arg(n);
    } else {
        response["success"] = false;
        response["message"] = QStringLiteral("不支持的操作: %1").arg(command);
    }

    // 发送响应
    QByteArray respData = QJsonDocument(response).toJson(QJsonDocument::Compact);
    client->write(respData);
    client->flush();
    client->waitForBytesWritten(2000);
    client->disconnectFromServer();

    Log::info(QStringLiteral("IPC 请求: %1 → %2")
                  .arg(command, response.value("success").toBool() ? "成功" : "失败"));
}
