#include "torrent_backend.h"
#include "torrent_downloader.h"
#include "app_paths.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QProcess>
#include <QMessageBox>
#include <QWidget>

const char* const TorrentBackend::ARIA2_ZIP_URL =
    "https://github.com/aria2/aria2/releases/download/release-1.37.0/"
    "aria2-1.37.0-win-64bit-build1.zip";

QString TorrentBackend::bundledAria2Path()
{
    return TorrentDownloader::bundledAria2Path();
}

QString TorrentBackend::aria2Path()
{
    return TorrentDownloader::aria2Path();
}

bool TorrentBackend::isAvailable()
{
    return TorrentDownloader::isAvailable();
}

// 递归查找目录下的 aria2c.exe（解压后可能位于子目录）
static QString findAria2c(const QString& dir)
{
    QDir d(dir);
    QFileInfoList entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        if (e.isDir()) {
            QString r = findAria2c(e.absoluteFilePath());
            if (!r.isEmpty())
                return r;
        } else if (e.fileName().compare(QStringLiteral("aria2c.exe"), Qt::CaseInsensitive) == 0) {
            return e.absoluteFilePath();
        }
    }
    return QString();
}

void TorrentBackend::ensureAvailable(QObject* context, std::function<void(bool)> cb, bool warnOnError)
{
    if (TorrentDownloader::isAvailable()) {
        if (cb) cb(true);
        return;
    }

    QString local = AppPaths::dataDir();   // 便携模式 = exe 目录，否则 AppData/Local
    if (local.isEmpty()) {
        if (cb) cb(false);
        return;
    }
    QString ariaDir = local + QStringLiteral("/aria2");
    QString destExe = ariaDir + QStringLiteral("/aria2c.exe");
    if (!QDir().mkpath(ariaDir)) {
        if (warnOnError)
            QMessageBox::warning(qobject_cast<QWidget*>(context),
                                 QStringLiteral("失败"),
                                 QStringLiteral("无法创建目录：%1").arg(ariaDir));
        if (cb) cb(false);
        return;
    }

    // 网络对象生命周期随 context；context 销毁时连接自动断开
    auto* nam = new QNetworkAccessManager(context);
    QUrl url(QString::fromUtf8(ARIA2_ZIP_URL));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));

    QString tmpZip = QDir::temp().absoluteFilePath(QStringLiteral("aria2-download.zip"));
    QNetworkReply* reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [=]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("下载失败"),
                                     QStringLiteral("aria2 下载失败：%1").arg(reply->errorString()));
            if (cb) cb(false);
            return;
        }
        QByteArray data = reply->readAll();
        QFile f(tmpZip);
        if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("写入失败"),
                                     QStringLiteral("无法写入：%1").arg(tmpZip));
            if (cb) cb(false);
            return;
        }
        f.close();

        // 用系统 tar 解压（Windows 10+ 自带 tar.exe），无需额外依赖
        QString extractDir = QDir::temp().absoluteFilePath(QStringLiteral("aria2-extract"));
        QDir(extractDir).removeRecursively();
        QDir().mkpath(extractDir);
        QProcess tar;
        tar.start(QStringLiteral("tar"),
                  QStringList() << QStringLiteral("-xf") << tmpZip
                                << QStringLiteral("-C") << extractDir);
        if (!tar.waitForFinished(30000)) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("解压失败"),
                                     QStringLiteral("aria2 压缩包解压超时"));
            if (cb) cb(false);
            return;
        }
        QString srcExe = findAria2c(extractDir);
        if (srcExe.isEmpty()) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("解压失败"),
                                     QStringLiteral("未在压缩包中找到 aria2c.exe"));
            if (cb) cb(false);
            return;
        }
        if (!QFile::copy(srcExe, destExe)) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("安装失败"),
                                     QStringLiteral("无法复制 aria2c 到：%1").arg(destExe));
            if (cb) cb(false);
            return;
        }
        QFile::remove(tmpZip);
        QDir(extractDir).removeRecursively();

        TorrentDownloader::setAria2Path(destExe);
        bool ok = TorrentDownloader::isAvailable();
        if (cb) cb(ok);
    });
}
