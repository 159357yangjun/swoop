#include "off_thread.h"

#include <QThread>

void runOffThread(QObject* context, std::function<void()> job, std::function<void()> done)
{
    if (!context) {
        /* 没有可以投递回来的对象 ⇒ 退化成同步执行。
         * 调用方都要传 context；这条只是防止误用把 job 丢进一条没人收尸的线程。 */
        if (job) job();
        if (done) done();
        return;
    }

    auto* worker = QThread::create([context, job, done] {
        if (job) job();
        /* 回到 context 所在线程再做 UI / 回调。
         * context 若已析构，Qt 会丢弃这条投递事件 —— 不会调到死对象上。 */
        QMetaObject::invokeMethod(
            context,
            [done] { if (done) done(); },
            Qt::QueuedConnection);
    });

    /* 线程跑完自己退出；deleteLater 由 finished 触发，挂在 worker 自身上，
     * 不依赖 context 的存活（context 先走也不会漏删）。 */
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
