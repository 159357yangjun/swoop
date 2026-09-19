#include "history_store.h"
#include "logger.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QVariant>
#include <QJsonObject>

HistoryStore& HistoryStore::instance()
{
    static HistoryStore s;
    return s;
}

HistoryStore::HistoryStore()
{
    // 使用独立连接名，避免与项目其他（潜在）Qt Sql 使用冲突
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                               QStringLiteral("idm_next_history"));
    m_db = db;
}

bool HistoryStore::init(const QString& dbPath)
{
    if (m_open)
        return true;
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        m_err = m_db.lastError().text();
        Log::error(QStringLiteral("HistoryStore 打开失败: %1").arg(m_err));
        return false;
    }
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS downloads ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  url TEXT NOT NULL,"
            "  filename TEXT,"
            "  dir TEXT,"
            "  size INTEGER DEFAULT -1,"
            "  status TEXT,"
            "  added_at TEXT,"
            "  finished_at TEXT"
            ")"))) {
        m_err = q.lastError().text();
        Log::error(QStringLiteral("HistoryStore 建表失败: %1").arg(m_err));
        return false;
    }
    m_open = true;
    Log::info(QStringLiteral("HistoryStore 已打开: %1").arg(dbPath));
    return true;
}

void HistoryStore::recordAdded(const QString& url, const QString& filename, const QString& dir)
{
    if (!m_open)
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO downloads (url, filename, dir, status, added_at) "
        "VALUES (?, ?, ?, 'queued', ?)"));
    q.addBindValue(url);
    q.addBindValue(filename);
    q.addBindValue(dir);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    if (!q.exec())
        Log::warn(QStringLiteral("HistoryStore 记录加入失败: %1").arg(q.lastError().text()));
}

void HistoryStore::recordFinished(const QString& url, const QString& filename,
                                  qint64 size, bool success)
{
    if (!m_open)
        return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE downloads SET status=?, size=?, finished_at=? "
        "WHERE id=(SELECT id FROM downloads WHERE url=? AND status='queued' "
        "         ORDER BY id DESC LIMIT 1)"));
    q.addBindValue(success ? QStringLiteral("completed") : QStringLiteral("failed"));
    q.addBindValue(size);
    q.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
    q.addBindValue(url);
    if (!q.exec()) {
        Log::warn(QStringLiteral("HistoryStore 更新失败: %1").arg(q.lastError().text()));
        return;
    }
    if (q.numRowsAffected() == 0) {
        // 没有对应的 queued 记录（例如直接完成的任务），新增一条
        QSqlQuery ins(m_db);
        ins.prepare(QStringLiteral(
            "INSERT INTO downloads (url, filename, size, status, finished_at) "
            "VALUES (?, ?, ?, ?, ?)"));
        ins.addBindValue(url);
        ins.addBindValue(filename);
        ins.addBindValue(size);
        ins.addBindValue(success ? QStringLiteral("completed") : QStringLiteral("failed"));
        ins.addBindValue(QDateTime::currentDateTime().toString(Qt::ISODate));
        if (!ins.exec())
            Log::warn(QStringLiteral("HistoryStore 记录完成失败: %1").arg(ins.lastError().text()));
    }
}

QJsonArray HistoryStore::recent(int limit)
{
    QJsonArray arr;
    if (!m_open)
        return arr;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, url, filename, size, status, added_at, finished_at "
        "FROM downloads ORDER BY id DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec())
        return arr;
    while (q.next()) {
        QJsonObject o;
        o[QStringLiteral("id")]          = q.value(0).toInt();
        o[QStringLiteral("url")]         = q.value(1).toString();
        o[QStringLiteral("filename")]    = q.value(2).toString();
        o[QStringLiteral("size")]        = q.value(3).toLongLong();
        o[QStringLiteral("status")]      = q.value(4).toString();
        o[QStringLiteral("added_at")]    = q.value(5).toString();
        o[QStringLiteral("finished_at")] = q.value(6).toString();
        arr.append(o);
    }
    return arr;
}

int HistoryStore::count() const
{
    if (!m_open)
        return 0;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM downloads")))
        return 0;
    if (q.next())
        return q.value(0).toInt();
    return 0;
}

void HistoryStore::clear()
{
    if (!m_open)
        return;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("DELETE FROM downloads")))
        Log::warn(QStringLiteral("HistoryStore 清空失败: %1").arg(q.lastError().text()));
}
