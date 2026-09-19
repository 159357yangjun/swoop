#include "task_list_model.h"
#include "app_icons.h"   // fileTypeIcon：文件名列的类型图标

#include <QUrl>
#include <QColor>
#include <QBrush>
#include <QFont>
#include <QDateTime>
#include <cmath>

TaskListModel::TaskListModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

int TaskListModel::rowCount(const QModelIndex&) const
{
    return m_tasks.size();
}

int TaskListModel::columnCount(const QModelIndex&) const
{
    return 9;  // 文件名/大小/状态/剩余时间/传输速度/最后连接/描述/协议/限速
}

static QString formatBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("-");
    if (bytes >= 1073741824)
        return QStringLiteral("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1048576)
        return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

static QString formatSpeed(int bps)
{
    if (bps <= 0) return QStringLiteral("-");
    if (bps >= 1048576)
        return QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 2);
    if (bps >= 1024)
        return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B/s").arg(bps);
}

static QString formatRemainingTime(qint64 remainBytes, int bps)
{
    if (remainBytes <= 0) return QStringLiteral("0:00:00");
    if (bps <= 0)       return QStringLiteral("未知");
    int secs = static_cast<int>(remainBytes / bps);
    if (secs < 0) secs = 0;
    int h = secs / 3600;
    int m = (secs % 3600) / 60;
    int s = secs % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QVariant TaskListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    if (role == StateRole)
        return m_tasks[index.row()].state;
    if (role == QueueRole)
        return m_tasks[index.row()].queue;
    if (role == ProgressRole) {
        const TaskRow& row = m_tasks[index.row()];
        if (row.state == 3) return 100;                 // 已完成固定 100%
        if (row.progressPct >= 0) return row.progressPct;  // 后端直接上报的进度
        if (row.fileSize <= 0) return 0;
        int pct = (int)(row.downloaded * 100 / row.fileSize);
        return qBound(0, pct, 100);
    }
    if (role == Qt::ToolTipRole) {
        // 文件名列会被截断（「curl-8.22...」），描述列只显示域名：
        // 没有 tooltip 用户就没法确认到底是哪个文件、哪个链接。
        const TaskRow& row = m_tasks[index.row()];
        QStringList tips;
        if (index.column() == 0 && !row.fileName.isEmpty())
            tips << row.fileName;
        if (row.state == 4 && !row.errorMsg.isEmpty())
            tips << QStringLiteral("失败原因：%1").arg(row.errorMsg);
        if ((index.column() == 0 || index.column() == 6) && !row.url.isEmpty())
            tips << row.url;
        if (index.column() == 5 && row.lastConnection.isValid())
            tips << QStringLiteral("最后连接：%1")
                        .arg(row.lastConnection.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        return tips.isEmpty() ? QVariant() : tips.join(QLatin1Char('\n'));
    }
    if (role == Qt::ForegroundRole) {
        // 协议列（7）协商到 HTTP/2 时绿色高亮，与任务详情弹窗配色一致
        if (index.column() == 7 && m_tasks[index.row()].protocol == QStringLiteral("HTTP/2"))
            return QBrush(QColor(26, 143, 75));  // #1a8f4b
        return QVariant();
    }
    if (role == Qt::FontRole) {
        if (index.column() == 7 && m_tasks[index.row()].protocol == QStringLiteral("HTTP/2")) {
            QFont f;
            f.setBold(true);
            return f;
        }
        return QVariant();
    }
    if (role == Qt::DecorationRole && index.column() == 0) {
        // 文件名列加文件类型图标：几十行里用户是「找那个 zip」，不是「读第 7 行」。
        // 颜色语义与左侧分类树同一套（AppIcons::fileTypeIcon，内部有缓存）。
        return AppIcons::fileTypeIcon(m_tasks[index.row()].fileName);
    }
    if (role != Qt::DisplayRole)
        return QVariant();

    const TaskRow& row = m_tasks[index.row()];
    switch (index.column()) {
        case 0: return row.fileName;
        case 1: return formatBytes(row.fileSize);
        case 2: {
            // 状态列：文本 + 进度百分比
            int pct = row.progressPct >= 0 ? row.progressPct
                      : (row.fileSize > 0
                         ? static_cast<int>(row.downloaded * 100 / row.fileSize) : 0);
            if (pct > 0 && row.state != 3)
                return QStringLiteral("%1 (%2%)").arg(row.statusText).arg(pct);
            return row.statusText;
        }
        case 3: return formatRemainingTime(row.fileSize - row.downloaded, row.speedBps);
        case 4: return formatSpeed(row.speedBps);
        case 5: {
            if (!row.lastConnection.isValid())
                return QStringLiteral("-");
            // 列表里只显示「月-日 时:分」：完整格式（yyyy-MM-dd HH:mm:ss）需要约 176px，
            // 硬塞会把列挤到显示「2026-09-14 ...」这种既占地方又读不出时间的截断文本。
            // 完整时间放 tooltip（见下方 ToolTipRole）。
            return row.lastConnection.toString(QStringLiteral("MM-dd HH:mm"));
        }
        case 6: {
            // 描述列：优先显示域名，避免 URL 过长
            if (row.url.isEmpty()) return QStringLiteral("-");
            QString host = QUrl(row.url).host();
            if (host.isEmpty()) {
                QString u = row.url;
                if (u.length() > 45) u = u.left(45) + QStringLiteral("...");
                return u;
            }
            return host;
        }
        case 7: {
            // 协议列：实际协商到的版本（HTTP/2 仅 HTTPS 出现），未协商显示占位
            return row.protocol.isEmpty() ? QStringLiteral("-") : row.protocol;
        }
        case 8: {
            // 限速列：全局流量档位/限速状态（所有行共用），未设置显示占位
            return m_trafficText.isEmpty() ? QStringLiteral("-") : m_trafficText;
        }
        default: return QVariant();
    }
}

QVariant TaskListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    static const QStringList headers = {
        QStringLiteral("文件名"), QStringLiteral("大小"),
        QStringLiteral("状态"), QStringLiteral("剩余时间"),
        QStringLiteral("传输速度"), QStringLiteral("最后连接"),
        QStringLiteral("描述"), QStringLiteral("协议"), QStringLiteral("限速")
    };
    return headers.value(section, QString());
}

