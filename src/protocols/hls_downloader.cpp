#include "hls_downloader.h"
#include "aes128.h"
#include "logger.h"
#include "app_paths.h"
#include "tool_probe.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QStandardPaths>

QString HlsDownloader::s_ffmpegPath;

HlsDownloader::HlsDownloader(QObject* parent)
    : IDownloader(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

// ── 静态：ffmpeg 定位 ──────────────────────────
void HlsDownloader::setFfmpegPath(const QString& path) { s_ffmpegPath = path; }

QString HlsDownloader::ffmpegPath()
{
    if (!s_ffmpegPath.isEmpty())
        return s_ffmpegPath;
    // 应用同目录的 ffmpeg/ 子目录（随安装包捆绑）
    QString app = QCoreApplication::applicationDirPath()
                  + QStringLiteral("/ffmpeg/ffmpeg.exe");
    if (QFile::exists(app))
        return app;
    // 本地（便携模式 = exe 目录，否则 AppData/Local）
    QString local = AppPaths::dataDir();
    if (!local.isEmpty()) {
        QString c = local + QStringLiteral("/ffmpeg/ffmpeg.exe");
        if (QFile::exists(c))
            return c;
    }
    return QStringLiteral("ffmpeg");  // 回退 PATH
}

bool HlsDownloader::isFfmpegAvailable()
{
    /* 同步 spawn 一次外部工具要 0.06~1.34s（yt-dlp 最狠），而这里过去每次调用都跑一遍。
       记忆判据见 src/utils/tool_probe.h。 */
    return ToolProbe::available(ffmpegPath(), { QStringLiteral("-version") });
}

bool HlsDownloader::isHlsUrl(const QString& url)
{
    return url.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive);
}

// ── IDownloader 接口 ──────────────────────────
bool HlsDownloader::canHandle(const QString& url) const
{
    return isHlsUrl(url);
}

void HlsDownloader::setTaskId(int id) { m_taskId = id; }

void HlsDownloader::start(const DownloadRequest& req)
{
    m_url      = req.url;
    m_saveDir  = req.savePath;
    m_fileName = req.fileName;
    m_extra    = req.extra;
    m_forcedVariant = req.extra.value(QStringLiteral("hlsVariant")).toString();

    m_cancelled = false;
    m_paused    = false;
    m_finished  = false;
    m_started   = false;
    m_segments.clear();
    m_downloadedBytes = 0;
    m_totalDuration   = 0.0;
    m_active = 0;
    m_doneCount = 0;
    m_encrypted = false;
    m_keyUrl.clear();
    m_key.clear();
    m_iv.clear();
    m_ivPresent = false;
    if (m_keyReply) { m_keyReply->deleteLater(); m_keyReply = nullptr; }

    QDir().mkpath(m_saveDir);

    Log::info(QStringLiteral("HlsDownloader 启动: %1").arg(m_url));
    emit stateChanged(m_taskId, 1);  // RUNNING
    fetchPlaylist(m_url, false);
}

void HlsDownloader::pause()
{
    if (m_merge && m_merge->state() == QProcess::Running) {
        m_merge->terminate();
        emit stateChanged(m_taskId, 2);  // PAUSED
        return;
    }
    // 手动模式：中止在途分片
    for (auto& s : m_segments)
        if (s.reply) { s.reply->abort(); s.reply = nullptr; }
    m_active = 0;
    m_paused = true;
    emit stateChanged(m_taskId, 2);
}

void HlsDownloader::resume()
{
    if (m_cancelled)
        return;
    m_cancelled = false;
    m_paused    = false;
    // HLS 不支持断点续传，恢复 = 从播放列表重新拉取（与 yt-dlp 后端一致）
    if (m_started)
        start({m_url, m_saveDir, m_fileName, 1, false, m_extra});
}

void HlsDownloader::cancel()
{
    m_cancelled = true;
    if (m_merge && m_merge->state() == QProcess::Running)
        m_merge->kill();
    if (m_keyReply)
        m_keyReply->abort();
    for (auto& s : m_segments)
        if (s.reply) s.reply->abort();
    emit stateChanged(m_taskId, 5);  // CANCELLED
}

