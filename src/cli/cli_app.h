#ifndef CLI_APP_H
#define CLI_APP_H

#include <QStringList>

// 命令行模式：直接调用纯 C 下载引擎（dlmgr_* API），不依赖 Qt GUI。
// 支持两种用法：
//   单命令： idm-next --cli add <url> [--dir DIR] [--name NAME] [--threads N] [--no-wait]
//   交互式： idm-next --cli            （进入 REPL，输入 help 查看命令）
class CliApp {
public:
    CliApp();

    // 返回进程退出码（0=成功）
    int run(const QStringList& args);

    // 引擎 C 回调函数（供 cli_app.cpp 中的全局 C 函数转发）
    void onProgress(int taskId, long long downloaded, long long total, int speed);
    void onComplete(int taskId, int success);

private:
    void printUsage() const;
    int  execCommand(const QStringList& args);
    int  cmdAdd(const QStringList& args);
    int  cmdList();
    int  cmdStart(const QStringList& args);
    int  cmdPause(const QStringList& args);
    int  cmdCancel(const QStringList& args);
    int  cmdRemove(const QStringList& args);
    int  cmdInfo(const QStringList& args);
    int  cmdSetLimit(const QStringList& args);
    void printTask(int id);
    void waitForComplete(int id);   // 轮询直到终态，实时打印进度
    void error(const QString& msg);

    bool m_waiting = false;
};

#endif // CLI_APP_H
