#ifndef VIDEO_BACKEND_H
#define VIDEO_BACKEND_H

#include <QObject>
#include <functional>

// 视频后端自包含交付：解析 yt-dlp 路径、检测可用性，
// 并在缺失时从 GitHub Releases 自动下载最新独立版（开箱即用）。
class VideoBackend : public QObject {
    Q_OBJECT
public:
    // 已存在的捆绑/缓存路径（应用目录或 AppData，不含 PATH 回退）
    static QString bundledYtDlpPath();

    // 解析最终选用的 yt-dlp 路径
    static QString ytDlpPath();

    // 系统当前是否可用（PATH 或已捆绑）
    static bool isAvailable();

    // 异步确保 yt-dlp 可用：
    //  - 已可用 → 直接回调 true
    //  - 缺失 → 下载到 AppData/yt-dlp/yt-dlp.exe，完成后回调结果
    // context 用于管理网络对象生命周期与信号连接；warnOnError 时在失败时弹窗
    static void ensureAvailable(QObject* context, std::function<void(bool)> cb,
                                bool warnOnError = false);
};

#endif // VIDEO_BACKEND_H