// ── 播放列表拉取与解析 ────────────────────────
void HlsDownloader::fetchPlaylist(const QString& url, bool isMedia)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));

    QNetworkReply* r = m_nam->get(req);
    // 捕获当前请求的上下文，避免重定向后 base 失真
    QString base = url;
    connect(r, &QNetworkReply::finished, this, [this, r, base, isMedia]() {
        r->deleteLater();
        if (m_cancelled)
            return;
        if (r->error() != QNetworkReply::NoError) {
            finish(false, r->errorString());
            return;
        }
        QByteArray data = r->readAll();
        QString text = QString::fromUtf8(data)
                           .replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
                           .replace(QStringLiteral("\r"),   QStringLiteral("\n"));
        if (!isMedia && text.contains(QStringLiteral("#EXT-X-STREAM-INF")))
            parseMasterPlaylist(text, base);
        else
            parseMediaPlaylist(text, base);
    });
}

void HlsDownloader::parseMasterPlaylist(const QString& text, const QString& base)
{
    const QStringList lines = text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);
    struct Variant { qint64 bw; QString uri; };
    QList<Variant> variants;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (!line.startsWith(QStringLiteral("#EXT-X-STREAM-INF:")))
            continue;
        qint64 bw = 0;
        QRegularExpression re(QStringLiteral("BANDWIDTH=(\\d+)"));
        auto m = re.match(line);
        if (m.hasMatch())
            bw = m.captured(1).toLongLong();
        // 下一行非注释即媒体播放列表 URI
        QString uri;
        for (int j = i + 1; j < lines.size(); ++j) {
            QString l = lines[j].trimmed();
            if (l.isEmpty() || l.startsWith('#'))
                continue;
            uri = l;
            break;
        }
        if (!uri.isEmpty())
            variants.append({bw, uri});
    }

    if (variants.isEmpty()) {
        finish(false, QStringLiteral("无法解析主播放列表（缺少变体）"));
        return;
    }

    Variant chosen = variants.first();
    for (const auto& v : variants)
        if (v.bw > chosen.bw)
            chosen = v;
    if (!m_forcedVariant.isEmpty()) {
        for (const auto& v : variants)
            if (QString::number(v.bw) == m_forcedVariant) { chosen = v; break; }
    }

    QString mediaUrl = resolveUrl(base, chosen.uri);
    Log::info(QStringLiteral("HlsDownloader 选定变体（带宽 %1）→ %2")
                  .arg(chosen.bw).arg(mediaUrl));
    fetchPlaylist(mediaUrl, true);
}

void HlsDownloader::parseMediaPlaylist(const QString& text, const QString& base)
{
    m_mediaPlaylistUrl = base;
    const QStringList lines = text.split(QStringLiteral("\n"), Qt::SkipEmptyParts);

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();

        if (line.startsWith(QStringLiteral("#EXT-X-KEY:"))) {
            if (line.contains(QStringLiteral("METHOD=NONE"))) {
                m_encrypted = false;   // 显式关闭加密
            } else if (line.contains(QStringLiteral("METHOD=AES-128"))) {
                m_encrypted = true;
                // 提取 URI="..."（密钥地址，通常相对媒体播放列表）
                QRegularExpression reUri(QStringLiteral("URI=\"([^\"]*)\""));
                auto m = reUri.match(line);
                if (m.hasMatch())
                    m_keyUrl = m.captured(1);
                // 提取可选 IV=0x...（16 字节）。无 IV 时按分片序号作为 IV（spec 默认）
                QRegularExpression reIv(QStringLiteral("IV=0x([0-9A-Fa-f]+)"));
                auto mi = reIv.match(line);
                if (mi.hasMatch()) {
                    m_iv = QByteArray::fromHex(mi.captured(1).toUtf8());
                    m_ivPresent = (m_iv.size() == 16);
                } else {
                    m_ivPresent = false;
                }
            }
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXTINF:"))) {
            QString durStr = line.mid(8);  // 去掉 "#EXTINF:"
            if (durStr.endsWith(','))
                durStr.chop(1);
            m_totalDuration += durStr.toDouble();
            // 下一行非注释即分片 URI
            QString uri;
            for (int j = i + 1; j < lines.size(); ++j) {
                QString l = lines[j].trimmed();
                if (l.isEmpty() || l.startsWith('#'))
                    continue;
                uri = l;
                break;
            }
            if (!uri.isEmpty()) {
                Segment s;
                s.url  = resolveUrl(base, uri);
                s.done = false;
                s.reply = nullptr;
                m_segments.append(s);
            }
            continue;
        }
        // #EXT-X-ENDLIST 等其它标签忽略
    }

    if (m_segments.isEmpty()) {
        finish(false, QStringLiteral("播放列表中未找到分片"));
        return;
    }
    Log::info(QStringLiteral("HlsDownloader 解析到 %1 个分片（加密=%2，时长≈%3s）")
                  .arg(m_segments.size()).arg(m_encrypted).arg(int(m_totalDuration)));
    beginDownload();
}

