#include "ipc_server.h"
#include "cli_forward.h"
#include "logger.h"
#include "task_controller.h"
#include "task_list_model.h"
#include "download_manager.h"
#include "torrent_downloader.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWidget>

/* 管道名只有 cli_forward.h 的 ipcServerName() 一个来源：
   客户端（CLI、副实例）与服务端以前各自写一遍同一个字面量，改一处不会报错，
   只表现为「永远连不上」—— 那和真正的 IPC bug 长得一模一样。 */

namespace {
const TaskRow* findTask(const QVector<TaskRow>& tasks, int id)
{
    for (const TaskRow& row : tasks) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

QString taskLine(const TaskRow& row)
{
    return QStringLiteral("#%1\t%2\t%3\t%4")
        .arg(row.id)
        .arg(TaskListModel::stateText(row.state))
        .arg(row.fileName.isEmpty() ? QStringLiteral("-") : row.fileName)
        .arg(row.url.isEmpty() ? QStringLiteral("-") : row.url);
}
}

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

    // 如果有残留的旧服务端（上次崩溃），先移除。
    // 正常的第二个 GUI 实例会在 main() 中先通过 activate 命令退出，不会走到这里，
    // 因而不会误删仍在运行实例的 IPC endpoint。
    QLocalServer::removeServer(ipcServerName());

    if (!m_server->listen(ipcServerName())) {
        Log::warn(QStringLiteral("IPC server 启动失败: %1").arg(m_server->errorString()));
        return false;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &IpcServer::onNewConnection);

    Log::info(QStringLiteral("IPC server 已启动，监听 %1").arg(ipcServerName()));
    return true;
}

void IpcServer::onNewConnection()
{
    // newConnection 可能一次积累多个客户端；全部取出，避免短时间浏览器/CLI 连发时
    // 有连接滞留到下一次信号才被处理。
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* client = m_server->nextPendingConnection();
        if (client)
            handleClient(client);
    }
}

