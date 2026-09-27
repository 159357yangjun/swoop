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
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

// 只对“看起来已经完整”的直接文件 URL 推断文件名。
// 视频页面（YouTube/Bilibili 等）必须留空，让 yt-dlp 根据标题/格式生成唯一输出名；
// 输入过程中也不能从 "h" / "https://exa" 这类半成品提前锁死文件名。
QString inferredFileNameForUrl(const QString& raw)
{
    const QString url = raw.trimmed();
    if (url.isEmpty() || VideoDownloader::isVideoUrl(url))
        return QString();

    QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid())
        return QString();

    const QString scheme = parsed.scheme().toLower();
    if ((scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
        && parsed.host().isEmpty())
        return QString();

    return parsed.fileName();
}

} // namespace

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

    // 用户完成一次 URL 编辑后再推断名称。这样逐字输入不会在第一个字符时写入错误文件名；
    // 对视频页面 inferredFileNameForUrl() 返回空，交给 yt-dlp 决定标题和扩展名。
    connect(m_urlEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_nameEdit->text().trimmed().isEmpty())
            return;
        const QString inferred = inferredFileNameForUrl(m_urlEdit->text());
        if (!inferred.isEmpty())
            m_nameEdit->setText(inferred);
    });

    m_threadSpin = new QSpinBox(this);
    m_threadSpin->setRange(1, 32);
    m_threadSpin->setValue(defaultThreads > 0 && defaultThreads <= 32 ? defaultThreads : 8);

    // 视频格式：仅当视频 URL 且 yt-dlp 可用时显示。
    // 输入框 textChanged 不再直接启动 yt-dlp；500ms 防抖后才探测，避免用户输入过程中
    // 连续创建/销毁多个外部进程造成卡顿和黑窗闪烁。
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

    m_formatDebounce = new QTimer(this);
    m_formatDebounce->setSingleShot(true);
    m_formatDebounce->setInterval(500);
    connect(m_formatDebounce, &QTimer::timeout, this, &NewTaskDialog::fetchFormats);

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
    if (m_nameEdit->text().trimmed().isEmpty()) {
        const QString name = inferredFileNameForUrl(url);
        if (!name.isEmpty())
            m_nameEdit->setText(name);
    }
    m_urlEdit->setFocus();
}

void NewTaskDialog::stopFormatProbe()
{
    if (m_formatDebounce)
        m_formatDebounce->stop();

    if (m_formatProc) {
        QProcess* proc = m_formatProc;
        m_formatProc = nullptr;
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
        proc->deleteLater();
    }

    m_formatBuffer.clear();
    m_formatRequestUrl.clear();
    if (m_formatCombo)
        m_formatCombo->setEnabled(true);
    if (m_formatRefresh) {
        m_formatRefresh->setEnabled(true);
        m_formatRefresh->setText(QStringLiteral("刷新格式"));
    }
}

void NewTaskDialog::onUrlChanged()
{
    QString url = m_urlEdit->text().trimmed();
    bool video = m_videoReady && VideoDownloader::isVideoUrl(url);
    m_formatLabel->setVisible(video);
    m_formatWidget->setVisible(video);

    if (!video) {
        stopFormatProbe();
        m_formatCombo->clear();
        m_formatCombo->addItem(QStringLiteral("最佳画质（自动合并音视频）"), QString());
        return;
    }

    if (m_formatRequestUrl != url && m_formatProc)
        stopFormatProbe();
    m_formatDebounce->start();
}

void NewTaskDialog::fetchFormats()
{
    if (!m_videoReady)
        return;

    QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty() || !VideoDownloader::isVideoUrl(url))
        return;

    if (m_formatDebounce)
        m_formatDebounce->stop();

    if (m_formatProc) {
        QProcess* old = m_formatProc;
        m_formatProc = nullptr;
        old->disconnect(this);
        if (old->state() != QProcess::NotRunning)
            old->kill();
        old->deleteLater();
    }

    m_formatBuffer.clear();
    m_formatRequestUrl = url;
    m_formatCombo->clear();
    m_formatCombo->addItem(QStringLiteral("最佳画质（自动合并音视频）"), QString());
    m_formatCombo->setEnabled(false);
    m_formatRefresh->setEnabled(false);
    m_formatRefresh->setText(QStringLiteral("读取中..."));

    QProcess* proc = new QProcess(this);
    m_formatProc = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyReadStandardOutput,
            this, &NewTaskDialog::onFormatsReadyRead);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &NewTaskDialog::onFormatsFinished);
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc](QProcess::ProcessError) {
                if (proc != m_formatProc)
                    return;
                m_formatProc = nullptr;
                m_formatCombo->setEnabled(true);
                m_formatRefresh->setEnabled(true);
                m_formatRefresh->setText(QStringLiteral("刷新格式"));
                m_formatCombo->setItemText(
                    0, QStringLiteral("最佳画质（格式列表读取失败，仍可直接下载）"));
                proc->deleteLater();
            });

    proc->start(VideoDownloader::ytDlpPath(),
                { QStringLiteral("--no-warnings"),
                  QStringLiteral("--skip-download"),
                  QStringLiteral("--list-formats"), url });
}

