#ifndef PROTOCOL_FACTORY_H
#define PROTOCOL_FACTORY_H

#include <QString>

class IDownloader;
class DownloadManager;

// 协议工厂：根据 URL scheme / 域名选择对应的下载器实现
// 当前支持 http/https + 视频网站（YouTube/Bilibili 等 → yt-dlp）；
// ftp/sftp/magnet/.torrent 待 libcurl / libtorrent 接入后扩展
class ProtocolFactory {
public:
    static ProtocolFactory& instance();

    // 按 URL 创建合适的下载器；返回对象所有权归调用方（parent 负责析构）
    // 不支持的协议返回 nullptr
    IDownloader* create(DownloadManager* mgr,
                        const QString& url,
                        QObject* parent = nullptr) const;

    // 该 URL 是否被当前任意协议支持
    bool isSupported(const QString& url) const;

private:
    ProtocolFactory() = default;
};

#endif // PROTOCOL_FACTORY_H
