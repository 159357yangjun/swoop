#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include <QObject>
#include <QString>

// 下载引擎管理器：封装纯 C 的 download_core.c，暴露 Qt 风格的信号槽 API
// 引擎层（dlmgr_* 函数）原封不动复用，本类仅做桥接
class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(QObject* parent = nullptr);
    ~DownloadManager();

    // 任务管理
    int  addTask(const QString& url, const QString& saveDir,
                 const QString& fileName = QString(), int threadCount = 0);
    void startTask(int taskId);
    void pauseTask(int taskId);
    void resumeTask(int taskId);
    void cancelTask(int taskId);
    void restartTask(int taskId);  // 重新开始：重置分片、删临时文件、重新探测
    void removeTask(int taskId);
    void rebindTask(int taskId);   // 重新绑定引擎回调（dlmgr_load_state 恢复的任务用）

    // 持久化
    bool saveState(const QString& path);
    bool loadState(const QString& path);

    // 全局配置
    void setMaxThreads(int n);
    void setMaxSpeed(int bytesPerSec);  // 0 = 不限速

signals:
    // 转发引擎回调给 UI
    void taskProgress(int taskId, qint64 downloaded, qint64 total, int speedBps);
    void taskCompleted(int taskId, bool success, const QString& error);
    void taskStateChanged(int taskId, int state);  // 0=PENDING 1=RUNNING 2=PAUSED 3=COMPLETED 4=FAILED 5=CANCELLED

private:
    // 引擎回调桥接（C 函数指针无法直接捕获 this，用 static + userdata 转发）
    static void progressCb(int task_id, int64_t downloaded, int64_t total, int speed, void* ud);
    static void completeCb(int task_id, int success, void* ud);

    void forwardProgress(int task_id, int64_t downloaded, int64_t total, int speed);
    void forwardComplete(int task_id, int success);

    // 进度聚合刷新：将高频的逐块进度信号合并为固定 ~10Hz 的批量刷新，
    // 避免成百上千次 taskProgress 信号淹没 GUI 事件循环（throttling/sampling 调度）
    void flushProgress();

    class Impl;
    Impl* d;  // pImpl：隐藏 C 引擎细节
};

#endif // DOWNLOAD_MANAGER_H
