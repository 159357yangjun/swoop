#include "notification_aggregator.h"
#include <QSystemTrayIcon>

NotificationAggregator::NotificationAggregator(QObject* parent)
    : QObject(parent)
{
}

void NotificationAggregator::enqueue(bool success, const QString& name, const QString& error)
{
    if (!success) {
        m_failCount++;
        m_lastError = error;          // 空也记录：失败时正文只显示「N 个下载失败」
    } else {
        m_doneCount++;
        if (m_doneNames.size() < 4)   // 仅保留前几个名字用于提示
            m_doneNames.append(name);
    }

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setSingleShot(true);
        connect(m_timer, &QTimer::timeout, this, &NotificationAggregator::onTimeout);
    }
    m_timer->start(750);  // 750ms 内的连续完成合并为一条
}

void NotificationAggregator::onTimeout()
{
    QString title, body;
    if (m_doneCount > 0 && m_failCount == 0) {
        title = QStringLiteral("下载完成");
        body  = (m_doneCount == 1)
                    ? m_doneNames.first()
                    : QStringLiteral("%1 个下载已完成").arg(m_doneCount);
    } else if (m_doneCount == 0 && m_failCount > 0) {
        title = QStringLiteral("下载失败");
        body  = QStringLiteral("%1 个下载失败").arg(m_failCount);
        if (!m_lastError.isEmpty())
            body += QStringLiteral("：%1").arg(m_lastError);
    } else {
        title = QStringLiteral("下载完成");
        body  = QStringLiteral("完成 %1 个，失败 %2 个").arg(m_doneCount).arg(m_failCount);
    }

    const bool chime = (m_doneCount > 0);
    emit notify(title, body, chime);

    m_doneCount = 0; m_failCount = 0; m_doneNames.clear(); m_lastError.clear();
}