QString HlsDownloader::resolveUrl(const QString& base, const QString& ref) const
{
    if (ref.startsWith(QStringLiteral("http://"),  Qt::CaseInsensitive) ||
        ref.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
        return ref;
    QUrl b(base), r(ref);
    return b.resolved(r).toString();
}

// ── 合并阶段 ──────────────────────────────────
void HlsDownloader::beginDownload()
{
    m_started = true;
    emit stateChanged(m_taskId, 1);
    if (isFfmpegAvailable()) {
        startFfmpegDownload();            // ffmpeg 自带 AES-128 解密 + MP4 转封装
    } else if (m_encrypted) {
        fetchKey();                       // 无 ffmpeg：纯 C++ 拉密钥 → 解密下载
    } else {
        startManualDownload();
    }
}

void HlsDownloader::startFfmpegDownload()
{
    QString out = outputPath(QStringLiteral("mp4"));
    QDir().mkpath(QFileInfo(out).absolutePath());

    QStringList args;
    args << QStringLiteral("-y")
         << QStringLiteral("-loglevel") << QStringLiteral("info")
         << QStringLiteral("-i") << m_mediaPlaylistUrl
         << QStringLiteral("-c") << QStringLiteral("copy")
         << out;

    m_merge = new QProcess(this);
    m_merge->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_merge, &QProcess::readyReadStandardOutput,
            this, &HlsDownloader::onMergeReadyRead);
    connect(m_merge, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &HlsDownloader::onMergeFinished);

    Log::info(QStringLiteral("HlsDownloader 启动 ffmpeg: %1 %2")
                  .arg(ffmpegPath()).arg(args.join(' ')));
    m_merge->start(ffmpegPath(), args);
    if (!m_merge->waitForStarted(5000))
        finish(false, QStringLiteral("无法启动 ffmpeg，请确认已安装并位于 PATH 或捆绑目录"));
}

void HlsDownloader::onMergeReadyRead()
{
    if (!m_merge)
        return;
    while (m_merge->canReadLine()) {
        QString line = QString::fromUtf8(m_merge->readLine());
        // ffmpeg 输出： frame=... fps=... size=... time=00:01:23.45 bitrate=... speed=...
        QRegularExpression re(QStringLiteral("time=(\\d+):(\\d+):(\\d+\\.?\\d*)"));
        auto m = re.match(line);
        if (m.hasMatch()) {
            int h  = m.captured(1).toInt();
            int mm = m.captured(2).toInt();
            double sec = m.captured(3).toDouble();
            double cur = h * 3600 + mm * 60 + sec;
            int pct = m_totalDuration > 0 ? int(cur / m_totalDuration * 100.0) : 0;
            if (pct > 100) pct = 100;
            emit progressChanged(m_taskId, pct, 100, 0);
        }
    }
}

