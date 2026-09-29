#include "cli_app.h"
#include "download_core.h"
#include "settings.h"

#include <QTextStream>
#include <QDir>

#include <cstdio>
#include <thread>
#include <chrono>

// 全局指针，供 C 回调函数转发到当前 CliApp 实例
static CliApp* g_cli = nullptr;

extern "C" {
    static void cliProgressCb(int task_id, int64_t downloaded, int64_t total, int speed, void* ud) {
        (void)ud;
        if (g_cli) g_cli->onProgress(task_id, downloaded, total, speed);
    }
    static void cliCompleteCb(int task_id, int success, void* ud) {
        (void)ud;
        if (g_cli) g_cli->onComplete(task_id, success);
    }
}

static QString stateStr(int s) {
    switch (s) {
        case 0: return QStringLiteral("等待");
        case 1: return QStringLiteral("下载中");
        case 2: return QStringLiteral("已暂停");
        case 3: return QStringLiteral("已完成");
        case 4: return QStringLiteral("失败");
        case 5: return QStringLiteral("已取消");
        default: return QStringLiteral("未知");
    }
}

CliApp::CliApp() {}

int CliApp::run(const QStringList& args)
{
    g_cli = this;

    QStringList a = args;
    if (!a.isEmpty()) a.removeFirst();  // 去掉程序名

    if (!a.isEmpty()) {
        // 单命令模式：直接执行并返回
        return execCommand(a);
    }

    // 交互模式
    QTextStream out(stdout);
    QTextStream in(stdin);
    out << QStringLiteral("IDM Next 命令行模式 — 输入 help 查看命令，quit 退出\n");
    out.flush();

    QString line;
    while (in.readLineInto(&line)) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line == QStringLiteral("quit") || line == QStringLiteral("exit"))
            break;
        execCommand(line.split(QLatin1Char(' '), Qt::SkipEmptyParts));
        out.flush();
    }
    return 0;
}

int CliApp::execCommand(const QStringList& args)
{
    if (args.isEmpty()) { printUsage(); return 1; }
    const QString& cmd = args.first();
    if (cmd == QStringLiteral("add"))    return cmdAdd(args);
    if (cmd == QStringLiteral("list"))   return cmdList();
    if (cmd == QStringLiteral("start"))  return cmdStart(args);
    if (cmd == QStringLiteral("pause"))  return cmdPause(args);
    if (cmd == QStringLiteral("cancel")) return cmdCancel(args);
    if (cmd == QStringLiteral("remove")) return cmdRemove(args);
    if (cmd == QStringLiteral("info"))   return cmdInfo(args);
    if (cmd == QStringLiteral("set-limit")) return cmdSetLimit(args);
    if (cmd == QStringLiteral("help") || cmd == QStringLiteral("?")) { printUsage(); return 0; }
    error(QStringLiteral("未知命令: %1").arg(cmd));
    printUsage();
    return 1;
}

int CliApp::cmdAdd(const QStringList& args)
{
    if (args.size() < 2) {
        error(QStringLiteral("用法: add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]"));
        return 1;
    }
    QString url  = args[1];
    QString dir  = Settings::defaultDownloadDir();
    QString name;
    int     threads = 0;       // 0 = 引擎默认
    bool    wait    = true;

    for (int i = 2; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--dir") && i + 1 < args.size())
            dir = args[++i];
        else if (args[i] == QStringLiteral("--name") && i + 1 < args.size())
            name = args[++i];
        else if (args[i] == QStringLiteral("--threads") && i + 1 < args.size())
            threads = args[++i].toInt();
        else if (args[i] == QStringLiteral("--no-wait"))
            wait = false;
        else {
            error(QStringLiteral("未知参数: %1").arg(args[i]));
            return 1;
        }
    }

    int id = dlmgr_add(url.toUtf8().constData(),
                       dir.toUtf8().constData(),
                       name.isEmpty() ? nullptr : name.toUtf8().constData(),
                       threads,
                       cliProgressCb, nullptr,
                       cliCompleteCb, nullptr);
    if (id <= 0) {
        error(QStringLiteral("添加任务失败（链接无效或引擎繁忙）"));
        return 1;
    }
    dlmgr_start(id);

    QTextStream out(stdout);
    out << QStringLiteral("任务已创建 #%1 -> %2\n").arg(id).arg(dir);
    out.flush();

    if (wait)
        waitForComplete(id);
    return 0;
}

int CliApp::cmdList()
{
    int ids[128];
    int n = dlmgr_list(ids, 128);
    QTextStream out(stdout);
    out << QStringLiteral("ID\t状态\t已下载/总大小\t速度\t文件\n");
    for (int i = 0; i < n; ++i)
        printTask(ids[i]);
    out.flush();
    return 0;
}

int CliApp::cmdStart(const QStringList& args)
{
    if (args.size() >= 2 && args[1] == QStringLiteral("all")) {
        int k = dlmgr_start_all();
        const int deferred = dlmgr_deferred_count();
        QTextStream out(stdout);
        /* 报清楚「这次真的起了几个」和「还有几个在等空位」——
         * 上限生效以后 start all 不再等于全部开跑，只说 k 会让人以为剩下的丢了。 */
        out << QStringLiteral("已启动 %1 个任务%2\n")
                   .arg(k)
                   .arg(deferred > 0
                            ? QStringLiteral("，另有 %1 个在等「同时下载的任务数」空位").arg(deferred)
                            : QString());
        out.flush();
        return 0;
    }
    if (args.size() < 2) { error(QStringLiteral("用法: start <id|all>")); return 1; }
    dlmgr_start(args[1].toInt());
    return 0;
}

