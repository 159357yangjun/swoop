#ifndef BATCH_IMPORT_DIALOG_H
#define BATCH_IMPORT_DIALOG_H

#include <QDialog>
#include <QStringList>

class QPlainTextEdit;
class QLineEdit;
class QSpinBox;
class QLabel;

// 批量导入结果
struct BatchImportResult {
    QStringList urls;       // 有效的下载链接
    QString     saveDir;    // 统一保存目录
    int         threadCount; // 并发线程数
};

// 批量导入对话框：从多行文本或文本文件批量导入下载链接
// 每行一个 URL，自动过滤空行和注释行（# 开头）
class BatchImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit BatchImportDialog(QWidget* parent = nullptr,
                               const QString& defaultSaveDir = QString(),
                               int defaultThreads = 8);

    BatchImportResult result() const { return m_result; }

private slots:
    void onBrowseDir();
    void onImportFile();      // 从文本文件导入
    void onParseText();       // 解析文本框内容，更新统计
    void onAccepted();

private:
    BatchImportResult m_result;

    QPlainTextEdit* m_textEdit;
    QLineEdit*      m_dirEdit;
    QSpinBox*       m_threadSpin;
    QLabel*         m_countLabel;  // 显示有效链接数
};

#endif // BATCH_IMPORT_DIALOG_H
