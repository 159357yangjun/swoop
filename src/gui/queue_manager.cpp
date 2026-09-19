#include "queue_manager.h"
#include "app_paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

QueueManager::QueueManager(QObject* parent)
    : QObject(parent)
{
    QString dataDir = AppPaths::dataDir();   // 便携模式 = exe 目录，否则 AppData/Local
    QDir().mkpath(dataDir);
    m_path = dataDir + QStringLiteral("/queues.json");
}

bool QueueManager::contains(const QString& name) const
{
    for (const auto& q : m_queues)
        if (q.name == name) return true;
    return false;
}

DownloadQueue QueueManager::queue(const QString& name) const
{
    for (const auto& q : m_queues)
        if (q.name == name) return q;
    return DownloadQueue{};
}

bool QueueManager::addQueue(const QString& name, int maxConcurrent)
{
    if (name.isEmpty() || contains(name))
        return false;
    DownloadQueue q;
    q.name = name;
    q.maxConcurrent = maxConcurrent;
    m_queues.append(q);
    save();
    emit queuesChanged();
    return true;
}

bool QueueManager::removeQueue(const QString& name)
{
    for (int i = 0; i < m_queues.size(); ++i) {
        if (m_queues[i].name == name) {
            m_queues.removeAt(i);
            save();
            emit queuesChanged();
            return true;
        }
    }
    return false;
}

bool QueueManager::renameQueue(const QString& oldName, const QString& newName)
{
    if (newName.isEmpty() || (newName != oldName && contains(newName)))
        return false;
    for (auto& q : m_queues) {
        if (q.name == oldName) {
            q.name = newName;
            save();
            emit queuesChanged();
            return true;
        }
    }
    return false;
}

bool QueueManager::setQueueParams(const QString& name, int maxConcurrent, bool ordered,
                                   bool enabled, bool useSchedule, const QTime& startAt,
                                   const QTime& stopAt)
{
    for (auto& q : m_queues) {
        if (q.name == name) {
            q.maxConcurrent = maxConcurrent;
            q.ordered       = ordered;
            q.enabled       = enabled;
            q.useSchedule   = useSchedule;
            q.startAt       = startAt;
            q.stopAt        = stopAt;
            save();
            emit queuesChanged();
            return true;
        }
    }
    return false;
}

void QueueManager::load()
{
    QFile f(m_path);
    if (!f.open(QFile::ReadOnly | QFile::Text))
        return;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return;
    QJsonArray arr = doc.object().value(QStringLiteral("queues")).toArray();
    m_queues.clear();
    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        DownloadQueue q;
        q.name = o.value(QStringLiteral("name")).toString();
        q.maxConcurrent = o.value(QStringLiteral("maxConcurrent")).toInt(2);
        q.ordered       = o.value(QStringLiteral("ordered")).toBool(true);
        q.enabled       = o.value(QStringLiteral("enabled")).toBool(true);
        q.useSchedule   = o.value(QStringLiteral("useSchedule")).toBool(false);
        q.startAt       = QTime::fromString(o.value(QStringLiteral("startAt")).toString(),
                                            QStringLiteral("HH:mm"));
        q.stopAt        = QTime::fromString(o.value(QStringLiteral("stopAt")).toString(),
                                            QStringLiteral("HH:mm"));
        if (!q.startAt.isValid()) q.startAt = QTime(0, 0);
        if (!q.stopAt.isValid())  q.stopAt  = QTime(23, 59);
        if (!q.name.isEmpty())
            m_queues.append(q);
    }
}

void QueueManager::save()
{
    QJsonArray arr;
    for (const auto& q : m_queues) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), q.name);
        o.insert(QStringLiteral("maxConcurrent"), q.maxConcurrent);
        o.insert(QStringLiteral("ordered"), q.ordered);
        o.insert(QStringLiteral("enabled"), q.enabled);
        o.insert(QStringLiteral("useSchedule"), q.useSchedule);
        o.insert(QStringLiteral("startAt"), q.startAt.toString(QStringLiteral("HH:mm")));
        o.insert(QStringLiteral("stopAt"),  q.stopAt.toString(QStringLiteral("HH:mm")));
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("queues"), arr);
    QFile f(m_path);
    if (f.open(QFile::WriteOnly | QFile::Text))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}
