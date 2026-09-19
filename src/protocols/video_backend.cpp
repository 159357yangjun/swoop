#include "video_backend.h"
#include "video_downloader.h"
#include "app_paths.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QMessageBox>
#include <QWidget>

QString VideoBackend::bundledYtDlpPath()
{
    return VideoDownloader::bundledYtDlpPath();
}

QString VideoBackend::ytDlpPath()
{
    return VideoDownloader::ytDlpPath();
}

bool VideoBackend::isAvailable()
{
    return VideoDownloader::isAvailable();
}

void VideoBackend::ensureAvailable(QObject* context, std::function<void(bool)> cb, bool warnOnError)
{
    if (VideoDownloader::isAvailable()) {
        if (cb) cb(true);
        return;
    }

    QString local = AppPaths::dataDir();   // 便携模式 = exe 目录，否则 AppData/Local
    if (local.isEmpty()) {
        if (cb) cb(false);
        return;
    }
    QString dir = local + QStringLiteral("/yt-dlp");
    QString target = dir + QStringLiteral("/yt-dlp.exe");
    if (!QDir().mkpath(dir)) {
        if (warnOnError)
            QMessageBox::warning(qobject_cast<QWidget*>(context),
                                 QStringLiteral("失败"),
                                 QStringLiteral("无法创建目录：%1").arg(dir));
        if (cb) cb(false);
        return;
    }

    // 网络对象生命周期随 context；context 销毁时连接自动断开
    auto* nam = new QNetworkAccessManager(context);
    QUrl url(QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));

    QNetworkReply* reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [=]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("下载失败"),
                                     QStringLiteral("yt-dlp 下载失败：%1").arg(reply->errorString()));
            if (cb) cb(false);
            return;
        }
        QByteArray data = reply->readAll();
        QFile f(target);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("写入失败"),
                                     QStringLiteral("无法写入：%1").arg(target));
            if (cb) cb(false);
            return;
        }
        f.close();
        VideoDownloader::setYtDlpPath(target);
        bool ok = VideoDownloader::isAvailable();
        if (cb) cb(ok);
    });
}
