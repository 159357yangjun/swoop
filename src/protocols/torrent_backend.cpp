#include "torrent_backend.h"
#include "torrent_downloader.h"
#include "app_paths.h"
#include "off_thread.h"   // 自愈的写盘/解压/复制挪出 GUI 线程

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QProcess>
#include <QMessageBox>
#include <memory>
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

        /* 写盘 / tar 解压（waitForFinished 上限 30 秒）/ 复制 / 清理 ——
         * 整段以前都跑在 GUI 线程上（这个 lambda 的接收者是 context），
         * 于是首次安装的自愈把界面冻住数秒到数十秒；同一段时间里
         * MainWindow 的 IPC 服务端也接不到新连接（第二实例就等不到回话）。
         * 现在挪进工作线程，只有 UI 提示、路径登记与回调回到 GUI 线程。 */
        struct Result { int stage = 0; };   // 0=成功 1=写盘 2=解压超时 3=找不到 exe 4=复制失败
        auto res = std::make_shared<Result>();
        runOffThread(context,
            [res, data, tmpZip, destExe]() {
                QFile f(tmpZip);
                if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
                    res->stage = 1;
                    return;
                }
                f.close();

                // 用系统 tar 解压（Windows 10+ 自带 tar.exe），无需额外依赖
                const QString extractDir =
                    QDir::temp().absoluteFilePath(QStringLiteral("aria2-extract"));
                QDir(extractDir).removeRecursively();
                QDir().mkpath(extractDir);
                QProcess tar;
                tar.start(QStringLiteral("tar"),
                          QStringList() << QStringLiteral("-xf") << tmpZip
                                        << QStringLiteral("-C") << extractDir);
                if (!tar.waitForFinished(30000)) {
                    res->stage = 2;
                    return;
                }
                const QString srcExe = findAria2c(extractDir);
                if (srcExe.isEmpty()) {
                    res->stage = 3;
                    return;
                }
                if (QFile::exists(destExe))
                    QFile::remove(destExe);
                if (!QFile::copy(srcExe, destExe)) {
                    res->stage = 4;
                    return;
                }
                QFile::remove(tmpZip);
                QDir(extractDir).removeRecursively();
            },
            [context, cb, warnOnError, res, tmpZip, destExe]() {
            if (res->stage != 0) {
                QString title, detail;
                switch (res->stage) {
                case 1:  title = QStringLiteral("写入失败");
                         detail = QStringLiteral("无法写入：%1").arg(tmpZip); break;
                case 2:  title = QStringLiteral("解压失败");
                         detail = QStringLiteral("aria2 压缩包解压超时"); break;
                case 3:  title = QStringLiteral("解压失败");
                         detail = QStringLiteral("未在压缩包中找到 aria2c.exe"); break;
                default: title = QStringLiteral("安装失败");
                         detail = QStringLiteral("无法复制 aria2c 到：%1").arg(destExe); break;
                }
                if (warnOnError)
                    QMessageBox::warning(qobject_cast<QWidget*>(context), title, detail);
                if (cb) cb(false);
                return;
            }

            TorrentDownloader::setAria2Path(destExe);
            const bool ok = TorrentDownloader::isAvailable();
            if (cb) cb(ok);
            });
    });
}
