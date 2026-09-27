#ifndef NEW_TASK_DIALOG_H
#define NEW_TASK_DIALOG_H

#include <QDialog>

// 新建任务对话框的结果：用户输入的下载参数
struct NewTaskInput {
    QString url;          // 下载链接（必填）
    QString saveDir;      // 保存目录
    QString fileName;     // 文件名（可选，空=自动从 URL 推断）
    int     threadCount;  // 并发线程数（HTTP 分片用）
    QString format;       // 视频格式码（yt-dlp -f），空=最佳画质自动合并
};

class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QLabel;
class QWidget;
class QTimer;
#include <QProcess>

class NewTaskDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewTaskDialog(QWidget* parent = nullptr,
                           const QString& defaultSaveDir = QString(),
                           int defaultThreads = 8);

    NewTaskInput result() const { return m_result; }

    // 预填下载链接（用于拖放/剪贴板捕获后预填）
    void setUrl(const QString& url);

private slots:
    void onBrowseDir();
    void onAccepted();
    void onUrlChanged();
    void fetchFormats();
    void onFormatsReadyRead();
    void onFormatsFinished(int exitCode, QProcess::ExitStatus status);

private:
    void stopFormatProbe();
    bool validateSaveDir(QString* normalizedDir);

    NewTaskInput m_result;

    QLineEdit* m_urlEdit;
    QLineEdit* m_dirEdit;
    QLineEdit* m_nameEdit;
    QSpinBox*  m_threadSpin;

    QLabel*    m_formatLabel  = nullptr;
    QWidget*   m_formatWidget = nullptr;
    QComboBox* m_formatCombo  = nullptr;
    QPushButton* m_formatRefresh = nullptr;
    QProcess*  m_formatProc   = nullptr;
    QTimer*    m_formatDebounce = nullptr;
    QByteArray m_formatBuffer;
    QString    m_formatRequestUrl;
    bool       m_videoReady   = false;
};

#endif // NEW_TASK_DIALOG_H