#ifndef HLS_DOWNLOADER_H
#define HLS_DOWNLOADER_H

#include "idownloader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QVariantMap>

// HLS（M3U8）流下载器：不依赖 yt-dlp，自建解析 + ffmpeg 合并
//
// 工作原理：
//   1. 拉取 m3u8 播放列表（主播放列表含 #EXT-X-STREAM-INF 变体 / 媒体播放列表含分片）
//   2. 解析出所有分片 URL（处理相对/绝对路径、变体选择），并检测 AES-128 加密（#EXT-X-KEY）
//   3. 合并阶段三选一：
//      a) 若系统存在 ffmpeg —— 直接 `ffmpeg -i <playlist> -c copy out.mp4`，
//         自动处理 AES-128 解密、音视频封装、码率拷贝，质量最佳（含 MP4 转封装）；
//      b) 若无 ffmpeg 但加密 —— 纯 C++（aes128.h）拉取密钥并逐分片 AES-128-CBC 解密，
//         再裸拼接为 .ts（VLC/mpv/PotPlayer 可播），彻底摆脱对 ffmpeg 的依赖；
//      c) 若无 ffmpeg 且未加密 —— 手动并发拉取每个 TS 分片，再裸拼接为 .ts
//   4. 通过 IDownloader 信号转发进度 / 完成 / 状态变更
class HlsDownloader : public IDownloader {
    Q_OBJECT
public:
    explicit HlsDownloader(QObject* parent = nullptr);

    bool canHandle(const QString& url) const override;
    void start(const DownloadRequest& req) override;
    void pause() override;
    void resume() override;
    void cancel() override;

    // 设置本下载器关联的任务 ID（信号中携带，接入主窗口任务列表）
    void setTaskId(int id);

    // 判断 URL 是否为 HLS 播放列表（.m3u8）
    static bool isHlsUrl(const QString& url);

    // ffmpeg 定位（留空则自动查找 PATH / 捆绑目录）
    static void setFfmpegPath(const QString& path);
    static QString ffmpegPath();
    static bool isFfmpegAvailable();

private slots:
    void onSegFinished();             // 手动模式下单个分片下载完成
    void onMergeReadyRead();          // ffmpeg 进度行
    void onMergeFinished(int exitCode, QProcess::ExitStatus status);
    void onKeyFinished();             // AES-128 密钥拉取完成（无 ffmpeg 时）

private:
    struct Segment {
        QString      url;     // 分片绝对 URL
        QString      file;    // 本地临时文件
        bool         done = false;
        QNetworkReply* reply = nullptr;
    };

    void fetchPlaylist(const QString& url, bool isMedia);
    void parseMasterPlaylist(const QString& text, const QString& base);
    void parseMediaPlaylist(const QString& text, const QString& base);
    QString resolveUrl(const QString& base, const QString& ref) const;

    void beginDownload();             // 选择后端（ffmpeg / 纯C++解密 / 手动）
    void startFfmpegDownload();       // ffmpeg 直接拉流合并（MP4 转封装）
    void fetchKey();                  // 无 ffmpeg：先拉取 AES-128 密钥
    void startManualDownload();       // 无 ffmpeg：手动分段下载（含纯C++解密）
    void launchSegments();            // 手动模式：按并发度拉取下一批分片
    void concatSegments();            // 手动模式：拼接为 .ts
    void emitProgress();              // 以“已完成分片占比”估算进度
    void finish(bool success, const QString& err);
    QString outputPath(const QString& ext) const;
    QByteArray segmentIv(int index) const;  // 计算分片 IV（无 IV 属性时用序号）

    QNetworkAccessManager* m_nam = nullptr;
    QList<Segment>         m_segments;
    int                    m_taskId   = -1;
    QString                m_url;
    QString                m_saveDir;
    QString                m_fileName;
    QVariantMap            m_extra;        // 协议特定参数（如 hlsVariant）
    QString                m_forcedVariant; // 指定变体带宽（空=自动选最佳）
    QString                m_workDir;       // 临时分片目录
    QString                m_mediaPlaylistUrl; // 选定变体后的媒体播放列表 URL
    double                 m_totalDuration = 0.0; // 秒（ffmpeg 进度估算用）
    qint64                 m_downloadedBytes = 0;
    int                    m_active = 0;
    int                    m_concurrency = 4;     // 手动模式并发分片数
    int                    m_doneCount = 0;       // 已完成分片计数（避免每次全量扫描）
    bool                   m_cancelled = false;
    bool                   m_paused    = false;
    bool                   m_started   = false;
    bool                   m_finished  = false;
    bool                   m_encrypted = false;    // 媒体播放列表含 AES-128
    QProcess*              m_merge = nullptr;

    // ── #52：AES-128 纯 C++ 解密支持 ──
    QString         m_keyUrl;          // #EXT-X-KEY 中的 URI（相对/绝对）
    QByteArray      m_key;             // 16 字节密钥（拉取后填充）
    QByteArray      m_iv;              // 16 字节 IV（若 KEY 标签显式给出）
    bool            m_ivPresent = false;
    QNetworkReply*  m_keyReply = nullptr;

    static QString s_ffmpegPath;
};

#endif // HLS_DOWNLOADER_H
