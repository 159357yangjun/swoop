#include "http_downloader.h"
#include "download_manager.h"

HttpDownloader::HttpDownloader(DownloadManager* mgr, QObject* parent)
    : IDownloader(parent)
    , m_mgr(mgr)
{
    // 引擎信号只连接一次，按 m_taskId 过滤属于自己的任务
    connect(m_mgr, &DownloadManager::taskProgress,
            this, &HttpDownloader::onProgress);
    connect(m_mgr, &DownloadManager::taskCompleted,
            this, &HttpDownloader::onCompleted);
    connect(m_mgr, &DownloadManager::taskStateChanged,
            this, &HttpDownloader::onState);
}

bool HttpDownloader::canHandle(const QString& url) const
{
    return url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) ||
           url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
}

void HttpDownloader::start(const DownloadRequest& req)
{
    int id = m_mgr->addTask(req.url, req.savePath, req.fileName);
    if (id <= 0) {
        emit completed(-1, false, QStringLiteral("添加任务失败"));
        return;
    }
    m_taskId = id;
    m_mgr->startTask(id);
}

void HttpDownloader::pause()  { if (m_taskId > 0) m_mgr->pauseTask(m_taskId); }
void HttpDownloader::resume() { if (m_taskId > 0) m_mgr->resumeTask(m_taskId); }
void HttpDownloader::cancel() { if (m_taskId > 0) m_mgr->cancelTask(m_taskId); }

void HttpDownloader::onProgress(int tid, qint64 downloaded, qint64 total, int speedBps)
{
    if (tid == m_taskId)
        emit progressChanged(m_taskId, downloaded, total, speedBps);
}

void HttpDownloader::onCompleted(int tid, bool success, const QString& error)
{
    if (tid == m_taskId)
        emit completed(m_taskId, success, error);
}

void HttpDownloader::onState(int tid, int state)
{
    if (tid == m_taskId)
        emit stateChanged(m_taskId, state);
}
