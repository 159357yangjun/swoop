#include "cli_forward.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
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

/* 幂等命令：处理两次与处理一次的结果相同，所以「送出去了但没听到回话」可以再问一次。
 * 为什么需要再问 —— CI 第 13 步那次红（90ac237）的现场把因果钉死了：副实例报
 * 「送出 50 字节、3 秒内没有回话」，而**赢家自己的日志里写着
 * 「IPC server 已启动」→「IPC 请求: activate → 成功」**——请求被处理了，
 * 只是回话落在副实例已经放弃之后。副实例就此 exit 1，用户看到的是
 * 「GUI 正在启动时再点图标，什么都没发生」。
 * 反面是 add：同一次「处理了但没赶上回话」如果发生在 add 上，重发就是下两遍，
 * 所以带副作用的命令一律只发一次。 */
static bool isIdempotentCommand(const QString& command)
{
    return command == QLatin1String("activate")
        || command == QLatin1String("help")
        || command == QLatin1String("?")
        || command == QLatin1String("list")
        || command == QLatin1String("info");
}

ForwardResult forwardToGui(const QStringList& cliArgs, bool printResponse,
                           const QString& serverName)
{
    const QString pipe = serverName.isEmpty() ? ipcServerName() : serverName;
    const QByteArray data = QJsonDocument(buildRequest(cliArgs)).toJson(QJsonDocument::Compact);
    const QString command = cliArgs.isEmpty() ? QStringLiteral("add") : cliArgs.first();
    const int attempts = isIdempotentCommand(command) ? 2 : 1;

    QString lastWhy;
    for (int attempt = 0; attempt < attempts; ++attempt) {
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
            lastWhy = QStringLiteral("已连上运行中的实例，但请求没写进去（%1）")
                          .arg(socket.errorString());
            socket.disconnectFromServer();
            reportFailure(lastWhy, printResponse);
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

        /* 3 秒内一个字节都没回来。把现场攒着：幂等命令先重来一次，
         * 全部用完还在「没回话」才报出去。 */
        lastWhy = QStringLiteral("已连上运行中的实例并送出 %1 字节，%2 秒内没有回话"
                                 "（管道状态 %3，错误：%4；共尝试 %5 次）；这条命令**没有**被确认执行")
                      .arg(data.size())
                      .arg(3)
                      .arg(static_cast<int>(socket.state()))
                      .arg(socket.errorString().isEmpty()
                               ? QStringLiteral("无") : socket.errorString())
                      .arg(attempt + 1);
        socket.disconnectFromServer();
    }

    reportFailure(lastWhy, printResponse);
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
