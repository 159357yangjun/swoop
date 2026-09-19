#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QDebug>

// 轻量日志封装，阶段5会换成 spdlog
// 当前用 qDebug 输出，API 保持与 spdlog 类似以便后续替换
namespace Log {

inline void info(const QString& msg)    { qInfo()  << "[INFO]"  << msg; }
inline void warn(const QString& msg)    { qWarning() << "[WARN]" << msg; }
inline void error(const QString& msg)   { qCritical() << "[ERROR]" << msg; }
inline void debug(const QString& msg)   { qDebug() << "[DEBUG]" << msg; }

} // namespace Log

#endif // LOGGER_H
