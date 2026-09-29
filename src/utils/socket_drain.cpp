#include "socket_drain.h"

#include <QLocalSocket>
#include <QElapsedTimer>

bool drainSocketBeforeClose(QLocalSocket* socket, int budgetMs)
{
    if (!socket)
        return false;

    QElapsedTimer spent;
    spent.start();
    while (socket->bytesToWrite() > 0 && spent.elapsed() < budgetMs) {
        /* 对端已经不在了就别把预算耗满：这种管道再等也不会有人收，
           而且这一步跑在 GUI 线程上，白等 = 界面卡住。 */
        if (socket->state() != QLocalSocket::ConnectedState)
            break;
        socket->waitForBytesWritten(100);
    }
    return socket->bytesToWrite() == 0;
}
