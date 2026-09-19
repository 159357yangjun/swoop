#include "queue_scheduler.h"

#include <QTime>
#include <QTimer>

#include <algorithm>

QueueScheduler::QueueScheduler(QObject* parent)
    : QObject(parent)
{
}

void QueueScheduler::setQueuesProvider(std::function<QList<DownloadQueue>()> cb)   { m_queues = std::move(cb); }
void QueueScheduler::setTasksProvider(std::function<QVector<TaskRow>()> cb)        { m_tasks = std::move(cb); }
void QueueScheduler::setMaxConcurrentProvider(std::function<int()> cb)             { m_maxConcurrent = std::move(cb); }
void QueueScheduler::setStartTaskCallback(std::function<void(int)> cb)             { m_startTask = std::move(cb); }

void QueueScheduler::markDeferred(int id)
{
    m_deferredByCap.insert(id);   // 等全局并发空位
}

void QueueScheduler::clearDeferred(int id)
{
    m_deferredByCap.remove(id);
}

void QueueScheduler::start(int msec)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &QueueScheduler::onTick);
    }
    m_timer->start(msec);
}

// ── 全局并发余量：设置页「同时下载的任务数」──
bool QueueScheduler::hasFreeSlot() const
{
    if (!m_tasks || !m_maxConcurrent)
        return true;
    const int limit = qMax(1, m_maxConcurrent());
    int running = 0;
    for (const auto& t : m_tasks())
        if (t.state == 1)
            running++;
    return running < limit;
}

// ── 队列调度器：各队列并发数超限时排队等待 ───────────
void QueueScheduler::onTick()
{
    if (!m_queues || !m_tasks || !m_maxConcurrent || !m_startTask)
        return;

    const QVector<TaskRow> tasks = m_tasks();

    // 统计每个队列正在下载（state==1）的任务数
    QMap<QString, int> running;
    int totalRunning = 0;                 // 全任务口径（含无队列的手动任务）
    for (const auto& t : tasks) {
        if (t.state != 1)
            continue;
        totalRunning++;
        if (!t.queue.isEmpty())
            running[t.queue]++;
    }

    // 「同时下载的任务数」是全局上限：队列内并发再高也不能把总量顶穿，
    // 否则设置页那一项就只是摆设（各队列各管各的，加起来无上限）。
    const int globalLimit = qMax(1, m_maxConcurrent());
    int globalBudget = globalLimit - totalRunning;

    QTime now = QTime::currentTime();
    for (const auto& q : m_queues()) {
        // 队列被停止：不启动该队列的任何新任务
        if (!q.enabled)
            continue;
        // 计划时间窗口：仅在开始~停止之间才启动新下载
        if (q.useSchedule && (now < q.startAt || now > q.stopAt))
            continue;

        int canStart = qMax(0, q.maxConcurrent - running.value(q.name, 0));
        if (canStart > globalBudget)
            canStart = globalBudget;
        if (canStart <= 0)
            continue;
        // 收集该队列中等待（state==0）的任务，按加入顺序（IDM 默认按序下载）
        QList<int> pending;
        for (const auto& t : tasks)
            if (t.queue == q.name && t.state == 0)
                pending << t.id;
        if (!q.ordered)
            std::reverse(pending.begin(), pending.end());
        for (int i = 0; i < pending.size() && i < canStart; ++i) {
            m_startTask(pending[i]);
            globalBudget--;
        }
    }

    // 无队列（手动/浏览器直下）的等待任务：只放行「因并发上限被暂缓」的那些
    // （m_deferredByCap），用户自己留着不开始的任务绝不擅自拉起。
    if (globalBudget > 0 && !m_deferredByCap.isEmpty()) {
        QList<int> waiting = m_deferredByCap.values();
        std::sort(waiting.begin(), waiting.end());   // 按加入顺序（ID 递增）
        for (int id : waiting) {
            if (globalBudget <= 0)
                break;
            // 已不是等待态（被删/被手动开始）→ 从暂缓集合摘掉
            // 注意：任务若已不在表里（被删），同样视为非等待态而跳过 —— 与原
            // m_states.value(id, -1) != 0 的判定一致，避免对着不存在的任务调 start。
            int state = -1;
            for (const auto& t : tasks) {
                if (t.id == id) { state = t.state; break; }
            }
            if (state != 0) {
                m_deferredByCap.remove(id);
                continue;
            }
            m_startTask(id);
            m_deferredByCap.remove(id);
            globalBudget--;
        }
    }
}
