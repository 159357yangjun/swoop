#include "task_controller.h"

#include <QDir>
#include <QUrl>

#include "download_manager.h"
#include "task_list_model.h"      // TaskRow / FileType / stateText
#include "category_filter_proxy.h" // detectFileType / fileTypeFolderName
#include "queue_manager.h"
#include "queue_scheduler.h"
#include "video_downloader.h"
#include "hls_downloader.h"
#include "torrent_downloader.h"
#include "settings.h"
#include "history_store.h"
#include "logger.h"

TaskController::TaskController(QObject* parent)
    : QObject(parent)
{
}

static QString taskQueueForId(TaskListModel* model, int id)
{
    if (!model)
        return QString();
    for (const TaskRow& row : model->tasks()) {
        if (row.id == id)
            return row.queue;
    }
    return QString();
}

// ── 媒体后端信号转发：统一汇入主窗口桥接槽 ─────────────
void TaskController::onMediaProgress(int taskId, qint64 downloaded, qint64 total, int speedBps)
{
    emit mediaProgress(taskId, downloaded, total, speedBps);
}
void TaskController::onMediaCompleted(int taskId, bool success, const QString& error)
{
    emit mediaCompleted(taskId, success, error);
}
void TaskController::onMediaStateChanged(int taskId, int state)
{
    emit mediaStateChanged(taskId, state);
}

