#ifndef QUEUE_MANAGER_DIALOG_H
#define QUEUE_MANAGER_DIALOG_H

#include <QDialog>

class QueueManager;
class QListWidget;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QTimeEdit;
class QPushButton;

// 下载队列管理对话框（对齐 IDM「队列属性」）：
// 左侧队列列表（新建/删除/重命名），右侧属性分组（同时下载数 / 按序 / 启用 / 计划时间）
class QueueManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit QueueManagerDialog(QueueManager* mgr, QWidget* parent = nullptr);

private slots:
    void onAdd();
    void onRemove();
    void onRename();
    void onSelectionChanged();
    void onParamsChanged();
    void onToggleQueue();      // 开始/停止队列
    void onScheduleToggled(bool on);

private:
    void refreshList();
    void refreshParams();
    void setPropsEnabled(bool on);   // 右侧属性面板随选中状态启用/禁用

    QueueManager* m_mgr;
    QListWidget*  m_list;
    QLineEdit*    m_nameEdit;
    QPushButton*  m_addBtn;
    QPushButton*  m_renameBtn;
    QPushButton*  m_removeBtn;
    QPushButton*  m_toggleBtn;       // 开始/停止队列

    // 右侧属性
    QSpinBox*   m_concSpin;
    QCheckBox*  m_orderedCheck;      // 按添加顺序下载
    QCheckBox*  m_schedCheck;        // 启用计划时间
    QTimeEdit*  m_startTime;
    QTimeEdit*  m_stopTime;
};

#endif // QUEUE_MANAGER_DIALOG_H
