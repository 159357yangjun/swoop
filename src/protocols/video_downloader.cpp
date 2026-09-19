#include "video_downloader.h"
#include "logger.h"
#include "app_paths.h"

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QStandardPaths>

// 静态成员初始化
QString VideoDownloader::s_ytDlpPath;

const QRegularExpression VideoDownloader::s_progressRe(
    QStringLiteral(R"(\[download\]\s+([\d.]+)%\s+of\s+([\d.]+)(KiB|MiB|GiB)\s+at\s+([\d.]+)(KiB|MiB|GiB)/s\s+ETA\s+(\d+):(\d+))")
);
const QRegularExpression VideoDownloader::s_destRe(
    QStringLiteral(R"(\[download\]\s+Destination:\s+(.+))")
);
const QRegularExpression VideoDownloader::s_completeRe(
    QStringLiteral(R"(\[download\]\s+100%\s+of\s+([\d.]+)(KiB|MiB|GiB))")
);

// 已知视频网站域名匹配（不区分大小写）
// 扩展时可在此添加更多站点
static const QStringList VIDEO_SITES = {
    QStringLiteral("youtube.com"),
    QStringLiteral("youtu.be"),
    QStringLiteral("bilibili.com"),
    QStringLiteral("b23.tv"),
    QStringLiteral("vimeo.com"),
    QStringLiteral("dailymotion.com"),
    QStringLiteral("twitch.tv"),
    QStringLiteral("nicovideo.jp"),
    QStringLiteral("tiktok.com"),
    QStringLiteral("douyin.com"),
    QStringLiteral("instagram.com"),
    QStringLiteral("facebook.com"),
    QStringLiteral("twitter.com"),
    QStringLiteral("x.com"),
    QStringLiteral("soundcloud.com"),
    QStringLiteral("pinterest.com"),
    QStringLiteral("reddit.com"),
    QStringLiteral("streamable.com"),
    QStringLiteral("pan.baidu.com"),  // 百度网盘分享页
};

VideoDownloader::VideoDownloader(QObject* parent)
    : IDownloader(parent)
{
}

// ── 静态方法 ──────────────────────────────────

void VideoDownloader::setYtDlpPath(const QString& path)
{
    s_ytDlpPath = path;
}

QString VideoDownloader::ytDlpPath()
{
    if (!s_ytDlpPath.isEmpty())
        return s_ytDlpPath;
    // 优先使用已捆绑/缓存的独立版
    QString bundled = bundledYtDlpPath();
    if (!bundled.isEmpty())
        return bundled;
    // 默认从 PATH 查找
    return QStringLiteral("yt-dlp");
}

QString VideoDownloader::bundledYtDlpPath()
{
    // 1. 应用同目录下的 yt-dlp/ 子目录（随安装包捆绑）
    QString candidate = QCoreApplication::applicationDirPath()
                        + QStringLiteral("/yt-dlp/yt-dlp.exe");
    if (QFile::exists(candidate))
        return candidate;
    // 2. 本地（便携模式 = exe 目录，否则 AppData/Local）
    QString local = AppPaths::dataDir();
    if (!local.isEmpty()) {
        candidate = local + QStringLiteral("/yt-dlp/yt-dlp.exe");
        if (QFile::exists(candidate))
            return candidate;
    }
    return QString();
}

bool VideoDownloader::isAvailable()
{
    QProcess proc;
    proc.start(ytDlpPath(), { QStringLiteral("--version") });
    if (!proc.waitForStarted(3000))
        return false;
    if (!proc.waitForFinished(5000))
        return false;
    return proc.exitCode() == 0;
}

bool VideoDownloader::isVideoUrl(const QString& url)
{
    QString lower = url.toLower();
    for (const auto& site : VIDEO_SITES) {
        if (lower.contains(site))
            return true;
    }
    return false;
}

// ── IDownloader 接口 ──────────────────────────

bool VideoDownloader::canHandle(const QString& url) const
{
    return isVideoUrl(url);
}

void VideoDownloader::start(const DownloadRequest& req)
{
    m_url      = req.url;
    m_savePath = req.savePath;
    m_fileName = req.fileName;
    m_cancelled = false;
    m_downloaded = 0;
    m_totalBytes = -1;
    m_lastPercent = 0;

    // 确保输出目录存在
    QDir().mkpath(m_savePath);

    // 构建 yt-dlp 命令参数
    // -o: 输出模板；--newline: 每行一个进度（便于解析）
    // --no-playlist: 只下载单个视频；-f: 格式选择
    QString outputTemplate;
    if (m_fileName.isEmpty())
        outputTemplate = m_savePath + QStringLiteral("/%(title)s.%(ext)s");
    else
        outputTemplate = m_savePath + QStringLiteral("/") + m_fileName;

    QStringList args = {
        QStringLiteral("--newline"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--no-warnings"),
        QStringLiteral("-o"), outputTemplate,
    };

    // 格式选择：默认最佳音视频合并
    QString format = req.extra.value(QStringLiteral("format")).toString();
    if (format.isEmpty())
        format = QStringLiteral("bestvideo+bestaudio/best");
    args << QStringLiteral("-f") << format;

    // 代理（可选）
    QString proxy = req.extra.value(QStringLiteral("proxy")).toString();
    if (!proxy.isEmpty())
        args << QStringLiteral("--proxy") << proxy;

    args << m_url;

    // 启动子进程
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);  // stdout + stderr 合并

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &VideoDownloader::onReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VideoDownloader::onFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &VideoDownloader::onErrorOccurred);

    Log::info(QStringLiteral("VideoDownloader 启动 yt-dlp: %1 %2")
                  .arg(ytDlpPath()).arg(args.join(' ')));

    emit stateChanged(m_taskId, 1);  // RUNNING

    m_process->start(ytDlpPath(), args);
    if (!m_process->waitForStarted(5000)) {
        emit completed(m_taskId, false, QStringLiteral("无法启动 yt-dlp，请确认已安装"));
        return;
    }
}