void NewTaskDialog::onFormatsReadyRead()
{
    QProcess* proc = qobject_cast<QProcess*>(sender());
    if (proc && proc == m_formatProc)
        m_formatBuffer.append(proc->readAllStandardOutput());
}

void NewTaskDialog::onFormatsFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess* proc = qobject_cast<QProcess*>(sender());
    if (!proc || proc != m_formatProc) {
        if (proc)
            proc->deleteLater();
        return;
    }

    m_formatProc = nullptr;
    proc->deleteLater();
    m_formatCombo->setEnabled(true);
    m_formatRefresh->setEnabled(true);
    m_formatRefresh->setText(QStringLiteral("刷新格式"));

    if (m_formatRequestUrl != m_urlEdit->text().trimmed()) {
        m_formatBuffer.clear();
        return;
    }

    if (status != QProcess::NormalExit || exitCode != 0) {
        m_formatBuffer.clear();
        m_formatCombo->setItemText(
            0, QStringLiteral("最佳画质（格式列表读取失败，仍可直接下载）"));
        return;
    }

    QString text = QString::fromUtf8(m_formatBuffer);
    m_formatBuffer.clear();
    QStringList lines = text.split(QLatin1Char('\n'));
    QRegularExpression idRe(QStringLiteral(R"(^[a-zA-Z0-9*+]+$)"));
    for (const QString& raw : lines) {
        QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('[')) || line.isEmpty())
            continue;
        QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        QString id  = parts[0];
        QString ext = parts[1];
        QString res = parts[2];
        if (id == QStringLiteral("ID"))
            continue;
        if (!idRe.match(id).hasMatch())
            continue;
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

bool NewTaskDialog::validateSaveDir(QString* normalizedDir)
{
    QString saveDir = QDir::cleanPath(m_dirEdit->text().trimmed());
    if (saveDir.isEmpty() || saveDir == QStringLiteral(".")) {
        QMessageBox::warning(this, QStringLiteral("保存目录"),
                             QStringLiteral("请选择一个有效的保存目录。"));
        return false;
    }

    QFileInfo info(saveDir);
    if (info.exists() && !info.isDir()) {
        QMessageBox::warning(this, QStringLiteral("保存目录"),
                             QStringLiteral("保存路径指向的是文件，不是文件夹。"));
        return false;
    }
    if (!info.exists() && !QDir().mkpath(saveDir)) {
        QMessageBox::warning(this, QStringLiteral("保存目录"),
                             QStringLiteral("无法创建保存目录：\n%1").arg(saveDir));
        return false;
    }

    info.setFile(saveDir);
    if (!info.isDir() || !info.isWritable()) {
        QMessageBox::warning(this, QStringLiteral("保存目录"),
                             QStringLiteral("保存目录不可写：\n%1").arg(saveDir));
        return false;
    }

    if (normalizedDir)
        *normalizedDir = QDir(saveDir).absolutePath();
    return true;
}

void NewTaskDialog::onAccepted()
{
    QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请填写下载链接"));
        return;
    }

    // 使用 StrictMode 校验用户实际输入的 URL，不把 `example.com/file` 静默转换成 HTTP
    // 后又把原字符串交给下载引擎，避免“对话框判定有效、引擎收到的却仍无 scheme”。
    bool aria2Input = TorrentDownloader::isAria2Url(url);
    QUrl parsed(url, QUrl::StrictMode);
    QString scheme = parsed.scheme().toLower();
    bool httpInput = parsed.isValid()
                     && (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
                     && !parsed.host().isEmpty();
    bool localTorrent = QFile::exists(url)
                        && url.endsWith(QStringLiteral(".torrent"), Qt::CaseInsensitive);
    if (!aria2Input && !httpInput && !localTorrent) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("链接格式不正确。支持 HTTP/HTTPS、FTP/FTPS、magnet 磁力链接和 .torrent 文件。\n例如：https://example.com/file.zip"));
        return;
    }

    QString saveDir;
    if (!validateSaveDir(&saveDir))
        return;

    m_result.url = url;
    m_result.saveDir = saveDir;
    m_result.fileName = m_nameEdit->text().trimmed();
    m_result.threadCount = m_threadSpin->value();
    m_result.format = m_formatWidget->isVisible()
        ? m_formatCombo->currentData().toString() : QString();
    accept();
}
