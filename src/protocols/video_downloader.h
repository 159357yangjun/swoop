#ifndef VIDEO_DOWNLOADER_H
#define VIDEO_DOWNLOADER_H

#include "idownloader.h"

#include <QProcess>
#include <QRegularExpression>

// 视频流下载器：通过 yt-dlp 命令行工具下载视频网站内容
//
// 工作原理：
//   1. 启动 yt-dlp 子进程，传入 URL + 格式选项 + 输出路径
//   2. 解析 yt-dlp stdout 的进度行（[download] xx.x% of xxxMiB at xxx ETA xx:xx）
//   3. 通过 IDownloader 信号转发进度/完成/状态变更
//
// 依赖：系统 PATH 中需有 yt-dlp（pip install yt-dlp）
// 后续可改为 vcpkg 安装或内置 yt-dlp 二进制
class VideoDownloader : public IDownloader {
    Q_OBJECT
public:
    explicit VideoDownloader(QObject* parent = nullptr);

    bool canHandle(const QString& url) const override;
    void start(const DownloadRequest& req) override;
    void pause() override;
    void resume() override;
    void cancel() override;

    // 设置 yt-dlp 可执行文件路径（默认从 PATH 查找）
    static void setYtDlpPath(const QString& path);
    static QString ytDlpPath();

    // 检测系统是否安装了 yt-dlp
    static bool isAvailable();

    // 已存在的捆绑/缓存路径（应用目录或 AppData，不含 PATH 回退）
    static QString bundledYtDlpPath();

    // 判断 URL 是否为已知视频网站
    static bool isVideoUrl(const QString& url);

    // 设置本下载器关联的任务 ID（信号中携带，接入主窗口任务列表）
    void setTaskId(int id);

private slots:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError error);

private:
    void parseProgress(const QString& line);
    void parseTotalSize(const QString& line);

    QProcess*      m_process = nullptr;
    int            m_taskId   = -1;   // 关联的主窗口任务 ID（信号中携带）
    QString        m_url;
    QString        m_savePath;
    QString        m_fileName;
    qint64         m_totalBytes  = -1;   // 总大小（解析自 yt-dlp 输出）
    qint64         m_downloaded  = 0;    // 已下载字节
    int            m_speedBps    = 0;    // 当前速度
    int            m_etaSec      = 0;    // 剩余秒数
    int            m_lastPercent = 0;    // 上次百分比（用于增量推算已下载字节）
    bool           m_cancelled   = false;

    static QString s_ytDlpPath;

    // yt-dlp 进度行正则：[download]  xx.x% of xxx.xxMiB at xxx.xxMiB/s ETA xx:xx
    static const QRegularExpression s_progressRe;
    // [download] Destination: filename.ext
    static const QRegularExpression s_destRe;
    // [download] 100% of xxx.xxKiB in 00:00
    static const QRegularExpression s_completeRe;
};

#endif // VIDEO_DOWNLOADER_H
