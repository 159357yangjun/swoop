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

/**
 * 尝试连接正在运行的 GUI 实例
 * @param cliArgs 要转发的命令参数
 * @param printResponse 是否把 GUI 返回消息打印到 stdout（GUI 启动转发时关闭）
 * @return true=已连接并转发成功（应直接退出），false=无 GUI 在运行（继续当前实例）
 */
static bool tryForwardToGui(const QStringList& cliArgs, bool printResponse = true)
{
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(IPC_SERVER_NAME));
    if (!socket.waitForConnected(500))
        return false;  // 无 GUI 在运行

    // 构造 JSON 消息
    QJsonObject obj;
    obj["command"] = "add";

    // 解析 CLI 参数：add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]
    for (int i = 0; i < cliArgs.size(); ++i) {
        const QString& a = cliArgs[i];
        if (a == QStringLiteral("add"))
            continue;  // 跳过 add 命令本身
        if (a == QStringLiteral("--dir") && i + 1 < cliArgs.size())
            obj["dir"] = cliArgs[++i];
        else if (a == QStringLiteral("--name") && i + 1 < cliArgs.size())
            obj["filename"] = cliArgs[++i];
        else if (a == QStringLiteral("--threads") && i + 1 < cliArgs.size())
            obj["threads"] = cliArgs[++i].toInt();
        else if (a == QStringLiteral("--format") && i + 1 < cliArgs.size())
            obj["format"] = cliArgs[++i];   // 视频画质（yt-dlp -f 选择串）
        else if (a == QStringLiteral("--no-wait"))
            obj["no_wait"] = true;
        else if (!a.startsWith(QLatin1String("--")))
            obj["url"] = a;  // 非 flag 的参数 = URL
    }

    // 如果命令不是 add（比如 list/activate），直接按首参数转发
    if (!cliArgs.isEmpty() && cliArgs.first() != QStringLiteral("add")) {
        obj["command"] = cliArgs.first();
        if (cliArgs.size() > 1)
            obj["args"] = cliArgs.mid(1).join(QLatin1Char(' '));
    }

    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    socket.write(data);
    socket.flush();
    socket.waitForBytesWritten(2000);

    // 读取 GUI 的响应
    if (socket.waitForReadyRead(3000)) {
        QByteArray response = socket.readAll();
        QJsonDocument doc = QJsonDocument::fromJson(response);
        QJsonObject respObj = doc.object();
        QString message = respObj.value("message").toString();
        if (printResponse && !message.isEmpty()) {
            QTextStream out(stdout);
            out << message << "\n";
            out.flush();
        }
        socket.disconnectFromServer();
        return true;
    }

    socket.disconnectFromServer();
    return true;  // 即使没收到响应也算转发成功（GUI 可能正在处理）
}

// WIN32 子系统下（GUI 可执行文件）无默认控制台；CLI 模式需用 AllocConsole 分配一个，
// 并把标准流重定向到该控制台，使 CliApp / IPC 转发结果的 stdout 输出可见。
static void ensureConsole()
{
#ifdef Q_OS_WIN
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    // 释放并重新绑定标准流到控制台设备
    if (freopen("CONOUT$", "w", stdout) == nullptr) { /* 忽略 */ }
    if (freopen("CONOUT$", "w", stderr) == nullptr) { /* 忽略 */ }
    if (freopen("CONIN$",  "r", stdin)  == nullptr) { /* 忽略 */ }
#endif
}

int main(int argc, char* argv[])
{
    // ── 解析运行模式 ──
    bool cliMode = false;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--cli") || a == QStringLiteral("-c")) {
            cliMode = true;
            break;
        }
    }

    if (cliMode) {
        // WIN32 子系统不会自动得到 C runtime 控制台；必须在“尝试 IPC 转发”之前就绑定，
        // 否则 GUI 已运行时的 list/help 等转发结果会写到不可见的 stdout。
        ensureConsole();

        // 提取 CLI 实际参数（去掉 --cli/-c 标志）
        QStringList cliArgs;
        for (int i = 1; i < argc; ++i) {
            QString a = QString::fromLocal8Bit(argv[i]);
            if (a == QStringLiteral("--cli") || a == QStringLiteral("-c"))
                continue;
            cliArgs.append(a);
        }

        // 尝试转发到正在运行的 GUI 实例
        // 如果是 list/start/pause 等查询命令，也转发
        if (tryForwardToGui(cliArgs)) {
            return 0;  // 已转发给 GUI，无需启动引擎
        }

        // 无 GUI 在运行，独立 CLI 模式
        dlmgr_init(nullptr);
        Settings settings;
        settings.load();
        settings.applyToEngine();

        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName(QStringLiteral("IDM Next"));
        QCoreApplication::setApplicationVersion(QString::fromLatin1(IDM_NEXT_VERSION));

        cliArgs.prepend(QString::fromLocal8Bit(argv[0]));  // 保留程序名

        CliApp cli;
        int rc = cli.run(cliArgs);
        dlmgr_destroy();
        return rc;
    }

    // ── GUI 模式 ──
    // 收集位置参数中的 URL（供 magnet / 浏览器协议关联、文件拖拽等）
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
    // 这样不会出现两个 GUI 同时运行、后启动实例 removeServer() 抢掉 IPC 的情况。
    if (!extUrl.isEmpty()) {
        if (tryForwardToGui(QStringList() << QStringLiteral("add") << extUrl, false))
            return 0;
    } else {
        if (tryForwardToGui(QStringList() << QStringLiteral("activate"), false))
            return 0;
    }

    dlmgr_init(nullptr);
    Settings settings;
    settings.load();
    settings.applyToEngine();

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("IDM Next"));
    QApplication::setApplicationVersion(QString::fromLatin1(IDM_NEXT_VERSION));

    // 安装简体中文按钮翻译（OK→确定、Cancel→取消…），保持界面语言一致
    installZhCnTranslator();

    Log::info(QStringLiteral("IDM Next 启动（GUI 模式）"));

    MainWindow window;
    window.show();

    // 本实例直接处理位置参数中的 URL（magnet / 直链 / 本地种子）
    if (!extUrl.isEmpty())
        window.enqueueUrl(extUrl);

    int rc = app.exec();
    dlmgr_destroy();
    return rc;
}