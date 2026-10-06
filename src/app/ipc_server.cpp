#include "ipc_server.h"
#include "cli_forward.h"
#include "logger.h"
#include "socket_drain.h"
#include "task_controller.h"
#include "task_list_model.h"
#include "download_manager.h"
#include "torrent_downloader.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTimer>
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
    std::function<void(const QString&, const QString&, const QString&, int, const QString&,
                       const QString&, const QString&)> cb)
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

IpcServer::DrainStat IpcServer::s_drainStat;

IpcServer::DrainStat IpcServer::drainStat()
{
    return s_drainStat;
}

/* ── 读阶段：非阻塞，readyRead 信号驱动（缺陷登记 §7-56）──────────────
 * 旧形状＝在 GUI 线程上 waitForReadyRead(100)×2 起步的有界循环。一条"连上一个字节
 * 都不发"的空转/半开连接 ⇒ 服务端白占主线程 ≥200ms；扩展 checkConnection 轮询 +
 * 被杀掉的副实例都是这种客户端 ⇒ **用户每点一次插件，界面就卡一下**。
 * 新形状：首检无字节 ⇒ 挂一次性 readyRead 立刻返回；宽限语义原样保留——
 * "已收到过半条消息才继续等下一片"由 graceTimer 承担，与旧 `i > 1` 的意图一致。
 * 分片累积、完整 JSON 判定、对端断开判定全部不变；只是不再在槽函数里睡。 */
namespace {

struct PendingRead : public QObject {
    QLocalSocket* socket = nullptr;
    QByteArray data;
    QTimer* graceTimer = nullptr;       // 首包宽限：收到半条后续 500ms；一个字节都没有 ⇒ 判空转
    IpcServer* server = nullptr;        // 交付目标（构造时注入，不靠 parent() 链反推）
    bool handoff = false;               // 防重入：done() 只交付一次

    PendingRead(QLocalSocket* s, IpcServer* srv) : QObject(s), socket(s), server(srv) {}

    void done() {
        if (handoff) return;
        handoff = true;
        socket->disconnect(this);       // 断开挂在这条 socket 上、以 this 为 context 的全部连接
        if (graceTimer) graceTimer->stop();
        server->handleClientData(socket, data);
        // 生命周期：PendingRead 是 socket 的子对象；handleClientData 走 deleteLater，
        // socket 销毁时本对象一起销毁。done() 之后不得再碰 this 的任何成员。
    }

    void pump() {
        data.append(socket->readAll());
        if (!data.isEmpty() && QJsonDocument::fromJson(data).isObject()) {
            done();
            return;
        }
        if (socket->state() != QLocalSocket::ConnectedState) {
            // 对端已走：收过东西却凑不成一条命令 ⇒ 静默销毁（没人听，别回话）。
            if (handoff) return;
            handoff = true;
            socket->disconnect(this);
            if (graceTimer) graceTimer->stop();
            socket->deleteLater();
            return;
        }
        if (!data.isEmpty() && !graceTimer->isActive())
            graceTimer->start(500);     // 半条消息：给它继续到的机会（比旧的 9×100ms 上限更宽）
    }
};

} // namespace

void IpcServer::onNewConnection()
{
    // newConnection 可能一次积累多个客户端；全部取出，避免短时间浏览器/CLI 连发时
    // 有连接滞留到下一次信号才被处理。
    QElapsedTimer drain;
    drain.start();
    int taken = 0;
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* client = m_server->nextPendingConnection();
        if (client) {
            ++taken;
            handleClient(client);
        }
    }
    // 测具读数：只记，不影响上面任何一步控制流
    s_drainStat.connections = taken;
    s_drainStat.lastMs = drain.elapsed();
    if (s_drainStat.maxMs < s_drainStat.lastMs)
        s_drainStat.maxMs = s_drainStat.lastMs;
}

void IpcServer::handleClient(QLocalSocket* client)
{
    auto* pr = new PendingRead(client, this);
    pr->graceTimer = new QTimer(pr);
    pr->graceTimer->setSingleShot(true);
    QObject::connect(pr->graceTimer, &QTimer::timeout, pr, [pr] { pr->done(); });
    QObject::connect(client, &QLocalSocket::readyRead, pr, [pr] { pr->pump(); });
    /* 空转判定的证人换成 disconnected，不是"连上且安静"：
     * Windows 命名管道上，对端进程**先写后连**（worker 线程 waitForConnected 返回前就
     * write+flush 是常态）⇒ 字节可能在 newConnection 发出之前就已可读，而 readyRead
     * 在 emit 那一刻消费掉它 —— 之后不会再有第二发。第一版把"已连接但首检无字节"
     * 当成空转立即交付，实测 ack=0/50：真请求被当哑巴连接处理掉了。
     * 真正的空转客户端（checkConnection 轮询、被杀的副实例）的特征是**它会关**：
     * 对端 disconnect ⇒ 永远等不到字节 ⇒ 在这里交付空 data，走"不支持的操作"回话。
     * 半条消息的续宽限仍在 pump() 里（收到过东西才挂 graceTimer）。 */
    QObject::connect(client, &QLocalSocket::disconnected, pr, [pr] {
        if (pr->data.isEmpty()) pr->done();   // 一个字节都没来过 ⇒ 空转坐实
        else pr->pump();                       // 凑齐就走正常路，凑不齐 done 时 fromJson 兜底
    });
    // enterMs 的旧口径＝进入 handleClient；异步化后取"首检时刻"，与 t0 同用墙钟，语义不漂。
    client->setProperty("_idmEnterMs", QDateTime::currentMSecsSinceEpoch());
    if (client->bytesAvailable() > 0) {
        pr->pump();                      // 常见路径：单次写入整体送达，当场就能凑齐
        return;
    }
    // 首检无字节：**不等、不睡**（旧形状在这里 waitForReadyRead(100)×2 起步，白占 GUI 线程
    // ≥200ms）。字节若已在路上会由 readyRead 补一发；真的没有 ⇒ 只有 disconnected 会来收尸。
}

