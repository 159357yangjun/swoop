#include "ffmpeg_backend.h"
#include "hls_downloader.h"
#include "app_paths.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QProcess>
#include <QMessageBox>
#include <QWidget>

// 官方 Windows essentials build（含 HLS TS→MP4 copy 所需的必要能力）。
// IDM Next 只调用 ffmpeg.exe；若未来上游切换到动态链接构建，同目录 DLL 仍会保留。
const char* const FfmpegBackend::FFMPEG_ZIP_URL =
    "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip";

bool FfmpegBackend::isAvailable()
{
    return HlsDownloader::isFfmpegAvailable();
}

// 旧版本会把 ffplay/ffprobe 一起复制（约 200 MB）。升级后即使 ffmpeg 已可用，
// 也要在 fast path 返回前清理；安装版可能残留在应用目录，按需下载版则在 dataDir。
static void cleanupLegacyFfmpegTools()
{
    const QString appFfmpeg = QCoreApplication::applicationDirPath()
                              + QStringLiteral("/ffmpeg");
    QFile::remove(appFfmpeg + QStringLiteral("/ffplay.exe"));
    QFile::remove(appFfmpeg + QStringLiteral("/ffprobe.exe"));

    const QString local = AppPaths::dataDir();
    if (!local.isEmpty()) {
        const QString dataFfmpeg = local + QStringLiteral("/ffmpeg");
        if (QDir::cleanPath(dataFfmpeg) != QDir::cleanPath(appFfmpeg)) {
            QFile::remove(dataFfmpeg + QStringLiteral("/ffplay.exe"));
            QFile::remove(dataFfmpeg + QStringLiteral("/ffprobe.exe"));
        }
    }
}

// 递归查找目录下的 ffmpeg.exe（解压后位于 bin/ 子目录）
static QString findFfmpeg(const QString& dir)
{
    QDir d(dir);
    QFileInfoList entries = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const auto& e : entries) {
        if (e.isDir()) {
            QString r = findFfmpeg(e.absoluteFilePath());
            if (!r.isEmpty())
                return r;
        } else if (e.fileName().compare(QStringLiteral("ffmpeg.exe"), Qt::CaseInsensitive) == 0) {
            return e.absoluteFilePath();
        }
    }
    return QString();
}

void FfmpegBackend::ensureAvailable(QObject* context, std::function<void(bool)> cb, bool warnOnError)
{
    cleanupLegacyFfmpegTools();
    if (isAvailable()) {
        if (cb) cb(true);
        return;
    }

    QString local = AppPaths::dataDir();   // 便携模式 = exe 目录，否则 AppData/Local
    if (local.isEmpty()) {
        if (cb) cb(false);
        return;
    }
    QString dir = local + QStringLiteral("/ffmpeg");
    QString target = dir + QStringLiteral("/ffmpeg.exe");
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
    QUrl url(QString::fromUtf8(FFMPEG_ZIP_URL));
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("IDM-Next/0.1"));

    QString tmpZip = QDir::temp().absoluteFilePath(QStringLiteral("ffmpeg-download.zip"));
    QNetworkReply* reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, context, [=]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("下载失败"),
                                     QStringLiteral("ffmpeg 下载失败：%1").arg(reply->errorString()));
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
        QString extractDir = QDir::temp().absoluteFilePath(QStringLiteral("ffmpeg-extract"));
        QDir(extractDir).removeRecursively();
        QDir().mkpath(extractDir);
        QProcess tar;
        tar.start(QStringLiteral("tar"),
                  QStringList() << QStringLiteral("-xf") << tmpZip
                                << QStringLiteral("-C") << extractDir);
        if (!tar.waitForFinished(60000)) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("解压失败"),
                                     QStringLiteral("ffmpeg 压缩包解压超时"));
            if (cb) cb(false);
            return;
        }
        QString srcExe = findFfmpeg(extractDir);
        if (srcExe.isEmpty()) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("解压失败"),
                                     QStringLiteral("未在压缩包中找到 ffmpeg.exe"));
            if (cb) cb(false);
            return;
        }

        // essentials 的 bin 目录还带 ffplay/ffprobe，它们各约百 MB 且 IDM Next 不调用。
        // 只复制 ffmpeg.exe + 同目录 DLL：当前静态构建只需 exe，同时兼容未来动态构建。
        QString binDir = QFileInfo(srcExe).absolutePath();
        QDir bd(binDir);
        bool allOk = true;
        bool copiedExe = false;
        for (const auto& fi : bd.entryInfoList(QDir::Files)) {
            const bool isFfmpeg =
                fi.fileName().compare(QStringLiteral("ffmpeg.exe"), Qt::CaseInsensitive) == 0;
            const bool isDll =
                fi.suffix().compare(QStringLiteral("dll"), Qt::CaseInsensitive) == 0;
            if (!isFfmpeg && !isDll)
                continue;

            QString dest = dir + QStringLiteral("/") + fi.fileName();
            if (QFile::exists(dest))
                QFile::remove(dest);
            if (!QFile::copy(fi.absoluteFilePath(), dest)) {
                allOk = false;
                break;
            }
            copiedExe = copiedExe || isFfmpeg;
        }

        cleanupLegacyFfmpegTools();

        if (!allOk || !copiedExe) {
            if (warnOnError)
                QMessageBox::warning(qobject_cast<QWidget*>(context),
                                     QStringLiteral("安装失败"),
                                     QStringLiteral("无法复制 ffmpeg 到：%1").arg(dir));
            if (cb) cb(false);
            return;
        }
        QFile::remove(tmpZip);
        QDir(extractDir).removeRecursively();

        HlsDownloader::setFfmpegPath(target);
        bool ok = isAvailable();
        if (cb) cb(ok);
    });
}