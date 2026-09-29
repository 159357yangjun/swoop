#pragma once

#include <QString>
#include <QStringList>

/* 单实例转发的唯一入口：CLI（--cli …）与「再点一次图标」起来的副实例都走这里，
   把命令交给已经在跑的 GUI，绝不另起第二套下载引擎。 */
enum class ForwardResult {
    NotConnected,   // 没有 GUI 在听 —— 调用方可以自己起一套
    Success,        // GUI 收到并确认了
    Failure         // GUI 收到了，但这一条没谈成（回话失败 / 没回话 / 写不进去）
};

/* IPC 管道名。以前 main.cpp 和 ipc_server.cpp 各写一遍字面量：
   两边不同步就是「服务在听 A、客户端在连 B」，现象与真正的 bug 一模一样，
   却永远不会报错。 */
QString ipcServerName();

/* 一次往返。serverName 做成参数是给探针用的 —— 它必须连自己的假服务，
   不能去碰用户此刻正在跑的那个实例。 */
ForwardResult forwardToGui(const QStringList& cliArgs, bool printResponse = true,
                           const QString& serverName = QString());

/* 等「已经拿到 mutex、但 IPC 还没起来」的 GUI 就绪：每 100ms 试一次，
   只在压根连不上时继续等；一旦连上过就立刻把结论交回去。 */
ForwardResult waitForGui(const QStringList& cliArgs, bool printResponse,
                         qint64 timeoutMs, const QString& serverName = QString());