void TaskListModel::addTask(const TaskRow& row)
{
    beginInsertRows(QModelIndex(), m_tasks.size(), m_tasks.size());
    TaskRow r = row;
    if (r.statusText.isEmpty()) r.statusText = stateText(r.state);
    int idx = m_tasks.size();
    m_tasks.append(r);
    m_rowOf[r.id] = idx;
    endInsertRows();
}

void TaskListModel::updateProgress(int taskId, qint64 downloaded, int speedBps)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    // 4Hz 刷新循环对每任务调用：值未变则不触发重绘，避免已完成/空闲行被反复全量重绘
    if (m_tasks[i].downloaded == downloaded && m_tasks[i].speedBps == speedBps)
        return;
    qint64 oldD = m_tasks[i].downloaded;
    m_tasks[i].downloaded = downloaded;
    m_tasks[i].speedBps   = speedBps;
    if (speedBps > 0)
        m_tasks[i].lastConnection = QDateTime::currentDateTime();

    // 节流守卫：进度列重绘最多 ~10Hz。仅当距上次重绘 ≥100ms 或进度变化 ≥0.5%
    // （未知大小时按已下载字节 ≥64KB）才发 dataChanged，避免大量任务同时下载时每个 tick
    // 都触发整行重绘，滚动与交互更顺滑；底层数据始终最新，最多视觉滞后 100ms。
    qint64 now  = QDateTime::currentMSecsSinceEpoch();
    qint64 last = m_lastPaintMs.value(taskId, 0);
    bool timeGate  = (now - last) >= 100;
    bool deltaGate = false;
    if (m_tasks[i].fileSize > 0) {
        double oldP = oldD * 100.0 / m_tasks[i].fileSize;
        double newP = downloaded * 100.0 / m_tasks[i].fileSize;
        deltaGate = std::fabs(newP - oldP) >= 0.5;
    } else {
        deltaGate = (downloaded - oldD) >= 65536;     // 未知大小：每 64KB 才重绘
    }
    if (timeGate || deltaGate) {
        m_lastPaintMs[taskId] = now;
        emit dataChanged(index(i, 2), index(i, 5));
    }
}

bool TaskListModel::isStale(int taskId, qint64 downloaded, int speedBps, int state, const QString& proto) const
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return true;     // 不在模型中视为需同步
    const TaskRow& r = m_tasks[it.value()];
    return r.state != state
        || r.downloaded != downloaded
        || r.speedBps != speedBps
        || r.protocol != proto;
}