int CliApp::cmdPause(const QStringList& args)
{
    if (args.size() >= 2 && args[1] == QStringLiteral("all")) {
        int k = dlmgr_stop_all();
        QTextStream out(stdout);
        out << QStringLiteral("已暂停 %1 个任务\n").arg(k);
        out.flush();
        return 0;
    }
    if (args.size() < 2) { error(QStringLiteral("用法: pause <id|all>")); return 1; }
    dlmgr_pause(args[1].toInt());
    return 0;
}

int CliApp::cmdCancel(const QStringList& args)
{
    if (args.size() < 2) { error(QStringLiteral("用法: cancel <id>")); return 1; }
    dlmgr_cancel(args[1].toInt());
    return 0;
}

int CliApp::cmdRemove(const QStringList& args)
{
    if (args.size() < 2) { error(QStringLiteral("用法: remove <id>")); return 1; }
    dlmgr_remove(args[1].toInt());
    return 0;
}

int CliApp::cmdInfo(const QStringList& args)
{
    if (args.size() < 2) { error(QStringLiteral("用法: info <id>")); return 1; }
    int id = args[1].toInt();
    TaskInfo info;
    if (dlmgr_get_task_info(id, &info) != 0) {
        error(QStringLiteral("任务不存在: %1").arg(id));
        return 1;
    }
    QTextStream out(stdout);
    out << QStringLiteral("ID:     %1\n").arg(id)
        << QStringLiteral("文件:   %1\n").arg(QString::fromUtf8(info.filename))
        << QStringLiteral("大小:   %1 B\n").arg(info.file_size)
        << QStringLiteral("已下载: %1 B\n").arg(info.downloaded)
        << QStringLiteral("速度:   %1 B/s\n").arg(info.speed_bps)
        << QStringLiteral("状态:   %1\n").arg(stateStr(info.status))
        << QStringLiteral("剩余:   %1 s\n").arg(info.eta_sec);
    out.flush();
    return 0;
}

int CliApp::cmdSetLimit(const QStringList& args)
{
    if (args.size() < 2) { error(QStringLiteral("用法: set-limit <KBPS>（0=不限速）")); return 1; }
    int kbps = args[1].toInt();
    dlmgr_set_speed_limit(kbps > 0 ? kbps * 1024 : 0);
    QTextStream out(stdout);
    out << QStringLiteral("全局限速: %1\n")
           .arg(kbps == 0 ? QStringLiteral("不限速")
                          : QStringLiteral("%1 KB/s").arg(kbps));
    out.flush();
    return 0;
}

void CliApp::printTask(int id)
{
    TaskInfo info;
    if (dlmgr_get_task_info(id, &info) != 0) return;
    QTextStream out(stdout);
    double dMB = info.downloaded / 1048576.0;
    double tMB = info.file_size  / 1048576.0;
    out << QStringLiteral("%1\t%2\t%3/%4 MB\t%5 KB/s\t%6\n")
           .arg(id)
           .arg(stateStr(info.status))
           .arg(dMB, 0, 'f', 1)
           .arg(tMB, 0, 'f', 1)
           .arg(info.speed_bps / 1024)
           .arg(QString::fromUtf8(info.filename));
}

void CliApp::waitForComplete(int id)
{
    m_waiting = true;
    QTextStream out(stdout);
    while (m_waiting) {
        TaskInfo info;
        if (dlmgr_get_task_info(id, &info) != 0) break;
        if (info.status == 3 || info.status == 4 || info.status == 5) break;

        out << QStringLiteral("\r#%1  %2/%3 MB  %4 KB/s")
               .arg(id)
               .arg(info.downloaded / 1048576.0, 0, 'f', 1)
               .arg(info.file_size  / 1048576.0, 0, 'f', 1)
               .arg(info.speed_bps / 1024);
        out.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    out << QStringLiteral("\n");
    out.flush();
    m_waiting = false;
}

void CliApp::onProgress(int, long long, long long, int)
{
    // 进度由 waitForComplete 轮询显示；此处预留（可用于调试）
}

void CliApp::onComplete(int taskId, int success)
{
    QTextStream out(stdout);
    out << QStringLiteral("\n任务 #%1 %2\n")
           .arg(taskId)
           .arg(success ? QStringLiteral("完成") : QStringLiteral("失败"));
    out.flush();
    m_waiting = false;  // 唤醒 waitForComplete 循环
}

void CliApp::error(const QString& msg)
{
    QTextStream err(stderr);
    err << QStringLiteral("错误: %1\n").arg(msg);
    err.flush();
}

void CliApp::printUsage() const
{
    QTextStream out(stdout);
    out << QStringLiteral(
        "命令:\n"
        "  add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]  添加并下载\n"
        "  list                                                 列出所有任务\n"
        "  start <id|all>                                       开始任务\n"
        "  pause <id|all>                                       暂停任务\n"
        "  cancel <id>                                          取消任务（删临时文件）\n"
        "  remove <id>                                          移除任务（从列表删除）\n"
        "  info <id>                                            查看任务详情\n"
        "  set-limit <KBPS>                                     全局限速（0=不限速）\n"
        "  help                                                 显示本帮助\n");
    out.flush();
}
