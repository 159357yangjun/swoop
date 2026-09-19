#ifndef APP_PATHS_H
#define APP_PATHS_H

#include <QString>
#include <QSettings>

// 应用数据与配置路径解析（支持“便携模式”）。
//
// FDM 提供 Portable 版：程序放在 U 盘即可在任何 Windows 机器上免安装运行。
// 本项目通过 exe 同目录的标记文件启用便携模式：
//   - idm-next.portable  或  portable  存在 ⇒ 便携模式
// 便携模式下，配置(ini)、历史库(SQLite)、自动下载的辅助二进制
// (aria2c/yt-dlp/ffmpeg) 与插件全部落在 exe 目录下，随程序一起移动。
//
// 另有环境变量 IDM_DATA_DIR：设置后数据目录与该目录下的 idm-next.ini 一并改到此处，
// 用于走查/自测工具与真实用户数据隔离（详见 app_paths.cpp 内的说明）。未设置时无影响。
class AppPaths {
public:
    // 是否便携模式
    static bool isPortable();

    // 用户数据根目录：便携 = exe 目录；否则 = AppData/Local/idm-next
    static QString dataDir();

    // 配置存储：便携 = exe 目录下的 idm-next.ini；否则 = 注册表（默认 QSettings）
    static QSettings settings();
};

#endif // APP_PATHS_H