void TaskListModel::setProgressPct(int taskId, int pct)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    int np = qBound(0, pct, 100);
    int old = m_tasks[i].progressPct;
    if (old == np)
        return;
    m_tasks[i].progressPct = np;
    // 进度条（ProgressRole）与状态列（2）需刷新；同样走节流守卫（最多 10Hz）
    qint64 now  = QDateTime::currentMSecsSinceEpoch();
    qint64 last = m_lastPaintMs.value(taskId, 0);
    bool timeGate  = (now - last) >= 100;
    bool deltaGate = std::fabs(np - old) >= 1;        // 百分比变化 ≥1 才重绘（0.5% 四舍五入）
    if (timeGate || deltaGate) {
        m_lastPaintMs[taskId] = now;
        emit dataChanged(index(i, 2), index(i, 2));
    }
}

void TaskListModel::updateStatus(int taskId, int state)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    if (m_tasks[i].state == state)
        return;   // 状态未变 → 不触发重绘（4Hz 循环下避免无谓全量重绘）
    m_tasks[i].state = state;
    m_tasks[i].statusText = stateText(state);
    // 列 2=状态(含进度条) … 5=最后连接：状态变更（尤其完成/失败）须刷新进度条与状态文本，
    // 否则进度条会停在 <100% 不更新（进度条绘制于第 2 列）。
    emit dataChanged(index(i, 2), index(i, 5));
}

void TaskListModel::setProtocol(int taskId, const QString& proto)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    if (m_tasks[i].protocol == proto) return;  // 无变化不触发重绘
    m_tasks[i].protocol = proto;
    emit dataChanged(index(i, 7), index(i, 7));
}

void TaskListModel::setErrorMsg(int taskId, const QString& msg)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    if (m_tasks[i].errorMsg == msg) return;
    m_tasks[i].errorMsg = msg;
    // tooltip 是行级的：整行重绘一次即可
    emit dataChanged(index(i, 0), index(i, columnCount() - 1));
}

void TaskListModel::setGlobalTraffic(const QString& text)
{
    if (m_trafficText == text) return;  // 无变化不触发全列重绘
    m_trafficText = text;
    if (!m_tasks.isEmpty())
        emit dataChanged(index(0, 8), index(m_tasks.size() - 1, 8));
}

void TaskListModel::setQueue(int taskId, const QString& queue)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    m_tasks[i].queue = queue;
    // 通知代理层队列归属变化（用于队列过滤）
    emit dataChanged(index(i, 0), index(i, 0),
                     QVector<int>() << QueueRole);
}

void TaskListModel::removeTask(int taskId)
{
    auto it = m_rowOf.find(taskId);
    if (it == m_rowOf.end()) return;
    int i = it.value();
    beginRemoveRows(QModelIndex(), i, i);
    m_tasks.removeAt(i);
    endRemoveRows();
    m_rowOf.remove(taskId);
    m_lastPaintMs.remove(taskId);   // 清理节流时间戳，避免哈希无限增长
    // 移除后后续行整体左移，索引需同步 -1
    for (auto rit = m_rowOf.begin(); rit != m_rowOf.end(); ++rit) {
        if (rit.value() > i)
            --rit.value();
    }
}

int TaskListModel::taskIdForRow(int row) const
{
    if (row < 0 || row >= m_tasks.size())
        return -1;
    return m_tasks[row].id;
}

bool TaskListModel::contains(int taskId) const
{
    // 复用 id→行 哈希索引，O(1) 判定；避免在 4Hz 刷新循环中对每任务做 O(n) 线性扫描
    return m_rowOf.contains(taskId);
}

QString TaskListModel::stateText(int state)
{
    switch (state) {
        case 0: return QStringLiteral("等待中");
        case 1: return QStringLiteral("下载中");
        case 2: return QStringLiteral("已暂停");
        case 3: return QStringLiteral("已完成");
        case 4: return QStringLiteral("失败");
        case 5: return QStringLiteral("已取消");
        default: return QStringLiteral("未知");
    }
}

void TaskListModel::clear()
{
    beginResetModel();
    m_tasks.clear();
    m_rowOf.clear();
    m_lastPaintMs.clear();
    endResetModel();
}
