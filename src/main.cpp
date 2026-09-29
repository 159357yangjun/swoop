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
#include "cli_forward.h"
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
// 转发本身搬到了 cli_forward.{h,cpp} —— 放在 main.cpp 里它是 static，
// ui_snapshot 的探针 link 不到，也就没法对「失败时说不说得出原因」这条
// 定出断言；搬出去之后那条红才测得出来。
#ifdef Q_OS_WIN
static const wchar_t* GUI_MUTEX_NAME = L"Local\\IDMNext.Gui.SingleInstance.v1";

/* 等「另一个正在启动的 GUI」就绪的预算，两个 waitForGui 调用点共用。
   数字来自 CI 第 11 步自己报回来的实测计时：同一份代码两轮分别收敛于 5.7s 与 8.7s
   —— 共享 runner 上 Qt 应用冷启动本身就有 ±3s 抖动。这段时间里副实例拿到的
   一直是 NotConnected（赢家的 listen 已推迟到事件循环第一轮，见 main_window.cpp），
   所以预算必须盖住**赢家整个启动**，而不是只盖住某一次 IPC 往返。
   原来写死 10s，对着 8.7s 只剩 1.3s 余量 → 慢一点就误判"没有 GUI"并退出 1，
   那就是 108abde / 4cd1f57 两次间歇性红。上限受 CI 观察窗（80×250ms=20s）约束，
   取 15s：既盖住抖动，又不会拖到观察窗之外、换成一种更难查的红。 */
static const DWORD GUI_READY_WAIT_MS = 15000;

/* 把这个预算和 CI 观察窗的契约钉在编译期：第 11 步只观察 80×250ms = 20s，
   副实例必须在那之前给出退出码；否则断言会退化成
   「只有一个进程退出」这种看不出根因的红。留 4s 给进程自身启动开销。 */
static_assert(GUI_READY_WAIT_MS + 4000 < 20000,
              "GUI_READY_WAIT_MS 必须留在 CI 第 11 步的 20s 观察窗之内");
#endif

#ifdef Q_OS_WIN
static bool guiMutexExists()
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, GUI_MUTEX_NAME);
    if (!h)
        return false;
    CloseHandle(h);
    return true;
}
#endif

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

        ForwardResult forwarded = forwardToGui(cliArgs);
        if (forwarded != ForwardResult::NotConnected)
            return forwarded == ForwardResult::Success ? 0 : 1;

#ifdef Q_OS_WIN
        // GUI 可能刚拿到 mutex、尚未建立 local socket。此时等待它就绪，不能启动独立引擎
        // 与正在启动的 GUI 同时读写任务状态。
        if (guiMutexExists()) {
            forwarded = waitForGui(cliArgs, true, GUI_READY_WAIT_MS);
            if (forwarded != ForwardResult::NotConnected)
                return forwarded == ForwardResult::Success ? 0 : 1;
            QTextStream(stderr) << QStringLiteral("IDM Next GUI 正在启动，但 IPC 未能就绪。\n");
            return 1;
        }
#endif

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

    const QStringList forwardArgs = !extUrl.isEmpty()
        ? (QStringList() << QStringLiteral("add") << extUrl)
        : (QStringList() << QStringLiteral("activate"));

#ifdef Q_OS_WIN
    // 真正的原子单实例门闩必须在 dlmgr_init / QApplication / MainWindow 之前建立。
    // 仅靠“先连一次 QLocalSocket”存在双进程同时连不上、随后都初始化 GUI 的竞态。
    HANDLE guiMutex = CreateMutexW(nullptr, FALSE, GUI_MUTEX_NAME);
    if (!guiMutex) {
        MessageBoxW(nullptr, L"无法创建 IDM Next 单实例锁。", L"IDM Next", MB_OK | MB_ICONERROR);
        return 1;
    }
    const bool anotherGuiStarting = (GetLastError() == ERROR_ALREADY_EXISTS);

    if (anotherGuiStarting) {
        ForwardResult forwarded = waitForGui(forwardArgs, false, GUI_READY_WAIT_MS);
        CloseHandle(guiMutex);
        if (forwarded != ForwardResult::NotConnected)
            return forwarded == ForwardResult::Success ? 0 : 1;
        /* 换成写日志 + stderr。原来的 MB_OK | MB_ICONERROR 模态框有两个毛病：
         *   · 无人值守的场合（CI、服务会话、开机自启）没人点确定，进程就挂在那里不退出，
         *     调用方看到的不是「失败」而是「永远等不到」；
         *   · 对用户也没有信息量——双击图标两次后弹一句"IPC 未能就绪"，他只能点确定。
         * 注意：这条分支不是 CI 那次红的原因（它报的是退出码 1，说明走的是 Failure，
         * 也就是连上了但没回话，见上面 waitForReadyRead 的注释）；这里改的是
         * 「压根连不上」那条路，退出码保持非 0，让调用方照样能察觉。 */
        Log::error(QStringLiteral("已有 IDM Next 正在启动，但 IPC 在 %1 秒内未就绪；本次启动放弃。")
                       .arg(GUI_READY_WAIT_MS / 1000));
        QTextStream(stderr) << QStringLiteral("IDM Next: 另一个实例正在启动，但 IPC 未能就绪。\n");
        return 1;
    }
#else
    void* guiMutex = nullptr;
#endif

    // 兼容升级场景：旧版本 GUI 可能已运行但没有上述命名 mutex。
    ForwardResult forwarded = forwardToGui(forwardArgs, false);
    if (forwarded != ForwardResult::NotConnected) {
#ifdef Q_OS_WIN
        CloseHandle(guiMutex);
#endif
        return forwarded == ForwardResult::Success ? 0 : 1;
    }

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
#ifdef Q_OS_WIN
    CloseHandle(guiMutex);
#endif
    return rc;
}
