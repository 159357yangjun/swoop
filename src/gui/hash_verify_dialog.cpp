#include "hash_verify_dialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QCryptographicHash>
#include <QFile>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>

HashVerifyDialog::HashVerifyDialog(QWidget* parent, const QString& suggestedFilePath)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("文件完整性校验"));
    setMinimumWidth(480);

    auto* mainLayout = new QVBoxLayout(this);

    // 文件路径
    auto* fileLayout = new QHBoxLayout;
    m_fileEdit = new QLineEdit(this);
    m_fileEdit->setText(suggestedFilePath);
    m_fileEdit->setPlaceholderText(QStringLiteral("选择要校验的文件..."));
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."), this);
    fileLayout->addWidget(new QLabel(QStringLiteral("文件:"), this));
    fileLayout->addWidget(m_fileEdit);
    fileLayout->addWidget(browseBtn);
    mainLayout->addLayout(fileLayout);

    // 算法 + 预期哈希
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    m_algoCombo = new QComboBox(this);
    m_algoCombo->addItem(QStringLiteral("MD5"),    static_cast<int>(QCryptographicHash::Md5));
    m_algoCombo->addItem(QStringLiteral("SHA-1"),  static_cast<int>(QCryptographicHash::Sha1));
    m_algoCombo->addItem(QStringLiteral("SHA-256"),static_cast<int>(QCryptographicHash::Sha256));
    m_algoCombo->setCurrentIndex(2);  // 默认 SHA-256

    m_expectedEdit = new QLineEdit(this);
    m_expectedEdit->setPlaceholderText(QStringLiteral("粘贴文件的预期哈希值（可选，留空则仅计算）"));
    m_expectedEdit->setClearButtonEnabled(true);

    form->addRow(QStringLiteral("哈希算法:"), m_algoCombo);
    form->addRow(QStringLiteral("预期值:"), m_expectedEdit);
    mainLayout->addLayout(form);

    // 校验按钮
    m_verifyBtn = new QPushButton(QStringLiteral("计算并校验"), this);
    m_verifyBtn->setMinimumHeight(32);
    mainLayout->addWidget(m_verifyBtn);

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    // 结果
    auto* resultLayout = new QVBoxLayout;
    auto* computedLabel = new QLabel(QStringLiteral("计算结果:"), this);
    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_resultLabel->setStyleSheet(QStringLiteral("font-family: monospace; padding: 4px; "
                                                "background: #f5f5f5; border: 1px solid #ddd;"));
    m_resultLabel->setMinimumHeight(28);

    m_matchLabel = new QLabel(this);
    m_matchLabel->setAlignment(Qt::AlignCenter);
    m_matchLabel->setMinimumHeight(32);
    m_matchLabel->setVisible(false);

    auto* copyBtn = new QPushButton(QStringLiteral("复制结果"), this);

    resultLayout->addWidget(computedLabel);
    resultLayout->addWidget(m_resultLabel);
    resultLayout->addWidget(copyBtn);
    resultLayout->addWidget(m_matchLabel);
    mainLayout->addLayout(resultLayout);

    mainLayout->addStretch();

    // 关闭按钮
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    // 连接
    connect(browseBtn, &QPushButton::clicked, this, &HashVerifyDialog::onBrowseFile);
    connect(m_verifyBtn, &QPushButton::clicked, this, &HashVerifyDialog::onVerify);
    connect(copyBtn, &QPushButton::clicked, this, &HashVerifyDialog::onCopyComputed);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

void HashVerifyDialog::onBrowseFile()
{
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择文件"), m_fileEdit->text());
    if (!path.isEmpty())
        m_fileEdit->setText(path);
}

void HashVerifyDialog::onVerify()
{
    QString filePath = m_fileEdit->text().trimmed();
    if (filePath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择文件"));
        return;
    }

    QFile f(filePath);
    if (!f.open(QFile::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件: %1").arg(filePath));
        return;
    }

    int algo = m_algoCombo->currentData().toInt();
    qint64 fileSize = f.size();
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 100);
    m_verifyBtn->setEnabled(false);
    m_matchLabel->setVisible(false);

    // 分块读取计算哈希（支持大文件）
    QCryptographicHash hash(static_cast<QCryptographicHash::Algorithm>(algo));
    const qint64 blockSize = 4 * 1024 * 1024;  // 4MB per block
    qint64 totalRead = 0;

    while (!f.atEnd()) {
        QByteArray chunk = f.read(blockSize);
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        totalRead += chunk.size();
        if (fileSize > 0)
            m_progressBar->setValue(static_cast<int>(totalRead * 100 / fileSize));
        QCoreApplication::processEvents();  // 保持 UI 响应
    }
    f.close();

    QByteArray result = hash.result().toHex();
    m_resultLabel->setText(QString::fromLatin1(result));
    m_progressBar->setVisible(false);
    m_verifyBtn->setEnabled(true);

    // 比对预期值
    QString expected = m_expectedEdit->text().trimmed().toLower();
    if (!expected.isEmpty()) {
        QString computed = QString::fromLatin1(result).toLower();
        if (computed == expected) {
            m_matchLabel->setText(QStringLiteral("✓ 校验通过 — 哈希值匹配"));
            m_matchLabel->setStyleSheet(
                QStringLiteral("background: #27ae60; color: white; font-weight: bold; "
                               "border-radius: 4px; padding: 8px; font-size: 14px;"));
        } else {
            m_matchLabel->setText(QStringLiteral("✗ 校验失败 — 哈希值不匹配！\n"
                                                 "预期: %1\n实际: %2")
                                      .arg(expected)
                                      .arg(computed));
            m_matchLabel->setStyleSheet(
                QStringLiteral("background: #c0392b; color: white; font-weight: bold; "
                               "border-radius: 4px; padding: 8px; font-size: 14px;"));
        }
        m_matchLabel->setVisible(true);
    } else {
        m_matchLabel->setText(QStringLiteral("已计算完成（未输入预期值，跳过比对）"));
        m_matchLabel->setStyleSheet(
            QStringLiteral("background: #f39c12; color: white; font-weight: bold; "
                           "border-radius: 4px; padding: 8px;"));
        m_matchLabel->setVisible(true);
    }
}

void HashVerifyDialog::onCopyComputed()
{
    QString hash = m_resultLabel->text();
    if (hash.isEmpty()) return;
    QApplication::clipboard()->setText(hash);
    m_resultLabel->setToolTip(QStringLiteral("已复制到剪贴板"));
}

QString HashVerifyDialog::computeHash(const QString& filePath, int algorithm)
{
    QFile f(filePath);
    if (!f.open(QFile::ReadOnly))
        return QString();

    QCryptographicHash hash(static_cast<QCryptographicHash::Algorithm>(algorithm));
    if (!hash.addData(&f))
        return QString();

    f.close();
    return QString::fromLatin1(hash.result().toHex());
}
