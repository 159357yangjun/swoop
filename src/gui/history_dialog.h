#ifndef HISTORY_DIALOG_H
#define HISTORY_DIALOG_H

#include <QDialog>

class QTableWidget;
class QPushButton;
class QLabel;

// 下载历史查看 / 清空对话框（#46 的界面入口）
// 展示 SQLite 持久化的历史记录（HistoryStore::recent），支持刷新、清空、双击复制链接。
class HistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit HistoryDialog(QWidget* parent = nullptr);

private slots:
    void refresh();                       // 重新从 HistoryStore 拉取并填充表格
    void onClear();                      // 清空全部历史（带确认）
    void onRowDoubleClicked(int row, int column);  // 双击行 → 复制该记录下载链接

private:
    void setupUi();
    static QString formatSize(qint64 bytes);   // 字节 → 人类可读
    static QString statusText(const QString& status);  // queued/completed/failed → 中文

    QTableWidget* m_table      = nullptr;
    QPushButton*  m_refreshBtn = nullptr;
    QPushButton*  m_clearBtn   = nullptr;
    QLabel*       m_countLabel = nullptr;  // 底部：记录数 / 操作提示
};

#endif // HISTORY_DIALOG_H
