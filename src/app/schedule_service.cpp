#include "schedule_service.h"
#include "app_paths.h"
#include "logger.h"

#include <QSettings>
#include <QTimer>

// 与上游 MainWindow 共用同一把钥匙，配置互通
static const char* kSchedulesKey = "schedules";

ScheduleService::ScheduleService(QObject* parent)
    : QObject(parent)
{
}

void ScheduleService::setTaskExistsProvider(std::function<bool(int)> cb)
{
    m_taskExists = std::move(cb);
}

void ScheduleService::load()
{
    QSettings st = AppPaths::settings();
    const QString raw = st.value(QLatin1String(kSchedulesKey)).toString();
    if (raw.isEmpty())
        return;

    int kept = 0, dropped = 0;
    const QStringList items = raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& s : items) {
        const QStringList f = s.split(QLatin1Char('|'));
        if (f.size() != 3)
            continue;
        const int id = f.at(0).toInt();
        const QDateTime at = QDateTime::fromString(f.at(1), Qt::ISODate);
        if (id <= 0 || !at.isValid())
            continue;
        // 任务已不存在（被删掉，或状态文件丢了）→ 丢弃这条，
        // 否则会留下一个到点后对着不存在任务反复触发的「幽灵定时」。
        if (m_taskExists && !m_taskExists(id)) {
            dropped++;
            continue;
        }
        m_scheduledTasks[id] = at;
        if (f.at(2) == QLatin1String("1"))
            m_recurringTasks.insert(id);
        kept++;
    }

    if (kept > 0) {
        Log::info(QStringLiteral("已恢复 %1 条定时设置（丢弃 %2 条失效记录）").arg(kept).arg(dropped));
        emit schedulesRestored(kept);
    }
    if (dropped > 0 && kept == 0)
        save();   // 顺手把失效记录清掉，别一直留着
}

void ScheduleService::save() const
{
    QStringList items;
    for (auto it = m_scheduledTasks.constBegin(); it != m_scheduledTasks.constEnd(); ++it) {
        items << QStringLiteral("%1|%2|%3")
                     .arg(it.key())
                     .arg(it.value().toString(Qt::ISODate))
                     .arg(m_recurringTasks.contains(it.key()) ? 1 : 0);
    }
    QSettings st = AppPaths::settings();
    st.setValue(QLatin1String(kSchedulesKey), items.join(QLatin1Char(';')));
}

void ScheduleService::setSchedule(int id, const QDateTime& time, bool recurring)
{
    m_scheduledTasks[id] = time;
    // 「每日重复」必须真的被记住并参与调度：对话框一直有这个勾选项，
    // 但此前没人读 isRecurring()，勾了等于没勾（典型的 UI 有、行为无）。
    if (recurring) m_recurringTasks.insert(id);
    else           m_recurringTasks.remove(id);
    save();   // 立刻落盘：用户期望「设了就记住」，而不是关掉程序就丢
}

void ScheduleService::clearSchedule(int id)
{
    m_scheduledTasks.remove(id);
    m_recurringTasks.remove(id);
    save();
}

void ScheduleService::removeTaskSchedule(int id)
{
    m_scheduledTasks.remove(id);
    m_recurringTasks.remove(id);
    save();   // 同步落盘，避免留下指向已删任务的幽灵定时
}

void ScheduleService::startChecker(int msec)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ScheduleService::onTick);
    }
    m_timer->start(msec);
}

void ScheduleService::onTick()
{
    if (m_scheduledTasks.isEmpty())
        return;

    QDateTime now = QDateTime::currentDateTime();
    QList<int> toStart;
    bool changed = false;   // 有递推或删除就要回写配置（重复任务的下次时间得记住）

    for (auto it = m_scheduledTasks.begin(); it != m_scheduledTasks.end(); ) {
        if (it.value() <= now) {
            toStart << it.key();
            if (m_recurringTasks.contains(it.key())) {
                /* 每日重复：递推到下一个未来时刻，而不是简单 +24 小时。
                 * 按天递推能对齐到「每天的同一时刻」，即使程序当天没开、
                 * 连续错过好几天，重开后也只会立刻补跑一次并跳到下一个未来时刻，
                 * 不会攒出一串过期时间把任务反复拉起。 */
                QDateTime next = it.value();
                do { next = next.addDays(1); } while (next <= now);
                it.value() = next;
                Log::info(QStringLiteral("任务 #%1 每日重复，下次 %2")
                              .arg(it.key()).arg(next.toString("yyyy-MM-dd HH:mm:ss")));
                changed = true;
                ++it;
            } else {
                it = m_scheduledTasks.erase(it);
                changed = true;
            }
        } else {
            ++it;
        }
    }
    if (changed)
        save();

    for (int id : toStart)
        emit taskDue(id);
}