void IpcServer::handleClient(QLocalSocket* client)
{
    // 读取客户端发来的 JSON 命令：原生消息以单次写入整体送达，但可能跨多次
    // readyRead 分片到达；此处用有界循环累积，直到得到可解析的完整 JSON 对象，
    // 避免单次 readAll 截断导致命令解析失败。
    QByteArray data;
    for (int i = 0; i < 10; ++i) {
        if (client->bytesAvailable() == 0)
            client->waitForReadyRead(100);   // 最多约 1s，正常情况首轮即有数据
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

    QString command = msg.value("command").toString().trimmed().toLower();
    QString args = msg.value("args").toString().trimmed();
    QJsonObject response;

    // TaskController / TaskListModel / DownloadManager 都是 MainWindow 的 QObject 子对象。
    // 对 CLI 控制命令直接查找现有运行实例的对象，确保操作的是 GUI 正在展示的同一批任务，
    // 而不是启动第二套独立下载引擎。
    QObject* root = parent();
    TaskController* controller = root ? root->findChild<TaskController*>() : nullptr;
    TaskListModel* model = root ? root->findChild<TaskListModel*>() : nullptr;
    DownloadManager* manager = root ? root->findChild<DownloadManager*>() : nullptr;

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
        } else if (!m_addTask) {
            response["success"] = false;
            response["message"] = QStringLiteral("下载任务入口尚未初始化");
        } else {
            m_addTask(url, filename, dir, threads, queue, format);
            response["success"] = true;
            response["message"] = QStringLiteral("已添加下载: %1").arg(filename.isEmpty() ? url : filename);
        }
    } else if (command == "list") {
        if (model) {
            const QVector<TaskRow> tasks = model->tasks();
            QStringList lines;
            lines << QStringLiteral("当前 %1 个任务").arg(tasks.size());
            for (const TaskRow& row : tasks)
                lines << taskLine(row);
            response["success"] = true;
            response["message"] = lines.join(QLatin1Char('\n'));
        } else {
            const int n = m_taskCount ? m_taskCount() : 0;
            response["success"] = true;
            response["message"] = QStringLiteral("当前 %1 个任务").arg(n);
        }
    } else if (command == "start" || command == "pause") {
        if (!controller || !model) {
            response["success"] = false;
            response["message"] = QStringLiteral("任务控制器尚未初始化");
        } else if (args.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0) {
            const QVector<TaskRow> tasks = model->tasks();
            int changed = 0;
            for (const TaskRow& row : tasks) {
                if (command == "start" && (row.state == 0 || row.state == 2)) {
                    if (row.state == 2)
                        controller->resumeTaskById(row.id);
                    else
                        controller->startTaskById(row.id);
                    changed++;
                } else if (command == "pause" && row.state == 1) {
                    controller->pauseTaskById(row.id);
                    changed++;
                }
            }
            response["success"] = true;
            response["message"] = command == "start"
                ? QStringLiteral("已启动/恢复 %1 个任务").arg(changed)
                : QStringLiteral("已暂停 %1 个任务").arg(changed);
        } else {
            bool ok = false;
            int id = args.toInt(&ok);
            const QVector<TaskRow> tasks = model->tasks();
            const TaskRow* row = ok ? findTask(tasks, id) : nullptr;
            if (!row) {
                response["success"] = false;
                response["message"] = QStringLiteral("任务不存在: %1").arg(args);
            } else {
                if (command == "start") {
                    if (row->state == 2)
                        controller->resumeTaskById(id);
                    else
                        controller->startTaskById(id);
                    response["message"] = QStringLiteral("已启动/恢复任务 #%1").arg(id);
                } else {
                    controller->pauseTaskById(id);
                    response["message"] = QStringLiteral("已暂停任务 #%1").arg(id);
                }
                response["success"] = true;
            }
        }
    } else if (command == "cancel" || command == "remove") {
        bool ok = false;
        int id = args.toInt(&ok);
        const QVector<TaskRow> tasks = model ? model->tasks() : QVector<TaskRow>();
        if (!controller || !model) {
            response["success"] = false;
            response["message"] = QStringLiteral("任务控制器尚未初始化");
        } else if (!ok || !findTask(tasks, id)) {
            response["success"] = false;
            response["message"] = QStringLiteral("任务不存在: %1").arg(args);
        } else {
            if (command == "cancel")
                controller->cancelTaskById(id);
            else
                controller->removeTaskById(id);
            response["success"] = true;
            response["message"] = command == "cancel"
                ? QStringLiteral("已取消任务 #%1").arg(id)
                : QStringLiteral("已移除任务 #%1").arg(id);
        }
    } else if (command == "info") {
        bool ok = false;
        int id = args.toInt(&ok);
        const QVector<TaskRow> tasks = model ? model->tasks() : QVector<TaskRow>();
        const TaskRow* row = ok ? findTask(tasks, id) : nullptr;
        if (!row) {
            response["success"] = false;
            response["message"] = QStringLiteral("任务不存在: %1").arg(args);
        } else {
            response["success"] = true;
            response["message"] = QStringLiteral(
                "ID: %1\n状态: %2\n文件: %3\n已下载: %4 B\n总大小: %5 B\n速度: %6 B/s\nURL: %7")
                .arg(row->id)
                .arg(TaskListModel::stateText(row->state))
                .arg(row->fileName)
                .arg(row->downloaded)
                .arg(row->fileSize)
                .arg(row->speedBps)
                .arg(row->url);
        }
    } else if (command == "set-limit") {
        bool ok = false;
        int kbps = args.toInt(&ok);
        if (!ok || kbps < 0 || !manager) {
            response["success"] = false;
            response["message"] = QStringLiteral("用法: set-limit <KBPS>（0=不限速）");
        } else {
            manager->setMaxSpeed(kbps > 0 ? kbps * 1024 : 0);
            TorrentDownloader::setGlobalSpeedLimit(kbps);
            response["success"] = true;
            response["message"] = kbps == 0
                ? QStringLiteral("全局限速: 不限速")
                : QStringLiteral("全局限速: %1 KB/s").arg(kbps);
        }
    } else if (command == "help" || command == "?") {
        response["success"] = true;
        response["message"] = QStringLiteral(
            "命令:\n"
            "  add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]\n"
            "  list\n"
            "  start <id|all>\n"
            "  pause <id|all>\n"
            "  cancel <id>\n"
            "  remove <id>\n"
            "  info <id>\n"
            "  set-limit <KBPS>\n"
            "  help");
    } else if (command == "activate") {
        // 第二次启动程序时不再创建另一套下载引擎/IPC server，而是把现有主窗口恢复到前台。
        // IpcServer 的 parent 在 MainWindow 中创建，因此这里可安全恢复其顶层窗口。
        QWidget* window = qobject_cast<QWidget*>(parent());
        if (window) {
            window->showNormal();
            window->raise();
            window->activateWindow();
            response["success"] = true;
            response["message"] = QStringLiteral("已激活 IDM Next");
        } else {
            response["success"] = false;
            response["message"] = QStringLiteral("无法定位主窗口");
        }
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
    client->deleteLater();

    Log::info(QStringLiteral("IPC 请求: %1 → %2")
                  .arg(command, response.value("success").toBool() ? "成功" : "失败"));
}