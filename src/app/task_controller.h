#ifndef IDM_APP_TASK_CONTROLLER_H
#define IDM_APP_TASK_CONTROLLER_H

#include <QObject>
#include <QHash>
#include <QMap>
#include <QString>
#include <functional>

#include "idownloader.h"   // DownloadRequest

class DownloadManager;
class TaskListModel;
class QueueManager;
class QueueScheduler;
class ScheduleService;
class VideoDownloader;
class HlsDownloader;
class TorrentDownloader;
class Settings;

// ── L2 应用编排组件：任务控制（媒体后端实例表 + 任务 CRUD/分派） ─────────
// 从 MainWindow 抽取。拥有视频/HLS/BT 三类媒体后端实例表（m_videoTasks 等）、
// 按队列延迟启动缓存（m_pendingStream）、以及「统一新增任务入口」internalAddTask
// 与后端无关的下载控制动词（start/pause/resume/cancel/restart/remove）。
//
// 对 MainWindow 只读经注入指针（引擎/模型/队列/调度器/设置），状态缓存经回调写入
// （m_states / m_speeds 仍由 MainWindow 持有，引擎信号桥接与状态栏聚合不动）；
// 媒体后端（Video/Hls/Torrent）的进度/完成/状态信号经本组件转发槽 → 信号，
// 由 MainWindow 连接到其原有桥接槽（onTaskProgress/Completed/StateChanged），
// 与 C 引擎信号会聚到同一套 UI 更新逻辑，行为完全不变。
class TaskController : public QObject {
    Q_OBJECT
public:
    explicit TaskController(QObject* parent = nullptr);

    // ── 依赖注入（MainWindow 在构造时接好） ──
    void setModel(TaskListModel* model)            { m_model = model; }
    void setManager(DownloadManager* mgr)           { m_manager = mgr; }
    void setQueueManager(QueueManager* qm)          { m_queueMgr = qm; }
    void setQueueScheduler(QueueScheduler* qs)      { m_queueScheduler = qs; }
    void setSettings(Settings* s)                   { m_settings = s; }

    // 任务状态 / 速度缓存写入回调（m_states / m_speeds 仍由 MainWindow 持有）
    void setStateSink(std::function<void(int, int)> set, std::function<void(int)> clear) {
        m_setState = std::move(set); m_clearState = std::move(clear);
    }
    void setSpeedSink(std::function<void(int, qint64)> set, std::function<void(int)> clear) {
        m_setSpeed = std::move(set); m_clearSpeed = std::move(clear);
    }
    // 删除任务时清除其定时/重复设置（ScheduleService 在 MainWindow 构造后期才创建，
    // 故用回调延迟注入，内部调用处做了空判）
    void setRemoveScheduleCallback(std::function<void(int)> cb) { m_removeSchedule = std::move(cb); }
    // 有新下载开始/状态变更时：中止待定关机、重算状态栏
    void setCancelAutoPower(std::function<void()> cb) { m_cancelAutoPower = std::move(cb); }
    void setSyncStatus(std::function<void()> cb)     { m_syncStatus = std::move(cb); }

    // ── 媒体后端实例表登记簿 ──
    bool isVideoTask(int id) const   { return m_videoTasks.contains(id); }
    bool isHlsTask(int id) const     { return m_hlsTasks.contains(id); }
    bool isTorrentTask(int id) const { return m_torrentTasks.contains(id); }
    bool isStreamTask(int id) const  { return isVideoTask(id) || isHlsTask(id) || isTorrentTask(id); }

    // ── 统一新增任务入口 ──
    // queue 为空：立即开始下载；否则加入指定队列，由 QueueScheduler 调度启动
    int internalAddTask(const QString& url, const QString& dir, const QString& name,
                        int threads, const QString& queue, const QString& format = QString(),
                        bool archiveByType = true);

    // ── 后端无关的下载控制：按 taskId 自动分派到 C 引擎或 yt-dlp 视频后端 ──
    void startTaskById(int id);
    void pauseTaskById(int id);
    void resumeTaskById(int id);
    void cancelTaskById(int id);
    void restartTaskById(int id);
    void removeTaskById(int id);

signals:
    // 媒体后端（Video/Hls/Torrent）的进度/完成/状态经此转发给主窗口桥接槽
    void mediaProgress(int taskId, qint64 downloaded, qint64 total, int speedBps);
    void mediaCompleted(int taskId, bool success, const QString& error);
    void mediaStateChanged(int taskId, int state);

private slots:
    void onMediaProgress(int taskId, qint64 downloaded, qint64 total, int speedBps);
    void onMediaCompleted(int taskId, bool success, const QString& error);
    void onMediaStateChanged(int taskId, int state);

private:
    TaskListModel*  m_model = nullptr;
    DownloadManager* m_manager = nullptr;
    QueueManager*    m_queueMgr = nullptr;
    QueueScheduler*  m_queueScheduler = nullptr;
    Settings*        m_settings = nullptr;

    std::function<void(int, int)>  m_setState;
    std::function<void(int)>       m_clearState;
    std::function<void(int, qint64)> m_setSpeed;
    std::function<void(int)>       m_clearSpeed;
    std::function<void(int)>       m_removeSchedule;
    std::function<void()>          m_cancelAutoPower;
    std::function<void()>          m_syncStatus;

    // 视频/HLS/BT 后端实例表（taskId → 后端对象）
    QHash<int, VideoDownloader*>   m_videoTasks;
    QHash<int, HlsDownloader*>     m_hlsTasks;
    QHash<int, TorrentDownloader*> m_torrentTasks;
    int m_videoIdSeq = 2000000;   // 与 C 引擎任务 id 区分的编号偏移

    // 延迟启动的媒体任务：加入队列（非立即开始）时缓存其 DownloadRequest，
    // 待队列调度器（QueueScheduler）放行后由 startTaskById 真正拉起后端。
    QMap<int, DownloadRequest> m_pendingStream;
};

#endif // IDM_APP_TASK_CONTROLLER_H
