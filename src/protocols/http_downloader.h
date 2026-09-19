#ifndef HTTP_DOWNLOADER_H
#define HTTP_DOWNLOADER_H

#include "idownloader.h"

class DownloadManager;

// HTTP/HTTPS 下载适配器：把统一接口 IDownloader 映射到引擎 DownloadManager
// 一个 HttpDownloader 实例对应一个下载任务（start 后持有 taskId）
class HttpDownloader : public IDownloader {
    Q_OBJECT
public:
    explicit HttpDownloader(DownloadManager* mgr, QObject* parent = nullptr);

    bool canHandle(const QString& url) const override;
    void start(const DownloadRequest& req) override;
    void pause() override;
    void resume() override;
    void cancel() override;

    int  taskId() const { return m_taskId; }

private slots:
    void onProgress(int tid, qint64 downloaded, qint64 total, int speedBps);
    void onCompleted(int tid, bool success, const QString& error);
    void onState(int tid, int state);

private:
    DownloadManager* m_mgr;
    int              m_taskId = -1;
};

#endif // HTTP_DOWNLOADER_H