void VideoDownloader::setTaskId(int id)
{
    m_taskId = id;
}

void VideoDownloader::pause()
{
    // yt-dlp 不支持暂停/恢复，只能终止后重新开始
    // 这里做简单终止
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->terminate();
        emit stateChanged(m_taskId, 2);  // PAUSED
    }
}

void VideoDownloader::resume()
{
    // yt-dlp 无断点续传概念，恢复 = 重新下载
    if (!m_url.isEmpty())
        start({m_url, m_savePath, m_fileName, 1, false, {}});
}

void VideoDownloader::cancel()
{
    m_cancelled = true;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    emit stateChanged(m_taskId, 5);  // CANCELLED
}

// ── yt-dlp 输出解析 ───────────────────────────

void VideoDownloader::onReadyRead()
{
    while (m_process->canReadLine()) {
        QByteArray line = m_process->readLine();
        parseProgress(QString::fromUtf8(line).trimmed());
    }
}

void VideoDownloader::parseProgress(const QString& line)
{
    // 匹配进度行：[download]  xx.x% of xxx.xxMiB at xxx.xxMiB/s ETA mm:ss
    auto m = s_progressRe.match(line);
    if (m.hasMatch()) {
        double percent  = m.captured(1).toDouble();
        double sizeVal  = m.captured(2).toDouble();
        QString sizeUnit = m.captured(3);    // KiB / MiB / GiB
        double speedVal = m.captured(4).toDouble();
        QString speedUnit = m.captured(5);   // KiB / MiB / GiB
        int etaMin      = m.captured(6).toInt();
        int etaSec      = m.captured(7).toInt();

        // 总大小（字节）
        static const double KIB = 1024.0, MIB = 1048576.0, GIB = 1073741824.0;
        double multiplier = (sizeUnit == "GiB") ? GIB : (sizeUnit == "MiB") ? MIB : KIB;
        if (m_totalBytes < 0)
            m_totalBytes = static_cast<qint64>(sizeVal * multiplier);

        // 已下载字节
        m_downloaded = static_cast<qint64>(m_totalBytes * percent / 100.0);
        m_lastPercent = static_cast<int>(percent);

        // 速度（字节/秒）
        double sMul = (speedUnit == "GiB") ? GIB : (speedUnit == "MiB") ? MIB : KIB;
        m_speedBps = static_cast<int>(speedVal * sMul);

        // ETA（秒）
        m_etaSec = etaMin * 60 + etaSec;

        emit progressChanged(m_taskId, m_downloaded, m_totalBytes, m_speedBps);
        return;
    }

    // 匹配 100% 完成行
    auto cm = s_completeRe.match(line);
    if (cm.hasMatch()) {
        double sizeVal = cm.captured(1).toDouble();
        QString sizeUnit = cm.captured(2);
        static const double KIB = 1024.0, MIB = 1048576.0, GIB = 1073741824.0;
        double multiplier = (sizeUnit == "GiB") ? GIB : (sizeUnit == "MiB") ? MIB : KIB;
        m_totalBytes = static_cast<qint64>(sizeVal * multiplier);
        m_downloaded = m_totalBytes;
        emit progressChanged(m_taskId, m_downloaded, m_totalBytes, 0);
        return;
    }

    // 匹配目标文件行：[download] Destination: filename.ext
    auto dm = s_destRe.match(line);
    if (dm.hasMatch()) {
        Log::info(QStringLiteral("VideoDownloader 目标文件: %1").arg(dm.captured(1)));
    }

    // 其他日志行（调试用）
    if (line.startsWith(QStringLiteral("[download]")) ||
        line.startsWith(QStringLiteral("[Merger]")) ||
        line.startsWith(QStringLiteral("[ffmpeg]")))
        Log::debug(QStringLiteral("yt-dlp: %1").arg(line));
}

void VideoDownloader::onFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status);

    if (m_cancelled) {
        emit completed(m_taskId, false, QStringLiteral("已取消"));
        return;
    }

    if (exitCode == 0) {
        Log::info(QStringLiteral("VideoDownloader 下载完成: %1").arg(m_url));
        emit progressChanged(m_taskId, m_totalBytes > 0 ? m_totalBytes : m_downloaded,
                             m_totalBytes > 0 ? m_totalBytes : m_downloaded, 0);
        emit completed(m_taskId, true, QString());
        emit stateChanged(m_taskId, 3);  // COMPLETED
    } else {
        QString err = QStringLiteral("yt-dlp 退出码 %1").arg(exitCode);
        Log::error(QStringLiteral("VideoDownloader 失败: %1").arg(err));
        emit completed(m_taskId, false, err);
        emit stateChanged(m_taskId, 4);  // FAILED
    }
}

void VideoDownloader::onErrorOccurred(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart: msg = QStringLiteral("yt-dlp 启动失败，请确认已安装"); break;
    case QProcess::Crashed:       msg = QStringLiteral("yt-dlp 崩溃"); break;
    case QProcess::Timedout:      msg = QStringLiteral("yt-dlp 超时"); break;
    default:                      msg = QStringLiteral("yt-dlp 进程错误: %1").arg(error); break;
    }
    Log::error(QStringLiteral("VideoDownloader: %1").arg(msg));
    if (!m_cancelled) {
        emit completed(m_taskId, false, msg);
        emit stateChanged(m_taskId, 4);  // FAILED
    }
}
