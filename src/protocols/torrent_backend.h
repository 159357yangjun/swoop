#ifndef TORRENT_BACKEND_H
#define TORRENT_BACKEND_H

#include <QObject>
#include <functional>

// BT/磁力后端自检与自动下载（aria2 式，镜像 VideoBackend 的开箱即用思路）
class TorrentBackend {
public:
    // 已捆绑/缓存路径
    static QString bundledAria2Path();
    // 解析后的 aria2c 路径（自定义 > 捆绑 > PATH）
    static QString aria2Path();
    // 系统是否可用
    static bool isAvailable();

    // 确保可用：可用则直接回调 true；否则从 GitHub 下载 aria2 二进制（zip）并解压，
    // 解压完成后回调结果。context 销毁时网络请求自动断开。
    static void ensureAvailable(QObject* context, std::function<void(bool)> cb, bool warnOnError);

    // aria2 官方 Windows 64 位发布包（稳定版，用于开箱自动下载）
    static const char* const ARIA2_ZIP_URL;
};

#endif // TORRENT_BACKEND_H
