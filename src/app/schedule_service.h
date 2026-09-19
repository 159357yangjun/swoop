#ifndef IDM_APP_SCHEDULE_SERVICE_H
#define IDM_APP_SCHEDULE_SERVICE_H

#include <QObject>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <functional>

class QTimer;

// ── L2 应用编排组件：定时下载持久化与触发 ───────────────────────────────
// 从 MainWindow 抽取。拥有 m_scheduledTasks(taskId→计划时间) / m_recurringTasks(每日重复集合)，
// 经 QSettings(AppPaths) 持久化；定时节拍到点后把「该启动的任务」通过 taskDue 信号交给上层，
// 本身不碰 UI。对 MainWindow 的耦合以两个回调解耦：
//   - taskExistsProvider：load 时校验任务是否还存在（避免幽灵定时）；
//   - taskDue 信号 → MainWindow::startTaskById。
//
// 行为契约（与原 MainWindow::loadSchedules / saveSchedules / onCheckScheduledTasks 一致）：
//  - load 丢弃指向已不存在任务的失效记录（必要时回写清理）；
//  - 到点：非重复任务从表中删除，每日重复任务递推到下一个未来时刻；变动即 save；
//  - 每个到期任务 emit taskDue(id)，由上层启动下载。
class ScheduleService : public QObject {
    Q_OBJECT
public:
    explicit ScheduleService(QObject* parent = nullptr);

    // 任务存在性校验（由 MainWindow 注入：m_taskModel->contains）
    void setTaskExistsProvider(std::function<bool(int)> cb);

    void load();   // 从 AppPaths::settings() 恢复（校验失效记录）
    void save() const;

    // UI 动作（onScheduleDownload）调用
    void setSchedule(int id, const QDateTime& time, bool recurring);
    void clearSchedule(int id);
    void removeTaskSchedule(int id);   // 任务被删时清幽灵定时

    bool hasPending() const { return !m_scheduledTasks.isEmpty(); }
    bool hasSchedule(int id) const { return m_scheduledTasks.contains(id); }
    QDateTime scheduleTime(int id) const { return m_scheduledTasks.value(id); }
    bool isRecurring(int id) const { return m_recurringTasks.contains(id); }

    // 启动定时检查节拍（默认 5s）
    void startChecker(int msec = 5000);

signals:
    // 有任务到点 → 上层据此 startTaskById(id)
    void taskDue(int id);
    // load() 恢复出若干条有效定时设置 → 上层据此在状态栏提示（沿用原 loadSchedules 的 UX）
    void schedulesRestored(int count);

private slots:
    void onTick();

private:
    std::function<bool(int)> m_taskExists;
    QMap<int, QDateTime> m_scheduledTasks;
    QSet<int>            m_recurringTasks;
    QTimer*              m_timer = nullptr;
};

#endif // IDM_APP_SCHEDULE_SERVICE_H
