#include <QApplication>
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QFile>
#include <QTranslator>
#include <QHash>
#include <QTextStream>

#include <cstdio>
#ifdef Q_OS_WIN
#  include <windows.h>
#endif

#include "main_window.h"
#include "cli_app.h"
#include "settings.h"
#include "download_core.h"
#include "logger.h"
#include "button_translator.h"   // 简体中文标准按钮翻译（与截图工具共用，见该头文件注释）

#ifndef IDM_NEXT_VERSION
#  define IDM_NEXT_VERSION "0.0.0"
#endif

// ── IPC 单实例通信 ──────────────────────────────
// 当 GUI 已在运行时，CLI/第二个 GUI 实例通过 local socket 把命令转发给现有 GUI，
// 避免重复 dlmgr_init 和第二个实例抢占 IPC server。
static const char* IPC_SERVER_NAME = "idm-next-ipc";

enum class ForwardResult {
    NotConnected,
    Success,
    Failure
};

/**
 * 尝试连接正在运行的 GUI 实例。
 * NotConnected 表示当前没有 GUI，可继续启动本实例；Success/Failure 都表示已存在 GUI，
 * 调用方不应再启动第二套引擎。CLI 可据 Success/Failure 返回正确进程退出码。
 */
static ForwardResult tryForwardToGui(const QStringList& cliArgs, bool printResponse = true)
{
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(IPC_SERVER_NAME));
    if (!socket.waitForConnected(500))
        return ForwardResult::NotConnected;

    // 构造 JSON 消息
    QJsonObject obj;
    obj["command"] = "add";

    // 解析 CLI 参数：add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]
    for (int i = 0; i < cliArgs.size(); ++i) {
        const QString& a = cliArgs[i];
        if (a == QStringLiteral("add"))
            continue;
        if (a == QStringLiteral("--dir") && i + 1 < cliArgs.size())
            obj["dir"] = cliArgs[++i];
        else if (a == QStringLiteral("--name") && i + 1 < cliArgs.size())
            obj["filename"] = cliArgs[++i];
        else if (a == QStringLiteral("--threads") && i + 1 < cliArgs.size())
            obj["threads"] = cliArgs[++i].toInt();
        else if (a == QStringLiteral("--format") && i + 1 < cliArgs.size())
            obj["format"] = cliArgs[++i];
        else if (a == QStringLiteral("--no-wait"))
            obj["no_wait"] = true;
        else if (!a.startsWith(QLatin1String("--")))
            obj["url"] = a;
    }

    // 如果命令不是 add（比如 list/start/pause/activate），直接按首参数转发
    if (!cliArgs.isEmpty() && cliArgs.first() != QStringLiteral("add")) {
        obj["command"] = cliArgs.first();
        if (cliArgs.size() > 1)
            obj["args"] = cliArgs.mid(1).join(QLatin1Char(' '));
    }

    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    socket.write(data);
    socket.flush();
    if (!socket.waitForBytesWritten(2000)) {
        socket.disconnectFromServer();
        return ForwardResult::Failure;
    }

    if (socket.waitForReadyRead(3000)) {
        QByteArray response = socket.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(response);
        QJsonObject respObj = doc.object();
        const bool success = respObj.value("success").toBool(false);
        QString message = respObj.value("message").toString();
        if (printResponse && !message.isEmpty()) {
            QTextStream stream(success ? stdout : stderr);
            stream << message << "\n";
            stream.flush();
        }
        socket.disconnectFromServer();
        return success ? ForwardResult::Success : ForwardResult::Failure;
    }

    socket.disconnectFromServer();
    return ForwardResult::Failure;
}

// WIN32 子系统下（GUI 可执行文件）无默认控制台；CLI 模式需用 AllocConsole 分配一个，
// 并把标准流重定向到该控制台，使 CliApp / IPC 转发结果的 stdout 输出可见。
static void ensureConsole()
{
#ifdef Q_OS_WIN
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    if (freopen("CONOUT$", "w", stdout) == nullptr) { /* 忽略 */ }
    if (freopen("CONOUT$", "w", stderr) == nullptr) { /* 忽略 */ }
    if (freopen("CONIN$",  "r", stdin)  == nullptr) { /* 忽略 */ }
#endif
}

int main(int argc, char* argv[])
{
    bool cliMode = false;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--cli") || a == QStringLiteral("-c")) {
            cliMode = true;
            break;
        }
    }

    if (cliMode) {
        // WIN32 子系统不会自动得到 C runtime 控制台；必须在“尝试 IPC 转发”之前就绑定。
        ensureConsole();

        QStringList cliArgs;
        for (int i = 1; i < argc; ++i) {
            QString a = QString::fromLocal8Bit(argv[i]);
            if (a == QStringLiteral("--cli") || a == QStringLiteral("-c"))
                continue;
            cliArgs.append(a);
        }

        ForwardResult forwarded = tryForwardToGui(cliArgs);
        if (forwarded != ForwardResult::NotConnected)
            return forwarded == ForwardResult::Success ? 0 : 1;

        // 无 GUI 在运行，独立 CLI 模式
        dlmgr_init(nullptr);
        Settings settings;
        settings.load();
        settings.applyToEngine();

        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("IDM Next"));
        QCoreApplication::setApplicationVersion(QString::fromLatin1(IDM_NEXT_VERSION));

        cliArgs.prepend(QString::fromLocal8Bit(argv[0]));

        CliApp cli;
        int rc = cli.run(cliArgs);
        dlmgr_destroy();
        return rc;
    }

    // ── GUI 模式 ──
    QString extUrl;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QLatin1String("--")))
            continue;
        bool looksUrl = a.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
                     || a.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)
                     || a.startsWith(QStringLiteral("ftp://"), Qt::CaseInsensitive)
                     || a.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive)
                     || QFile::exists(a);
        if (looksUrl)
            extUrl = a;
    }

    // 已存在 GUI：带 URL 时转发添加任务；普通二次启动则只激活已有窗口。
    // Success/Failure 都必须退出，避免命令失败时反而启动第二套引擎并抢 IPC。
    ForwardResult forwarded = !extUrl.isEmpty()
        ? tryForwardToGui(QStringList() << QStringLiteral("add") << extUrl, false)
        : tryForwardToGui(QStringList() << QStringLiteral("activate"), false);
    if (forwarded != ForwardResult::NotConnected)
        return forwarded == ForwardResult::Success ? 0 : 1;

    dlmgr_init(nullptr);
    Settings settings;
    settings.load();
    settings.applyToEngine();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("IDM Next"));
    QApplication::setApplicationVersion(QString::fromLatin1(IDM_NEXT_VERSION));

    installZhCnTranslator();

    Log::info(QStringLiteral("IDM Next 启动（GUI 模式）"));

    MainWindow window;
    window.show();

    if (!extUrl.isEmpty())
        window.enqueueUrl(extUrl);

    int rc = app.exec();
    dlmgr_destroy();
    return rc;
}