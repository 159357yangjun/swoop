#include "batch_import_dialog.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QTextStream>
#include <QUrl>
#include <QStandardPaths>
#include <QMessageBox>

BatchImportDialog::BatchImportDialog(QWidget* parent, const QString& defaultSaveDir, int defaultThreads)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("批量导入下载任务"));
    setMinimumSize(520, 400);

    auto* mainLayout = new QVBoxLayout(this);

    // URL 文本框
    auto* urlLabel = new QLabel(QStringLiteral("下载链接（每行一个 URL）："), this);
    mainLayout->addWidget(urlLabel);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setPlaceholderText(QStringLiteral(
        "https://example.com/file1.zip\n"
        "https://example.com/file2.zip\n"
        "# 以 # 开头的行会被忽略\n"
        "magnet:?xt=urn:btih:..."));
    m_textEdit->setMinimumHeight(160);
    mainLayout->addWidget(m_textEdit);

    // 按钮行：导入文件 + 解析统计
    auto* btnLayout = new QHBoxLayout;
    auto* importBtn = new QPushButton(QStringLiteral("从文件导入..."), this);
    m_countLabel = new QLabel(QStringLiteral("有效链接: 0"), this);
    btnLayout->addWidget(importBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_countLabel);
    mainLayout->addLayout(btnLayout);

    // 保存目录
    auto* dirLayout = new QHBoxLayout;
    m_dirEdit = new QLineEdit(this);
    m_dirEdit->setText(defaultSaveDir.isEmpty()
                       ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                       : defaultSaveDir);
    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."), this);
    dirLayout->addWidget(new QLabel(QStringLiteral("保存目录:"), this));
    dirLayout->addWidget(m_dirEdit);
    dirLayout->addWidget(browseBtn);
    mainLayout->addLayout(dirLayout);

    // 线程数
    auto* threadLayout = new QHBoxLayout;
    m_threadSpin = new QSpinBox(this);
    m_threadSpin->setRange(1, 32);
    m_threadSpin->setValue(defaultThreads > 0 ? defaultThreads : 8);
    threadLayout->addWidget(new QLabel(QStringLiteral("每个任务线程数:"), this));
    threadLayout->addWidget(m_threadSpin);
    threadLayout->addStretch();
    mainLayout->addLayout(threadLayout);

    // OK / Cancel
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);

    // 连接
    connect(importBtn, &QPushButton::clicked, this, &BatchImportDialog::onImportFile);
    connect(m_textEdit, &QPlainTextEdit::textChanged, this, &BatchImportDialog::onParseText);
    connect(browseBtn, &QPushButton::clicked, this, &BatchImportDialog::onBrowseDir);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &BatchImportDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    onParseText();  // 初始统计
}

void BatchImportDialog::onBrowseDir()
{
    QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择保存目录"), m_dirEdit->text());
    if (!dir.isEmpty())
        m_dirEdit->setText(dir);
}

void BatchImportDialog::onImportFile()
{
    QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入链接文件"),
                                                 QString(),
                                                 QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (!f.open(QFile::ReadOnly | QFile::Text)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("无法打开文件: %1").arg(path));
        return;
    }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    QString content = in.readAll();

    // 追加到文本框（不覆盖已有内容）
    if (!m_textEdit->toPlainText().trimmed().isEmpty())
        m_textEdit->appendPlainText(content);
    else
        m_textEdit->setPlainText(content);
}

void BatchImportDialog::onParseText()
{
    // 解析文本框内容，统计有效链接数
    QStringList lines = m_textEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
    int count = 0;
    for (const QString& line : lines) {
        QString url = line.trimmed();
        if (url.isEmpty() || url.startsWith('#'))
            continue;
        // 简单 URL 校验
        if (url.startsWith("http://", Qt::CaseInsensitive) ||
            url.startsWith("https://", Qt::CaseInsensitive) ||
            url.startsWith("ftp://", Qt::CaseInsensitive) ||
            url.startsWith("magnet:", Qt::CaseInsensitive))
            ++count;
    }
    m_countLabel->setText(QStringLiteral("有效链接: %1").arg(count));
}

void BatchImportDialog::onAccepted()
{
    // 解析并收集有效 URL
    QStringList lines = m_textEdit->toPlainText().split('\n', Qt::SkipEmptyParts);
    QStringList urls;
    for (const QString& line : lines) {
        QString url = line.trimmed();
        if (url.isEmpty() || url.startsWith('#'))
            continue;
        if (url.startsWith("http://", Qt::CaseInsensitive) ||
            url.startsWith("https://", Qt::CaseInsensitive) ||
            url.startsWith("ftp://", Qt::CaseInsensitive) ||
            url.startsWith("magnet:", Qt::CaseInsensitive))
            urls << url;
    }

    if (urls.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无有效链接"),
                             QStringLiteral("文本框中没有有效的下载链接"));
        return;
    }

    m_result.urls       = urls;
    m_result.saveDir    = m_dirEdit->text().trimmed();
    m_result.threadCount = m_threadSpin->value();
    accept();
}
