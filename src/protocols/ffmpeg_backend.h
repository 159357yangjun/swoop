#pragma once
#include <QObject>
#include <functional>

// ffmpeg 自包含交付：检测系统/捆绑 ffmpeg 是否可用，缺失时自动下载官方
// Windows build 到 AppData，使 HLS TS→MP4 转封装 / 加密流转码开箱即用。
// 与 TorrentBackend（aria2）/ VideoBackend（yt-dlp）保持同一模式。
//
// 注意：ffmpeg 官方 Windows build 是动态链接的（ffmpeg.exe 依赖同目录
// 的 avcodec/avformat/... 等 DLL），因此自动安装时需把整个 bin/ 目录
// （exe + DLL）一起复制到目标目录，不能只复制单个 exe。
class FfmpegBackend {
public:
    // 当前 ffmpeg 是否可用（捆绑目录 / 自定义 / PATH）
    static bool isAvailable();

    // 异步确保 ffmpeg 可用：已可用 → cb(true)；缺失 → 下载并解压到
    // AppData/ffmpeg/ 后回调结果。warnOnError 控制失败时是否弹窗。
    static void ensureAvailable(QObject* context,
                                std::function<void(bool)> cb,
                                bool warnOnError = true);

private:
    static const char* const FFMPEG_ZIP_URL;
};