void HlsDownloader::onMergeFinished(int exitCode, QProcess::ExitStatus)
{
    if (m_cancelled)
        return;
    if (exitCode == 0) {
        emit progressChanged(m_taskId, 100, 100, 0);
        finish(true, QString());
    } else {
        finish(false, QStringLiteral("ffmpeg 退出码 %1（可能是加密流或网络问题）").arg(exitCode));
    }
}

// ── 无 ffmpeg 时的 AES-128 密钥拉取与纯 C++ 解密下载 ──
void HlsDownloader::fetchKey()
{
    if (m_keyUrl.isEmpty()) {
        finish(false, QStringLiteral("加密 HLS 缺少密钥 URI（#EXT-X-KEY 无 URI）"));
        return;
    }
    QString keyUrl = resolveUrl(m_mediaPlaylistUrl, m_keyUrl);
    QNetworkRequest req(keyUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));
    Log::info(QStringLiteral("HlsDownloader 拉取 AES-128 密钥: %1").arg(keyUrl));
    m_keyReply = m_nam->get(req);
    connect(m_keyReply, &QNetworkReply::finished, this, &HlsDownloader::onKeyFinished);
}

void HlsDownloader::onKeyFinished()
{
    if (!m_keyReply)
        return;
    QNetworkReply* r = m_keyReply;
    m_keyReply = nullptr;
    r->deleteLater();

    if (m_cancelled)
        return;
    if (r->error() != QNetworkReply::NoError) {
        finish(false, QStringLiteral("获取 HLS 密钥失败: %1").arg(r->errorString()));
        return;
    }
    QByteArray key = r->readAll();
    if (key.size() != 16) {
        finish(false, QStringLiteral("HLS 密钥长度异常（%1 字节，应为 16）").arg(key.size()));
        return;
    }
    m_key = key;
    Log::info(QStringLiteral("HlsDownloader 已获取 16 字节密钥，开始纯 C++ AES-128 解密下载"));
    startManualDownload();
}

// 计算分片 IV：显式给出则用之；否则按分片序号（媒体序列号，默认从 0 起）大端写入
QByteArray HlsDownloader::segmentIv(int index) const
{
    if (m_ivPresent && m_iv.size() == 16)
        return m_iv;
    QByteArray iv(16, 0);
    iv[12] = static_cast<char>((index >> 24) & 0xff);
    iv[13] = static_cast<char>((index >> 16) & 0xff);
    iv[14] = static_cast<char>((index >> 8)  & 0xff);
    iv[15] = static_cast<char>( index        & 0xff);
    return iv;
}

void HlsDownloader::startManualDownload()
{
    m_workDir = QDir::temp().filePath(QStringLiteral("idm_hls_") + QString::number(m_taskId));
    QDir().mkpath(m_workDir);
    for (int i = 0; i < m_segments.size(); ++i)
        m_segments[i].file = m_workDir + QStringLiteral("/seg_%1.ts").arg(i, 5, 10, QLatin1Char('0'));
    launchSegments();
}

void HlsDownloader::launchSegments()
{
    while (m_active < m_concurrency) {
        int idx = -1;
        for (int i = 0; i < m_segments.size(); ++i) {
            if (!m_segments[i].done && !m_segments[i].reply) { idx = i; break; }
        }
        if (idx < 0)
            break;  // 没有可启动的分片（全部在途或已完成）

        Segment& s = m_segments[idx];
        QNetworkRequest req(s.url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));

        QNetworkReply* r = m_nam->get(req);
        r->setProperty("segIndex", idx);   // 把分片索引绑到 reply，完成回调 O(1) 定位
        s.reply = r;
        m_active++;

        // 用信号发送者的身份区分是哪个分片完成
        connect(r, &QNetworkReply::finished, this, &HlsDownloader::onSegFinished);
    }
}

