#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

#include <QObject>
#include <QJsonArray>
#include <QString>
#include <QSqlDatabase>

// 下载历史持久化（SQLite，Qt6::Sql + 自带 QSQLite 驱动）
// 以“下载历史”形式持久化记录（与引擎自身的活动任务状态文件互不干扰），
// 跨会话保留已完成/失败任务的 URL、文件名、大小、状态与时间戳，供日后查询/审计。
class HistoryStore : public QObject {
    Q_OBJECT
public:
    static HistoryStore& instance();

    // 打开（必要时创建）数据库文件并建立表；返回是否成功
    bool init(const QString& dbPath);
    bool isOpen() const { return m_open; }

    // 记录“已加入”事件（status=queued）
    void recordAdded(const QString& url, const QString& filename, const QString& dir);
    // 记录“完成/失败”事件：优先更新同 URL 最近一条 queued 记录，否则新增
    void recordFinished(const QString& url, const QString& filename,
                        qint64 size, bool success);

    // 最近 limit 条历史（倒序），每行含 id/url/filename/size/status/added_at/finished_at
    QJsonArray recent(int limit = 200);
    int  count() const;
    void clear();

    QString lastError() const { return m_err; }

private:
    HistoryStore();
    bool m_open = false;
    QString m_err;
    QSqlDatabase m_db;
};

#endif // HISTORY_STORE_H
