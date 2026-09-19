#ifndef IDOWNLOADER_H
#define IDOWNLOADER_H

#include <QObject>
#include <QString>
#include <QVariantMap>

// 下载请求
struct DownloadRequest {
    QString url;          // 下载地址
    QString savePath;     // 保存目录
    QString fileName;     // 文件名（可选，不填则从 URL 推断）
    int     threadCount;  // 线程数（HTTP 分片用）
    bool    isMagnet;     // 是否为磁力链接
    QVariantMap extra;    // 协议特定参数（如 FTP 用户名密码、视频格式等）
};

// 统一下载接口：所有协议（HTTP/FTP/BT/视频流）实现此接口
class IDownloader : public QObject {
    Q_OBJECT
public:
    explicit IDownloader(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~IDownloader() = default;

    // 判断此下载器能否处理该 URL
    virtual bool canHandle(const QString& url) const = 0;

    // 生命周期
    virtual void start(const DownloadRequest& req) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void cancel() = 0;

signals:
    void progressChanged(int taskId, qint64 downloaded, qint64 total, int speedBps);
    void completed(int taskId, bool success, const QString& errorMsg);
    void stateChanged(int taskId, int newState);
};

#endif // IDOWNLOADER_H
