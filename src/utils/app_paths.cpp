#include "app_paths.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>

// 数据目录/配置存储的「隔离出口」：环境变量 IDM_DATA_DIR 一旦设置，
// 数据目录与 ini 全部改到该目录下，不再碰注册表和 %LOCALAPPDATA%。
//
// 存在的理由：tools/ui_snapshot.exe 这类走查/自测工具每次运行都会建任务、
// 改设置、写历史。之前它们和正式程序共用同一份存储，于是真实用户目录里的
// tasks.json 被截图任务撑到上百条（实测：30 → 60 → 117 条，全是 ui-shots 目录
// 下的假任务），排查「任务数对不上」时非常容易被带偏。
// 工具侧只要在启动时 setenv 到一个 build/ 下的临时目录，就能做到完全无痕；
// 未设置该变量时行为与从前完全一致，正式程序不受影响。
static QString dataDirOverride()
{
    return qEnvironmentVariable("IDM_DATA_DIR");
}

bool AppPaths::isPortable()
{
    if (!dataDirOverride().isEmpty())
        return true;            // 覆盖模式下按「自包含」处理，配置也落在该目录
    QString exeDir = QCoreApplication::applicationDirPath();
    return QFile::exists(exeDir + QStringLiteral("/idm-next.portable"))
        || QFile::exists(exeDir + QStringLiteral("/portable"));
}

QString AppPaths::dataDir()
{
    const QString override = dataDirOverride();
    if (!override.isEmpty())
        return override;
    if (isPortable())
        return QCoreApplication::applicationDirPath();
    QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (local.isEmpty())
        local = QCoreApplication::applicationDirPath();
    return local;
}

QSettings AppPaths::settings()
{
    const QString override = dataDirOverride();
    if (!override.isEmpty()) {
        QDir().mkpath(override);
        return QSettings(override + QStringLiteral("/idm-next.ini"), QSettings::IniFormat);
    }
    if (isPortable()) {
        QString ini = QCoreApplication::applicationDirPath()
                      + QStringLiteral("/idm-next.ini");
        return QSettings(ini, QSettings::IniFormat);
    }
    // 必须显式给出组织名：QCoreApplication 未设置 organizationName 时，
    // 默认构造的 QSettings() 会得到一个「无效」存储（fileName 为空、写入被静默丢弃、
    // 读取恒为默认值），导致主题/下载目录/代理/线程数等全部设置无法持久化。
    // 这里用固定的 org/app，与 QStandardPaths 的数据目录（AppLocalDataLocation）无关，
    // 因此不会移动既有的 tasks.json / 历史数据库位置。
    return QSettings(QStringLiteral("IDMNext"), QStringLiteral("IDM Next"));
}
