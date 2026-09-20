#pragma once

#include <QObject>
#include <QTimer>
#include <QString>
#include <QStringList>

/**
 * NotificationAggregator（L2 应用编排组件）
 *
 * 把 MainWindow 里「下载完成/失败气泡」的聚合逻辑收进来：同一时间窗口（750ms）
 * 内的多次完成/失败被合并成单条托盘气泡 + 至多一次完成提示音，避免刷屏。
 *
 * 组件只负责「累计 + 到期合并」，不碰 UI：到期后经 notify(title, body, chime) 信号
 * 把内容交回 MainWindow，由 MainWindow 经 m_trayIcon->showMessage 显示、并按 chime
 * 决定是否播放提示音。任务名 / 错误原因由调用方在 enqueue 时传入。
 */
class NotificationAggregator : public QObject {
    Q_OBJECT
public:
    explicit NotificationAggregator(QObject* parent = nullptr);

    // 累计一次完成/失败。name 用于单条提示（取首个完成的任务名）；
    // error 为该次失败的原因，用于失败气泡正文（空则省略）。
    void enqueue(bool success, const QString& name, const QString& error = QString());

signals:
    // 聚合窗口到期后发出：title/body 为气泡内容，chime 表示应播放完成提示音。
    void notify(const QString& title, const QString& body, bool chime);

private slots:
    void onTimeout();

private:
    QTimer*     m_timer = nullptr;
    int         m_doneCount = 0;
    int         m_failCount = 0;
    QStringList m_doneNames;   // 已完成任务名（取首个用于单条提示）
    QString     m_lastError;   // 最近一次失败原因（用于失败气泡）
};
