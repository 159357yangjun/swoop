#ifndef TASK_LIST_MODEL_H
#define TASK_LIST_MODEL_H

#include <QAbstractTableModel>
#include <QVector>
#include <QHash>
#include <QDateTime>

// 任务列表数据模型：连接引擎任务队列与 UI 表格视图
// 阶段2填充实际数据映射
struct TaskRow {
    int       id;
    QString   fileName;
    qint64    fileSize;
    qint64    downloaded;
    int       speedBps;
    int       state;        // 见 taskStateChanged 的取值（0~5）
    QString   statusText;
    QString   queue;        // 所属下载队列名称，空 = 不属于任何队列
    QString   url;          // 原始 URL（用于描述列）
    QDateTime lastConnection; // 最近一次收到数据的时间（最后连接列）
    int       progressPct = -1; // 显式进度百分比（未知文件大小时由后端直接上报，>=0 优先）
    QString   protocol;    // 实际协商到的协议版本："HTTP/1.1" / "HTTP/2"（仅 HTTPS 可能 HTTP/2，空=未知）
    QString   errorMsg;    // 最近一次失败原因（引擎写入），仅失败行非空，用于行 tooltip
    QString   notice;      // 临时状态文本（如「服务器限流，3 秒后重试」），非空时优先于 stateText(state) 显示
};

class TaskListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit TaskListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    // 数据操作
    void addTask(const TaskRow& row);
    void updateProgress(int taskId, qint64 downloaded, int speedBps);
    void setProgressPct(int taskId, int pct);  // 未知大小时由后端直接上报进度
    void updateStatus(int taskId, int state);
    // 差量判断：该任务当前的 状态/进度/速度/协议 是否与传入值不同（供 4Hz 刷新循环跳过未变任务）
    bool isStale(int taskId, qint64 downloaded, int speedBps, int state, const QString& proto) const;
    void setProtocol(int taskId, const QString& proto); // 刷新实际协商协议版本（HTTP/1.1 / HTTP/2）
    void setErrorMsg(int taskId, const QString& msg);   // 同步失败原因（仅状态切换为失败时调用，避免每 tick 做字符串比较）
    // 临时状态文本（服务器限流倒计时）。空串 = 撤掉、回落到 stateText(state)。
    void setStatusNotice(int taskId, const QString& notice);
    void setGlobalTraffic(const QString& text); // 刷新全局流量档位/限速状态（所有行共用）
    void setQueue(int taskId, const QString& queue);
    void removeTask(int taskId);
    int  taskIdForRow(int row) const;
    bool contains(int taskId) const;
    void clear();

    const QVector<TaskRow>& tasks() const { return m_tasks; }

    // 供 CategoryFilterProxy 按状态过滤使用的自定义角色
    enum { StateRole = Qt::UserRole + 1, QueueRole = Qt::UserRole + 2,
           ProgressRole = Qt::UserRole + 3,   // 进度百分比 0-100（进度条委托用）
           GroupHeaderRole = Qt::UserRole + 6 };  // 分组代理：该行是否为分组头（bool）

    // 状态枚举 → 中文文本（与 download_core.h TaskStatus 对齐：0待定 1运行 2暂停 3完成 4失败 5取消）
    static QString stateText(int state);
    // 引擎的限流等待 → 一句中文（sec<=0 返回空串表示「没在等」）。
    // 抽成静态纯函数是为了让离屏探针 ui_snapshot 能直接断言文案，而不必真的造一个 429 服务器。
    static QString throttleNoticeText(int sec, int http);

private:
    // 让第 2 列的显示文本跟上「有无临时通知」这件事（notice 优先，否则用状态文本）
    void refreshStatusText(int rowIdx);
    QVector<TaskRow> m_tasks;
    QHash<int, int>  m_rowOf;   // taskId -> 行索引，O(1) 查找（增删时同步维护）
    QHash<int, qint64> m_lastPaintMs; // taskId -> 上次进度列重绘时间戳(ms)，用于节流
    QString          m_trafficText; // 全局流量档位/限速状态文本（「限速」列共用）
};

#endif // TASK_LIST_MODEL_H
