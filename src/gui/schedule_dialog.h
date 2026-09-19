#ifndef SCHEDULE_DIALOG_H
#define SCHEDULE_DIALOG_H

#include <QDialog>
#include <QDateTime>

class QDateTimeEdit;
class QComboBox;
class QCheckBox;

// 定时下载设置对话框：为指定任务设置定时开始时间
class ScheduleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScheduleDialog(QWidget* parent = nullptr);

    // 获取设置结果
    QDateTime scheduledTime() const;
    bool      isEnabled() const;       // 是否启用定时
    bool      isRecurring() const;     // 是否每日重复

    // 预填已有定时设置（用于编辑模式）
    void setScheduledTime(const QDateTime& dt);
    void setEnabled(bool enabled);
    void setRecurring(bool recurring);

private:
    QDateTimeEdit* m_dateTimeEdit;
    QCheckBox*     m_enableCheck;
    QCheckBox*     m_recurringCheck;   // 每日重复
};

#endif // SCHEDULE_DIALOG_H
