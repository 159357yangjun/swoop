#include "cli_forward.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QDateTime>
#include <QTextStream>
#include <QThread>

#include <cstdio>

#include "logger.h"

QString ipcServerName()
{
    return QStringLiteral("idm-next-ipc");
}

/* 把 CLI 参数拼成转发给 GUI 的 JSON。规则与 CliApp 的命令行一致：
   add 是默认命令，其余命令原样按首参数转发。 */
static QJsonObject buildRequest(const QStringList& cliArgs)
{
    QJsonObject obj;
    obj[QStringLiteral("command")] = QStringLiteral("add");
    /* 客户端送出的墙钟时刻：服务端把它原样算成「排队多久」。
     * 两个进程没法共享 QElapsedTimer，但同一台机器上墙钟是同一把尺 ——
     * CI 那个「连上了、3 秒没回话」的红需要这个数才能定性到
     * 「是事件循环没空理这条连接」还是「服务端处理本身就慢」。 */
    obj[QStringLiteral("t0")] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());

    // add <url> [--dir DIR] [--name NAME] [--threads N] [--format F] [--no-wait]
    for (int i = 0; i < cliArgs.size(); ++i) {
        const QString& a = cliArgs[i];
        if (a == QStringLiteral("add"))
            continue;
        if (a == QStringLiteral("--dir") && i + 1 < cliArgs.size())
            obj[QStringLiteral("dir")] = cliArgs[++i];
        else if (a == QStringLiteral("--name") && i + 1 < cliArgs.size())
            obj[QStringLiteral("filename")] = cliArgs[++i];
        else if (a == QStringLiteral("--threads") && i + 1 < cliArgs.size())
            obj[QStringLiteral("threads")] = cliArgs[++i].toInt();
        else if (a == QStringLiteral("--format") && i + 1 < cliArgs.size())
            obj[QStringLiteral("format")] = cliArgs[++i];
        else if (a == QStringLiteral("--no-wait"))
            obj[QStringLiteral("no_wait")] = true;
        else if (!a.startsWith(QLatin1String("--")))
            obj[QStringLiteral("url")] = a;
    }

    if (!cliArgs.isEmpty() && cliArgs.first() != QStringLiteral("add")) {
        obj[QStringLiteral("command")] = cliArgs.first();
        if (cliArgs.size() > 1)
            obj[QStringLiteral("args")] = cliArgs.mid(1).join(QLatin1Char(' '));
    }
    return obj;
}

/* 失败时到底要说清楚是**哪一种**失败。
   以前三条不同的路（写不进去 / 没回话 / 对方回了 success=false）都只留下一句
   「退出码 1」，而 stderr 是空的 —— CI 第 11 步的 remote-help 就是这样：
   annotations 里 stdout/stderr 双双「<空>」，于是这一个间歇性红在本会话里
   查不下去，只能猜。现在每条路都带上「连上了没、送出多少字节、等了多久、
   管道状态与错误串」，既给用户看，也给日志和 CI 取证用。 */
static void reportFailure(const QString& reason, bool printToStderr)
{
    Log::warn(QStringLiteral("IPC 转发失败: %1").arg(reason));
    if (printToStderr) {
        QTextStream stream(stderr);
        stream << QStringLiteral("IDM Next: %1\n").arg(reason);
        stream.flush();
    }
}

/* 一次往返，**绝不重发**。
 * 为什么不重发：CI 现场证明「服务端处理了、回话却落在客户端 3 秒死线之后」是真实存在的
 * 时序（服务端日志里的「排队 Nms」就是为它加的）。那种情况下重发等于让同一件事做两遍 ——
 * add 会下两份任务。这里曾经加过「幂等命令再问一次」，已撤回（CI 观察窗也从 9s 退回 5s）：
 * 一是它没治好那个红（带重发的那一轮第 13 步照样失败），二是它会把真问题盖住。
 * 而现场数字（排队 51ms、处理 0ms，客户端却什么都没收到）指向"回话没推出去"而不是"慢"——
 * 那条假设的判据在 utils/socket_drain.h 与日志字段 `送达=0/1` 上，本机还证不也证不伪。
 * 总之：慢也好、丢也好，都要照实报出来，而不是被第二次尝试掩盖。 */
ForwardResult forwardToGui(const QStringList& cliArgs, bool printResponse,
                           const QString& serverName)
{
    const QString pipe = serverName.isEmpty() ? ipcServerName() : serverName;
    const QByteArray data = QJsonDocument(buildRequest(cliArgs)).toJson(QJsonDocument::Compact);

    QLocalSocket socket;
    socket.connectToServer(pipe);
    if (!socket.waitForConnected(500)) {
        /* 压根没人监听 —— 这不是失败，是「本机还没有 GUI」，
         * 调用方要据此自己起一套，所以一个字都不报。 */
        return ForwardResult::NotConnected;
    }

    socket.write(data);
    socket.flush();
    if (!socket.waitForBytesWritten(2000)) {
        const QString why = QStringLiteral("已连上运行中的实例，但请求没写进去（%1）")
                                .arg(socket.errorString());
        socket.disconnectFromServer();
        reportFailure(why, printResponse);
        return ForwardResult::Failure;
    }

    if (socket.waitForReadyRead(3000)) {
        const QByteArray response = socket.readAll();
        const QJsonObject respObj = QJsonDocument::fromJson(response).object();
        const bool success = respObj.value(QStringLiteral("success")).toBool(false);
        const QString message = respObj.value(QStringLiteral("message")).toString();
        if (printResponse && !message.isEmpty()) {
            QTextStream stream(success ? stdout : stderr);
            stream << message << "\n";
            stream.flush();
        }
        socket.disconnectFromServer();
        if (!success) {
            if (message.isEmpty()) {
                reportFailure(QStringLiteral("运行中的实例回了话，但结果是失败，而且没给原因"),
                              printResponse);
            }
            return ForwardResult::Failure;
        }
        return ForwardResult::Success;
    }

    /* 3 秒内一个字节都没回来：把现场报出来，别再让它只是一句「exit 1」。 */
    const QString why = QStringLiteral("已连上运行中的实例并送出 %1 字节，3 秒内没有回话"
                                       "（管道状态 %2，错误：%3）；这条命令**没有**被确认执行。"
                                       "运行中的实例可能正忙，其日志里的「排队 Nms」能定到哪一步")
                            .arg(data.size())
                            .arg(static_cast<int>(socket.state()))
                            .arg(socket.errorString().isEmpty()
                                     ? QStringLiteral("无") : socket.errorString());
    socket.disconnectFromServer();
    reportFailure(why, printResponse);
    return ForwardResult::Failure;
}

ForwardResult waitForGui(const QStringList& cliArgs, bool printResponse,
                         qint64 timeoutMs, const QString& serverName)
{
    const qint64 stepMs = 100;
    qint64 waited = 0;
    for (;;) {
        ForwardResult r = forwardToGui(cliArgs, printResponse, serverName);
        if (r != ForwardResult::NotConnected)
            return r;
        if (waited >= timeoutMs)
            return ForwardResult::NotConnected;
        QThread::msleep(static_cast<unsigned long>(stepMs));
        waited += stepMs;
    }
}