// ── 统一的新增任务入口 ──────────────────────────────
// queue 为空：立即开始下载；否则加入指定队列，由 QueueScheduler 调度启动
int TaskController::internalAddTask(const QString& url, const QString& dir, const QString& name,
                                    int threads, const QString& queue, const QString& format,
                                    bool archiveByType)
{
    // 浏览器扩展等外部来源可能传入一个尚未存在的队列名：自动创建，
    // 否则该队列的 HTTP 任务会永远停在 state 0 等待调度（卡死）。
    if (!queue.isEmpty() && m_queueMgr && !m_queueMgr->contains(queue))
        m_queueMgr->addQueue(queue, 2);

    // 按文件类型自动归档（IDM 风格）：开启时把文件归入 saveDir/<分类>/ 子目录。
    // archiveByType=false 表示调用方已自行拼好归档子目录（如站点抓取器），不再二次归档。
    QString finalDir = dir;
    if (archiveByType && m_settings && m_settings->autoArchiveByType() && !dir.isEmpty()) {
        FileType ft = FileType::Other;
        if (VideoDownloader::isVideoUrl(url)
                || url.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)
                || url.endsWith(QStringLiteral(".mpd"),  Qt::CaseInsensitive)) {
            ft = FileType::Video;  // 视频站点 URL 无扩展名，单独判定
        } else {
            QString fname = name.isEmpty() ? QUrl(url).fileName() : name;
            if (fname.isEmpty())
                fname = url;
            ft = CategoryFilterProxy::detectFileType(fname);
        }
        finalDir = dir + QStringLiteral("/") + CategoryFilterProxy::fileTypeFolderName(ft);
    }
    QDir().mkpath(finalDir);

    // ── BT/磁力/FTP（magnet / .torrent / ftp://）：交给 aria2 后端（aria2 式 RPC 集成）
    if (TorrentDownloader::isAria2Url(url)) {
        int id = m_videoIdSeq++;
        auto* td = new TorrentDownloader(this);
        td->setTaskId(id);

        connect(td, &TorrentDownloader::progressChanged, this, &TaskController::onMediaProgress);
        connect(td, &TorrentDownloader::completed,      this, &TaskController::onMediaCompleted);
        connect(td, &TorrentDownloader::stateChanged,   this, &TaskController::onMediaStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 QueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_model->addTask(row);

        m_setSpeed(id, 0);
        m_setState(id, row.state);
        m_torrentTasks[id] = td;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings->maxThreads();
        req.isMagnet  = url.startsWith(QStringLiteral("magnet:"), Qt::CaseInsensitive);
        if (startNow) {
            td->start(req);
            if (m_cancelAutoPower) m_cancelAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 QueueScheduler 延迟启动
            if (queue.isEmpty()) m_queueScheduler->markDeferred(id);   // 等全局并发空位
        }

        if (m_syncStatus) m_syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加 aria2 任务 #%1: %2").arg(id).arg(url));
        return id;
    }

    // ── HLS（M3U8）：交给原生 HlsDownloader（自建解析 + ffmpeg 合并），不依赖 yt-dlp
    if (url.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
        int id = m_videoIdSeq++;  // 与视频/引擎任务区分的编号偏移
        auto* hd = new HlsDownloader(this);
        hd->setTaskId(id);

        connect(hd, &HlsDownloader::progressChanged, this, &TaskController::onMediaProgress);
        connect(hd, &HlsDownloader::completed,      this, &TaskController::onMediaCompleted);
        connect(hd, &HlsDownloader::stateChanged,   this, &TaskController::onMediaStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 QueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_model->addTask(row);

        m_setSpeed(id, 0);
        m_setState(id, row.state);
        m_hlsTasks[id] = hd;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings->maxThreads();
        req.isMagnet  = false;
        if (!format.isEmpty())
            req.extra.insert(QStringLiteral("format"), format);
        if (startNow) {
            hd->start(req);
            if (m_cancelAutoPower) m_cancelAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 QueueScheduler 延迟启动
            if (queue.isEmpty()) m_queueScheduler->markDeferred(id);   // 等全局并发空位
        }

        if (m_syncStatus) m_syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加 HLS 任务 #%1: %2").arg(id).arg(url));
        return id;
    }

    // ── 视频站点（YouTube / B 站 / …）/ DASH：交给 yt-dlp 后端，不走分段 HTTP 引擎
    bool isVideo = VideoDownloader::isVideoUrl(url)
                   || url.endsWith(QStringLiteral(".mpd"),  Qt::CaseInsensitive);
    if (isVideo) {
        int id = m_videoIdSeq++;
        auto* vd = new VideoDownloader(this);
        vd->setTaskId(id);

        // 信号接入统一的任务进度/完成/状态处理（与 C 引擎任务共用同一套 UI 更新逻辑）
        connect(vd, &VideoDownloader::progressChanged, this, &TaskController::onMediaProgress);
        connect(vd, &VideoDownloader::completed,      this, &TaskController::onMediaCompleted);
        connect(vd, &VideoDownloader::stateChanged,   this, &TaskController::onMediaStateChanged);

        TaskRow row;
        row.id = id;
        row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
        if (row.fileName.isEmpty())
            row.fileName = url;
        row.url       = url;
        row.fileSize  = -1;
        row.downloaded = 0;
        row.speedBps  = 0;
        row.lastConnection = QDateTime::currentDateTime();
        row.queue     = queue;
        // 无队列才立即开始；且无队列同样受「同时下载的任务数」全局上限约束——
        // 满额时留在 state 0，由 QueueScheduler 有空位再自动拉起（与有队列的任务一致）。
        const bool startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot();
        row.state     = startNow ? 1 : 0;
        row.statusText = TaskListModel::stateText(row.state);
        m_model->addTask(row);

        m_setSpeed(id, 0);
        m_setState(id, row.state);
        m_videoTasks[id] = vd;

        DownloadRequest req;
        req.url       = url;
        req.savePath  = finalDir;
        req.fileName  = name;
        req.threadCount = threads > 0 ? threads : m_settings->maxThreads();
        req.isMagnet  = false;
        if (!format.isEmpty())
            req.extra.insert(QStringLiteral("format"), format);
        if (startNow) {
            vd->start(req);
            if (m_cancelAutoPower) m_cancelAutoPower();
        } else {
            m_pendingStream[id] = req;   // 交给 QueueScheduler 延迟启动
            if (queue.isEmpty()) m_queueScheduler->markDeferred(id);   // 等全局并发空位
        }

        if (m_syncStatus) m_syncStatus();
        HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
        Log::info(QStringLiteral("添加视频任务 #%1 (yt-dlp): %2").arg(id).arg(url));
        return id;
    }

    int id = m_manager->addTask(url, finalDir, name, threads > 0 ? threads : m_settings->maxThreads());
    if (id <= 0)
        return -1;

    TaskRow row;
    row.id = id;
    row.fileName = name.isEmpty() ? QUrl(url).fileName() : name;
    if (row.fileName.isEmpty())
        row.fileName = url;
    row.url            = url;
    row.fileSize       = -1;
    row.downloaded     = 0;
    row.speedBps       = 0;
    row.lastConnection = QDateTime::currentDateTime();
    row.queue          = queue;
    // 与媒体任务一致：无队列也受「同时下载的任务数」约束，满额则排队等待
    const bool startNow = queue.isEmpty() && m_queueScheduler->hasFreeSlot();
    if (startNow) {
        m_manager->startTask(id);
        row.state = 1;  // 下载中
    } else {
        row.state = 0;  // 等待队列调度（已入队列，或已达同时下载上限）
        if (queue.isEmpty()) m_queueScheduler->markDeferred(id);   // 等全局并发空位
    }
    row.statusText = TaskListModel::stateText(row.state);
    m_model->addTask(row);

    m_setSpeed(id, 0);
    m_setState(id, row.state);
    if (m_cancelAutoPower) m_cancelAutoPower();
    if (m_syncStatus) m_syncStatus();
    HistoryStore::instance().recordAdded(url, row.fileName, finalDir);
    Log::info(QStringLiteral("添加任务 #%1 (队列:%2): %3")
                  .arg(id).arg(queue.isEmpty() ? QStringLiteral("无") : queue).arg(url));
    return id;
}

// ── 后端无关的下载控制：按 taskId 自动分派到 C 引擎或 yt-dlp 视频后端 ──
void TaskController::startTaskById(int id)
{
    m_queueScheduler->clearDeferred(id);  // 无论手动还是调度器放行，都算已经脱离等待
    // 加入队列的媒体任务在添加时未立即启动，缓存了 DownloadRequest；
    // 此处由队列调度器放行后真正拉起后端（视频/HLS/BT）。
    if (m_pendingStream.contains(id)) {
        DownloadRequest req = m_pendingStream.take(id);
        bool started = false;
        if (isTorrentTask(id)) {
            if (auto* td = m_torrentTasks.value(id)) { td->start(req); started = true; }
        } else if (isHlsTask(id)) {
            if (auto* hd = m_hlsTasks.value(id)) { hd->start(req); started = true; }
        } else if (isVideoTask(id)) {
            if (auto* vd = m_videoTasks.value(id)) { vd->start(req); started = true; }
        }
        if (!started) {
            m_setState(id, 4);
            m_model->updateStatus(id, 4);
            if (m_syncStatus) m_syncStatus();
            Log::warn(QStringLiteral("待启动媒体任务 #%1 缺少对应后端实例").arg(id));
            return;
        }
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isStreamTask(id)) return;  // 视频/HLS 任务在添加时已由后端启动
    m_manager->startTask(id);
    m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
    m_model->updateStatus(id, 1);
    if (m_syncStatus) m_syncStatus();
}

void TaskController::pauseTaskById(int id)
{
    // 媒体任务可能还只是“等待队列/等待并发空位”，此时后端从未 start()。
    // 对未启动后端直接 pause() 没有意义；只冻结状态并从无队列 deferred 集合移除。
    if (m_pendingStream.contains(id)) {
        m_queueScheduler->clearDeferred(id);
        m_setState(id, 2);
        m_model->updateStatus(id, 2);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->pause();
        m_setState(id, 2);
        m_model->updateStatus(id, 2);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->pause();
        m_setState(id, 2);
        m_model->updateStatus(id, 2);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->pause();
        m_setState(id, 2);
        m_model->updateStatus(id, 2);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    m_manager->pauseTask(id);
    m_setState(id, 2);
    m_model->updateStatus(id, 2);
}

void TaskController::resumeTaskById(int id)
{
    // 尚未真正 start() 的媒体任务恢复时不能调用 backend->resume()：后端还没有 URL/保存路径。
    // 恢复为等待态并重新进入调度；无队列且当前有全局空位时可立即启动。
    if (m_pendingStream.contains(id)) {
        const QString queue = taskQueueForId(m_model, id);
        if (queue.isEmpty() && m_queueScheduler->hasFreeSlot()) {
            startTaskById(id);
            return;
        }
        m_setState(id, 0);
        m_model->updateStatus(id, 0);
        if (queue.isEmpty())
            m_queueScheduler->markDeferred(id);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->resume();
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->resume();
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->resume();
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    m_manager->resumeTask(id);
    m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
    m_model->updateStatus(id, 1);
    if (m_syncStatus) m_syncStatus();
}

void TaskController::cancelTaskById(int id)
{
    // 未启动媒体任务没有外部进程/RPC 可取消；保留 DownloadRequest，允许“重新开始”复用。
    if (m_pendingStream.contains(id)) {
        m_queueScheduler->clearDeferred(id);
        m_setState(id, 5);
        m_model->updateStatus(id, 5);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->cancel();
        m_setState(id, 5);
        m_model->updateStatus(id, 5);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->cancel();
        m_setState(id, 5);
        m_model->updateStatus(id, 5);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->cancel();
        m_setState(id, 5);
        m_model->updateStatus(id, 5);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    m_manager->cancelTask(id);
    m_setState(id, 5);
    m_model->updateStatus(id, 5);
    if (m_syncStatus) m_syncStatus();
}

void TaskController::restartTaskById(int id)
{
    // 对从未启动过的媒体任务，“重新开始”与重新放回调度队列等价。
    if (m_pendingStream.contains(id)) {
        resumeTaskById(id);
        return;
    }
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) hd->resume();  // HLS 无断点续传，重新下载
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) vd->resume();  // yt-dlp 无断点续传，重新下载
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) td->resume();  // aria2 重新拉起任务
        m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
        m_model->updateStatus(id, 1);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    m_manager->restartTask(id);
    m_model->updateProgress(id, 0, 0);
    m_setSpeed(id, 0);
    m_setState(id, 1); if (m_cancelAutoPower) m_cancelAutoPower();
    if (m_syncStatus) m_syncStatus();
    Log::info(QStringLiteral("任务 #%1 已重新开始").arg(id));
}

void TaskController::removeTaskById(int id)
{
    m_pendingStream.remove(id);  // 清理可能仍在等待队列调度的媒体任务请求
    // 任务没了，定时/重复设置也要一起清掉，否则 ScheduleService 里会残留
    // 指向已删除 id 的条目（到期后 startTaskById 作用在不存在的任务上）。
    if (m_removeSchedule) m_removeSchedule(id);
    m_queueScheduler->clearDeferred(id);  // 已被并发上限暂缓的任务删除后不必再等空位
    if (isHlsTask(id)) {
        if (auto* hd = m_hlsTasks.value(id)) {
            hd->cancel();
            hd->deleteLater();
            m_hlsTasks.remove(id);
        }
        m_model->removeTask(id);
        m_clearSpeed(id);
        m_clearState(id);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isVideoTask(id)) {
        if (auto* vd = m_videoTasks.value(id)) {
            vd->cancel();
            vd->deleteLater();
            m_videoTasks.remove(id);
        }
        m_model->removeTask(id);
        m_clearSpeed(id);
        m_clearState(id);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    if (isTorrentTask(id)) {
        if (auto* td = m_torrentTasks.value(id)) {
            td->cancel();
            td->deleteLater();
            m_torrentTasks.remove(id);
        }
        m_model->removeTask(id);
        m_clearSpeed(id);
        m_clearState(id);
        if (m_syncStatus) m_syncStatus();
        return;
    }
    m_manager->removeTask(id);
    m_model->removeTask(id);
    m_clearSpeed(id);
    m_clearState(id);
    if (m_syncStatus) m_syncStatus();
}