void HlsDownloader::onSegFinished()
{
    auto* r = qobject_cast<QNetworkReply*>(sender());
    if (!r)
        return;
    int idx = r->property("segIndex").toInt();
    r->deleteLater();

    if (m_cancelled)
        return;

    if (idx < 0 || idx >= m_segments.size())
        return;
    Segment& s = m_segments[idx];
    if (s.reply == r) s.reply = nullptr;
    m_active--;

    if (r->error() != QNetworkReply::NoError) {
        finish(false, r->errorString());
        return;
    }

    QByteArray data = r->readAll();

    // 无 ffmpeg 的加密流：纯 C++ AES-128-CBC 解密（HLS 分片长度为 16 的整数倍）
    if (m_encrypted && m_key.size() == 16 && (data.size() % 16) == 0) {
        Aes128 aes(m_key);
        QByteArray iv = segmentIv(idx);
        QByteArray dec = aes.decryptCbc(data, iv);
        if (!dec.isEmpty())
            data = dec;
        else
            Log::warn(QStringLiteral("HlsDownloader 分片 %1 解密失败（长度=%2）").arg(idx).arg(data.size()));
    } else if (m_encrypted && m_key.size() == 16) {
        Log::warn(QStringLiteral("HlsDownloader 分片 %1 长度非 16 倍数（%2），跳过解密")
                      .arg(idx).arg(data.size()));
    }

    QFile f(s.file);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
        finish(false, QStringLiteral("写入分片失败：%1").arg(s.file));
        return;
    }
    f.close();

    s.done = true;
    m_doneCount++;
    m_downloadedBytes += data.size();
    emitProgress();

    if (m_doneCount == m_segments.size()) {
        concatSegments();
        return;
    }
    launchSegments();  // 继续下一批
}

void HlsDownloader::concatSegments()
{
    QString out = outputPath(QStringLiteral("ts"));
    QFile outF(out);
    if (!outF.open(QIODevice::WriteOnly)) {
        finish(false, QStringLiteral("无法创建输出文件：%1").arg(out));
        return;
    }
    for (const auto& s : m_segments) {
        QFile in(s.file);
        if (in.open(QIODevice::ReadOnly)) {
            // 流式拷贝：8KB 缓冲逐块写入，避免把整个分片读进内存（大分片也只占固定内存）
            static const qint64 kBuf = 8192;
            QByteArray buf;
            buf.resize(static_cast<int>(kBuf));
            qint64 n;
            while ((n = in.read(buf.data(), kBuf)) > 0)
                outF.write(buf.data(), n);
            in.close();
        }
    }
    outF.close();

    // 清理临时分片
    for (const auto& s : m_segments)
        QFile::remove(s.file);
    QDir().rmdir(m_workDir);

    Log::info(QStringLiteral("HlsDownloader 拼接完成: %1").arg(out));
    finish(true, QString());
}

void HlsDownloader::emitProgress()
{
    int total = m_segments.size();
    int pct = total > 0 ? m_doneCount * 100 / total : 0;
    emit progressChanged(m_taskId, pct, 100, 0);
}

void HlsDownloader::finish(bool success, const QString& err)
{
    if (m_finished)
        return;
    m_finished = true;

    if (success) {
        Log::info(QStringLiteral("HlsDownloader 下载完成: %1").arg(m_url));
        emit stateChanged(m_taskId, 3);  // COMPLETED
    } else {
        Log::error(QStringLiteral("HlsDownloader 失败: %1").arg(err));
        emit stateChanged(m_taskId, 4);  // FAILED
    }
    emit completed(m_taskId, success, err);
}

QString HlsDownloader::outputPath(const QString& ext) const
{
    QString name = m_fileName;
    if (name.isEmpty()) {
        QUrl u(m_url);
        name = QFileInfo(u.path()).fileName();
        if (name.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive))
            name.chop(5);
        if (name.isEmpty())
            name = QStringLiteral("video");
    }
    if (!name.endsWith(QStringLiteral(".") + ext, Qt::CaseInsensitive))
        name = name + QStringLiteral(".") + ext;
    return m_saveDir + QStringLiteral("/") + name;
}
