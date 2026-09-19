#include "new_task_dialog.h"
#include "video_downloader.h"
#include "torrent_downloader.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QUrl>
#include <QProcess>
#include <QRegularExpression>

NewTaskDialog::NewTaskDialog(QWidget* parent, const QString& defaultSaveDir, int defaultThreads)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新建下载任务"));
    setMinimumWidth(560);

    m_urlEdit = new QLineEdit(this);
    m_urlEdit->setPlaceholderText(QStringLiteral("粘贴下载链接，例如 https://example.com/file.zip"));
    connect(m_urlEdit, &QLineEdit::textChanged, this, &NewTaskDialog::onUrlChanged);

    m_dirEdit = new QLineEdit(this);
    m_dirEdit->setText(defaultSaveDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
        : defaultSaveDir);

    auto* browseBtn = new QPushButton(QStringLiteral("浏览..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &NewTaskDialog::onBrowseDir);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(QStringLiteral("可选，留空自动从链接推断"));

    m_threadSpin = new QSpinBox(this);
    m_threadSpin->setRange(1, 32);
    m_threadSpin->setValue(defaultThreads > 0 && defaultThreads <= 32 ? defaultThreads : 8);

    // 视频格式：仅当视频 URL 且 yt-dlp 可用时显示
    m_formatCombo = new QComboBox(this);
    m_formatCombo->setMinimumWidth(220);
    m_formatCombo->addItem(QStringLiteral("最佳画质（自动合并音视频）"), QString());
    m_formatRefresh = new QPushButton(QStringLiteral("刷新格式"), this);
    connect(m_formatRefresh, &QPushButton::clicked, this, &NewTaskDialog::fetchFormats);
    auto* fmtRow = new QHBoxLayout;
    fmtRow->addWidget(m_formatCombo, 1);
    fmtRow->addWidget(m_formatRefresh);
    m_formatWidget = new QWidget(this);
    m_formatWidget->setLayout(fmtRow);
    m_formatLabel = new QLabel(QStringLiteral("视频格式:"), this);
    m_formatLabel->setVisible(false);
    m_formatWidget->setVisible(false);
    m_videoReady = VideoDownloader::isAvailable();

    auto* dirLayout = new QHBoxLayout;
    dirLayout->addWidget(m_dirEdit, 1);
    dirLayout->addWidget(browseBtn);

    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("下载链接:"), m_urlEdit);
    form->addRow(QStringLiteral("保存目录:"), dirLayout);
    form->addRow(QStringLiteral("文件名:"),   m_nameEdit);
    form->addRow(QStringLiteral("线程数:"),   m_threadSpin);
    form->addRow(m_formatLabel, m_formatWidget);

    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &NewTaskDialog::onAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(form);
    mainLayout->addWidget(buttonBox);
}

void NewTaskDialog::setUrl(const QString& url)
{
    m_urlEdit->setText(url);
    // 如果文件名为空，尝试从 URL 推断文件名预填
    if (m_nameEdit->text().isEmpty()) {
        QString name = QUrl(url).fileName();
        if (!name.isEmpty())
            m_nameEdit->setText(name);
    }
    m_urlEdit->setFocus();
    onUrlChanged();  // 触发视频格式刷新
}

void NewTaskDialog::onUrlChanged()
{
    QString url = m_urlEdit->text().trimmed();
    bool video = m_videoReady && VideoDownloader::isVideoUrl(url);
    m_formatLabel->setVisible(video);
    m_formatWidget->setVisible(video);
    if (video)
        fetchFormats();
}

void NewTaskDialog::fetchFormats()
{
    if (m_formatProc) { m_formatProc->deleteLater(); m_formatProc = nullptr; }
    m_formatBuffer.clear();
    m_formatCombo->clear();
    m_formatCombo->addItem(QStringLiteral("最佳画质（自动合并音视频）"), QString());
    m_formatCombo->setEnabled(false);

    QString url = m_urlEdit->text().trimmed();
    m_formatProc = new QProcess(this);
    m_formatProc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_formatProc, &QProcess::readyReadStandardOutput,
            this, &NewTaskDialog::onFormatsReadyRead);
    connect(m_formatProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &NewTaskDialog::onFormatsFinished);
    m_formatProc->start(VideoDownloader::ytDlpPath(),
                        { QStringLiteral("--no-warnings"),
                          QStringLiteral("--skip-download"),
                          QStringLiteral("--list-formats"), url });
}

void NewTaskDialog::onFormatsReadyRead()
{
    if (m_formatProc)
        m_formatBuffer.append(m_formatProc->readAllStandardOutput());
}

void NewTaskDialog::onFormatsFinished(int, QProcess::ExitStatus)
{
    m_formatCombo->setEnabled(true);
    if (m_formatProc) { m_formatProc->deleteLater(); m_formatProc = nullptr; }

    QString text = QString::fromUtf8(m_formatBuffer);
    m_formatBuffer.clear();
    QStringList lines = text.split(QLatin1Char('\n'));
    QRegularExpression idRe(QStringLiteral(R"(^[a-zA-Z0-9*+]+$)"));
    for (const QString& raw : lines) {
        QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('[')) || line.isEmpty())
            continue;  // 跳过 [info]/[youtube] 等日志行
        QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        QString id  = parts[0];
        QString ext = parts[1];
        QString res = parts[2];
        if (id == QStringLiteral("ID"))
            continue;  // 表头
        if (!idRe.match(id).hasMatch())
            continue;  // 非格式 ID 行
        QString label = QStringLiteral("%1  ·  %2  ·  .%3").arg(id, res, ext);
        m_formatCombo->addItem(label, id);
    }
}

void NewTaskDialog::onBrowseDir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择保存目录"), m_dirEdit->text());
    if (!dir.isEmpty())
        m_dirEdit->setText(dir);
}

void NewTaskDialog::onAccepted()
{
    QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请填写下载链接"));
        return;
    }
    // 允许 http(s)/ftp 等带 "://" 的链接，以及 magnet: 磁力链接与本地/远程 .torrent 文件
    bool looksValid = url.contains(QStringLiteral("://"))
                      || TorrentDownloader::isTorrentUrl(url)
                      || QFile::exists(url);
    if (!looksValid) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("链接格式不正确，应为 http(s):// 链接、magnet: 磁力链接或 .torrent 文件"));
        return;
    }
    m_result.url = url;
    m_result.saveDir = m_dirEdit->text().trimmed();
    QString name = m_nameEdit->text().trimmed();
    m_result.fileName = name;
    m_result.threadCount = m_threadSpin->value();
    m_result.format = m_formatWidget->isVisible()
        ? m_formatCombo->currentData().toString() : QString();
    accept();
}