void IpcServer::handleClientData(QLocalSocket* client, const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject msg = doc.object();

    /* 延迟取证（每一步红过一次都拿不到原因，就是因为缺这两个数）：
     *   排队 = 进入本函数时刻 − 客户端 t0  → 大 ⇒ 事件循环没空理这条连接
     *   处理 = 回话写完时刻 − 进入本函数时刻 → 大 ⇒ 命令本身在 GUI 线程上慢
     * 老客户端不带 t0 时记 -1，不参与统计。 */
    const double t0 = msg.value(QStringLiteral("t0")).toDouble(0.0);
    const qint64 enterMs = client->property("_idmEnterMs").toLongLong();
    const qint64 queueMs = (t0 > 0.0) ? (enterMs - static_cast<qint64>(t0)) : -1;
    QElapsedTimer handledClock;
    handledClock.start();

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
        QString hlsVar   = msg.value("hls_variant").toString();  // HLS 变体 BANDWIDTH 串（空=最高档）
        int    threads   = msg.value("threads").toInt();

        if (url.isEmpty()) {
            response["success"] = false;
            response["message"] = QStringLiteral("URL 为空");
        } else if (!m_addTask) {
            response["success"] = false;
            response["message"] = QStringLiteral("下载任务入口尚未初始化");
        } else {
            m_addTask(url, filename, dir, threads, queue, format, hlsVar);
            /* 用户从浏览器卡片上选中的画质**必须留一行可复算的读数**：
             * 「IPC 请求: add → 成功」那条通用回话行只有命令名与时序，看不出用户选了哪一档，
             * 所以真站 E2E 只能靠"产物字节数 == 低档拼接"反推（间接、且要求下载真跑完）。
             * 这行把投递时刻收到的原始字段写出来 ⇒ 点击→IPC 这一段当场可核，不必等落盘。
             * ⚠ 只加日志，不改任何控制流；空值也照印（默认最高档同样要有证人）。 */
            Log::info(QStringLiteral("IPC add 参数: url=%1 file=%2 dir=%3 threads=%4 queue=%5 format=%6 hlsVariant=%7")
                          .arg(url, filename, dir).arg(threads).arg(queue, format, hlsVar));
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
        /* ⚠️ 这个分支必须是**幂等**的：同一个请求被投两次（客户端超时后重发、
         * 用户连点两次图标、副实例没等到回话就退出）不得产生第二份副作用。
         * 这条不是约定俗成的推理，是被断言的：`IDM_ACTIVATE_PROBE`
         * （tools/ui_snapshot.cpp）连投两次、各自读到 success，再断言
         * 任务列表行数与引擎任务数不变、窗口状态不变。
         * 想在这里加"顺手做点别的"（建任务、弹通知、记一次数、发一个信号）之前先看那条测试，
         * 加了就会红 —— 那正是它存在的理由。 */
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
    /* 发回话前不猜"对端会读完"：Windows 上 QLocalSocket 的写出是异步的
     * （QWindowsPipeWriter），所以关掉之前要把队列推空 —— 这是**保守做法**，
     * 但注意它**没有被本地证明过**：IDM_FLUSH_PROBE 拿 2MB 回话、对端晚 300ms 读做 A/B，
     * "只等一次"与"推到空"两边都把 2MB 发完了（销毁前 bytesToWrite()=0），
     * 本机造不出"返回时仍有待发字节"。它的判据是下面这个 `送达` 字段：
     * CI 出现 送达=0 才坐实"回话没推完就被销毁"这条。
     * 与之并列、且**本地已测到**的第二条机制在上面那个读循环里（1090ms → 217ms）。 */
    const bool clientStillHere = (client->state() == QLocalSocket::ConnectedState);
    client->write(respData);
    client->flush();
    const bool delivered = drainSocketBeforeClose(client, 2000);
    client->disconnectFromServer();
    client->deleteLater();

    /* 取证入口（CI 第 13 步抓进 annotation，绿的那些轮也记）：
     * 排队大 ⇒ 事件循环被别的东西占住；处理大 ⇒ 命令本身在 GUI 线程上慢；
     * 送达=0 ⇒ 回话没推出去（就是上面这个 bug 的现行信号）。 */
    Log::info(QStringLiteral("IPC 请求: %1 → %2（排队 %3ms，处理 %4ms，送达=%5，回话时客户端=%6）")
                  .arg(command,
                       response.value("success").toBool() ? QStringLiteral("成功") : QStringLiteral("失败"))
                  .arg(queueMs)
                  .arg(handledClock.elapsed())
                  .arg(delivered ? 1 : 0)
                  .arg(clientStillHere ? QStringLiteral("在") : QStringLiteral("已走")));
}