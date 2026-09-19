#include "protocol_factory.h"
#include "http_downloader.h"
#include "video_downloader.h"
#include "hls_downloader.h"
#include "torrent_downloader.h"

#include <QString>

ProtocolFactory& ProtocolFactory::instance()
{
    static ProtocolFactory f;
    return f;
}

IDownloader* ProtocolFactory::create(DownloadManager* mgr,
                                     const QString& url,
                                     QObject* parent) const
{
    // HLS（M3U8）播放列表 → 原生 HlsDownloader（自建解析 + ffmpeg 合并）
    if (HlsDownloader::isHlsUrl(url))
        return new HlsDownloader(parent);

    // BT/磁力/FTP（magnet / *.torrent / ftp://）→ TorrentDownloader（aria2 式 RPC 集成）
    if (TorrentDownloader::isAria2Url(url))
        return new TorrentDownloader(parent);

    // 视频网站 URL（YouTube/Bilibili 等）→ VideoDownloader
    if (VideoDownloader::isVideoUrl(url))
        return new VideoDownloader(parent);

    if (url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
        url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
        return new HttpDownloader(mgr, parent);

    // 预留：ftp/ftps/sftp → FtpDownloader（libcurl）
    return nullptr;
}

bool ProtocolFactory::isSupported(const QString& url) const
{
    return HlsDownloader::isHlsUrl(url) ||
           TorrentDownloader::isAria2Url(url) ||
           VideoDownloader::isVideoUrl(url) ||
           url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
           url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
    // 后续扩展：
    // || url.startsWith("ftp://") || url.startsWith("ftps://") || url.startsWith("sftp://")
}
