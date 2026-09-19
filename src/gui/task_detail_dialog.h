#ifndef TASK_DETAIL_DIALOG_H
#define TASK_DETAIL_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QProgressBar>

class DownloadManager;
class QTimer;
class SpeedChartWidget;
class QTabWidget;
class QPushButton;

// 任务详情对话框：双击任务列表行弹出，展示单个任务的完整实时信息
// 通过定时器轮询引擎 dlmgr_get_task_info 刷新数据（500ms 间隔）
class TaskDetailDialog : public QDialog {
    Q_OBJECT
public:
    TaskDetailDialog(int taskId, DownloadManager* mgr, QWidget* parent = nullptr);
    ~TaskDetailDialog();

private slots:
    void refreshData();  // 定时刷新引擎数据到 UI
    void onVerifyFile(); // 打开文件校验对话框

private:
    void setupUi();

    int                m_taskId;
    DownloadManager*   m_mgr;
    QTimer*            m_timer;

    // 信息标签
    QLabel*      m_fileNameLabel;
    QLabel*      m_urlLabel;
    QLabel*      m_statusLabel;
    QLabel*      m_protocolLabel;   // 协议版本（HTTP/1.1 / HTTP/2）
    QLabel*      m_sizeLabel;
    QLabel*      m_downloadedLabel;
    QLabel*      m_speedLabel;
    QLabel*      m_etaLabel;
    QLabel*      m_chunksLabel;
    QLabel*      m_errorLabel;
    QProgressBar* m_progressBar;
    QPushButton*  m_verifyBtn;  // 文件校验按钮

    // 速度图表标签页
    QTabWidget*      m_tabWidget;
    SpeedChartWidget* m_speedChart;
};

#endif // TASK_DETAIL_DIALOG_H
