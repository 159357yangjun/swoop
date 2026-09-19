#include "download_manager.h"
#include "download_core.h"  // 纯 C 引擎 API

#include <QDebug>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QTimer>

class DownloadManager::Impl {
public:
    DownloadManager* q;
    // 进度聚合缓冲：taskId -> (已下字节, 速度)。只保留每个任务的最新一帧。
    QHash<int, QPair<qint64, int>> pendingProgress;
    /* ⚠️ 这个哈希是被两个线程同时碰的：写入来自引擎的 worker 线程
     * （progressCb → forwardProgress），读出/清空来自 GUI 线程的 QTimer
     * （flushProgress）。QHash 不是线程安全的容器，无锁读写会直接破坏它的桶数组
     * —— 表现为偶发的堆损坏崩溃，而不是可预期的错误。必须加锁。 */
    QMutex progressMutex;
    QTimer* flushTimer = nullptr;   // 固定 ~10Hz 批量刷新定时器
};

DownloadManager::DownloadManager(QObject* parent)
    : QObject(parent), d(new Impl{this})
{
    // 引擎的初始化/销毁统一由 main() 负责（GUI 与 CLI 共用），此处不再重复

    // 进度聚合：以 100ms（10Hz）为窗口批量刷新，将成百上千次逐块信号
    // 合并为固定频率刷新，杜绝 GUI 事件循环被进度信号淹没。
    d->flushTimer = new QTimer(this);
    d->flushTimer->setInterval(100);
    connect(d->flushTimer, &QTimer::timeout, this, &DownloadManager::flushProgress);
    d->flushTimer->start();
}

DownloadManager::~DownloadManager()
{
    // 先停刷新定时器：析构体跑完后 Impl（含那把锁）就没了，此时再有
    // timeout 触发 flushProgress 会踩到已释放的成员。
    if (d->flushTimer)
        d->flushTimer->stop();
    delete d;
}

int DownloadManager::addTask(const QString& url, const QString& saveDir,
                             const QString& fileName, int threadCount)
{
    QByteArray urlUtf8  = url.toUtf8();
    QByteArray dirUtf8  = saveDir.toUtf8();
    QByteArray nameUtf8 = fileName.toUtf8();

    // 注意参数顺序：url, save_dir, filename, thread_count, 回调...
    // thread_count: 0=使用引擎默认配置（DownloadConfig.thread_pool_size）
    int id = dlmgr_add(urlUtf8.constData(),
                       dirUtf8.constData(),
                       fileName.isEmpty() ? nullptr : nameUtf8.constData(),
                       threadCount,
                       &DownloadManager::progressCb,
                       this,
                       &DownloadManager::completeCb,
                       this);
    return id;
}

void DownloadManager::startTask(int taskId)
{
    dlmgr_start(taskId);
    emit taskStateChanged(taskId, 1);  // 1 = RUNNING
}

void DownloadManager::pauseTask(int taskId)
{
    dlmgr_pause(taskId);
    emit taskStateChanged(taskId, 2);  // 2 = PAUSED
}

void DownloadManager::resumeTask(int taskId)
{
    // 引擎未提供独立的 resume API：dlmgr_start 兼具「开始」与「从暂停恢复」语义
    dlmgr_start(taskId);
    emit taskStateChanged(taskId, 1);  // 1 = RUNNING
}

void DownloadManager::cancelTask(int taskId)
{
    dlmgr_cancel(taskId);
    emit taskStateChanged(taskId, 5);  // 5 = CANCELLED
}

void DownloadManager::restartTask(int taskId)
{
    // 引擎 dlmgr_restart：重置分片、删除临时文件、重新探测文件大小并开始下载
    dlmgr_restart(taskId);
    emit taskStateChanged(taskId, 1);  // 1 = RUNNING
}

void DownloadManager::removeTask(int taskId)
{
    dlmgr_remove(taskId);
}

void DownloadManager::rebindTask(int taskId)
{
    // dlmgr_load_state 恢复的任务未注册 Qt 回调，此处补绑以便实时刷新
    dlmgr_set_callbacks(taskId,
                         &DownloadManager::progressCb, this,
                         &DownloadManager::completeCb, this);
}

bool DownloadManager::saveState(const QString& path)
{
    return dlmgr_save_state(path.toUtf8().constData()) == 0;
}

bool DownloadManager::loadState(const QString& path)
{
    return dlmgr_load_state(path.toUtf8().constData()) == 0;
}

void DownloadManager::setMaxThreads(int n)
{
    DownloadConfig cfg = dlmgr_get_config();
    cfg.thread_pool_size = n;
    dlmgr_set_config(&cfg);
}

void DownloadManager::setMaxSpeed(int bytesPerSec)
{
    dlmgr_set_speed_limit(bytesPerSec);
}

// ── 回调桥接 ─────────────────────────────────────
void DownloadManager::progressCb(int task_id, int64_t downloaded, int64_t total, int speed, void* ud)
{
    auto* self = static_cast<DownloadManager*>(ud);
    if (self) self->forwardProgress(task_id, downloaded, total, speed);
}

void DownloadManager::completeCb(int task_id, int success, void* ud)
{
    auto* self = static_cast<DownloadManager*>(ud);
    if (self) self->forwardComplete(task_id, success);
}

void DownloadManager::forwardProgress(int task_id, int64_t downloaded, int64_t total, int speed)
{
    // 由 worker 线程调用：与 flushProgress() 争同一个哈希，必须持锁
    QMutexLocker locker(&d->progressMutex);
    // 只保留每个任务的最新一帧，丢弃中间的冗余帧（sampling：以最新值代表窗口）
    d->pendingProgress[task_id] = qMakePair(downloaded, speed);
}

void DownloadManager::flushProgress()
{
    QHash<int, QPair<qint64, int>> batch;
    {
        // 快照后清空，避免在 emit 过程中被 worker 线程再次写入造成重入
        QMutexLocker locker(&d->progressMutex);
        if (d->pendingProgress.isEmpty())
            return;
        batch = d->pendingProgress;
        d->pendingProgress.clear();
    }

    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
        emit taskProgress(it.key(), it.value().first, 0, it.value().second);
    }
}

void DownloadManager::forwardComplete(int task_id, int success)
{
    emit taskCompleted(task_id, success != 0, QString());
    // 同步转发终态：3=COMPLETED 4=FAILED（与 download_core.h TaskStatus 对齐）
    emit taskStateChanged(task_id, success != 0 ? 3 : 4);
}
