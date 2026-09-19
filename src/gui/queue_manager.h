#ifndef QUEUE_MANAGER_H
#define QUEUE_MANAGER_H

#include <QObject>
#include <QString>
#include <QList>
#include <QTime>

// 下载队列定义（对齐 IDM「队列属性」：同时下载数 / 按序下载 / 启用开关 / 计划时间）
struct DownloadQueue {
    QString name;
    int  maxConcurrent = 2;        // 同时下载的任务数
    bool ordered       = true;     // 按添加顺序下载（IDM 默认开启）
    bool enabled       = true;     // 队列是否启用（可在弹窗里 停止/开始）
    bool useSchedule   = false;    // 是否启用「开始/停止时间」计划
    QTime startAt      = QTime(0, 0);
    QTime stopAt       = QTime(23, 59);
};

// 队列管理：维护队列定义并持久化到 AppData 的 queues.json
// 注意：任务的「归属队列」记录在 TaskListModel（GUI 层），本类只管队列本身的元数据
class QueueManager : public QObject {
    Q_OBJECT
public:
    explicit QueueManager(QObject* parent = nullptr);

    QList<DownloadQueue> queues() const { return m_queues; }
    bool contains(const QString& name) const;
    DownloadQueue queue(const QString& name) const;

    bool addQueue(const QString& name, int maxConcurrent = 2);
    bool removeQueue(const QString& name);
    bool renameQueue(const QString& oldName, const QString& newName);
    bool setQueueParams(const QString& name, int maxConcurrent, bool ordered,
                        bool enabled, bool useSchedule, const QTime& startAt,
                        const QTime& stopAt);

    void load();
    void save();

signals:
    void queuesChanged();   // 队列增删改后触发，供主窗口重建左侧树

private:
    QList<DownloadQueue> m_queues;
    QString m_path;
};

#endif // QUEUE_MANAGER_H
