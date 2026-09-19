#ifndef HASH_VERIFY_DIALOG_H
#define HASH_VERIFY_DIALOG_H

#include <QDialog>
#include <QString>

class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;

// 文件完整性校验对话框
// 支持 MD5 / SHA1 / SHA256 三种哈希算法
// 用户选择文件 → 输入预期哈希值 → 点击校验 → 计算并比对
class HashVerifyDialog : public QDialog {
    Q_OBJECT
public:
    explicit HashVerifyDialog(QWidget* parent = nullptr,
                              const QString& suggestedFilePath = QString());

private slots:
    void onBrowseFile();
    void onVerify();        // 计算哈希并比对
    void onCopyComputed();  // 复制计算结果到剪贴板

private:
    QString computeHash(const QString& filePath, int algorithm);

    QLineEdit*   m_fileEdit;       // 文件路径
    QLineEdit*   m_expectedEdit;   // 预期哈希值
    QComboBox*   m_algoCombo;      // 算法选择
    QLabel*      m_resultLabel;    // 计算结果
    QLabel*      m_matchLabel;     // 比对结果（匹配/不匹配）
    QPushButton* m_verifyBtn;
    QProgressBar* m_progressBar;   // 大文件计算进度
};

#endif // HASH_VERIFY_DIALOG_H
