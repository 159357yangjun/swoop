#ifndef IDM_APP_QUEUE_SCHEDULER_H
#define IDM_APP_QUEUE_SCHEDULER_H

#include <QObject>
#include <QList>
#include <QSet>
#include <QVector>
#include <functional>

#include "queue_manager.h"     // DownloadQueue（队列定义）
#include "task_list_model.h"   // TaskRow（任务行，取 state/queue/id）

class QTimer;

// ── L2 应用编排组件：队列调度器（各队列并发 + 全局并发上限） ─────────────
// 从 MainWindow 抽取。拥有「因全局并发上限被暂缓的任务」集合（原 m_deferredByCap）。
// 每秒节拍一次：按各队列的并发数 / 计划时间窗口，把 state==0 的等待任务交给上层启动；
// 无队列的手动任务只放行**曾被上限暂缓**的那些 —— 用户自己留着不开始的任务绝不擅自拉起。
//
// 对 MainWindow 只读经注入提供器（队列表 / 任务表 / 同时下载上限），启动动作经回调，
// 组件自身不碰引擎与 UI。
class QueueScheduler : public QObject {
    Q_OBJECT
public:
    explicit QueueScheduler(QObject* parent = nullptr);

    void setQueuesProvider(std::function<QList<DownloadQueue>()> cb);
    void setTasksProvider(std::function<QVector<TaskRow>()> cb);
    void setMaxConcurrentProvider(std::function<int()> cb);
    void setStartTaskCallback(std::function<void(int)> cb);   // → MainWindow::startTaskById

    // 被「同时下载的任务数」上限暂缓的任务登记（原 m_deferredByCap）
    void markDeferred(int id);
    void clearDeferred(int id);

    bool hasFreeSlot() const;   // 原 MainWindow::hasFreeDownloadSlot

    // 启动节拍定时器（默认 1s，原 m_queueTimer）
    void start(int msec = 1000);

private slots:
    void onTick();

private:
    std::function<QList<DownloadQueue>()> m_queues;
    std::function<QVector<TaskRow>()>     m_tasks;
    std::function<int()>                  m_maxConcurrent;
    std::function<void(int)>              m_startTask;

    QSet<int> m_deferredByCap;
    QTimer*   m_timer = nullptr;
};

#endif // IDM_APP_QUEUE_SCHEDULER_H
