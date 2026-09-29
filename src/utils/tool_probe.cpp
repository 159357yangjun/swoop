#include "tool_probe.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>

namespace {

struct Entry {
    qint64    size  = -1;
    QDateTime mtime;
    bool      ok    = false;
};

QMutex   g_mtx;
QHash<QString, Entry> g_cache;

} // namespace

namespace ToolProbe {

bool available(const QString& exePath, const QStringList& args)
{
    if (exePath.isEmpty()) return false;

    /* 键里必须带上 args：结果是由「这个文件 + 这几个参数」共同决定的。
       今天每个工具只有一种探法（--version），少带这一项不会出错，
       但下次有人给同一个 exe 加一种探法时，就会拿到别人的缓存结果——
       那种错查起来极慢，所以键写成 path + args。 */
    const QString key = exePath + QLatin1Char('\x1f') + args.join(QLatin1Char('\x1f'));

    const QFileInfo fi(exePath);
    const qint64    size  = fi.size();
    const QDateTime mtime = fi.lastModified();

    {
        const QMutexLocker lk(&g_mtx);
        auto it = g_cache.constFind(key);
        if (it != g_cache.constEnd() && it->size == size && it->mtime == mtime)
            return it->ok;
    }

    bool ok = false;
    if (fi.exists()) {
        QProcess proc;
        proc.start(exePath, args);
        ok = proc.waitForStarted(3000)
             && proc.waitForFinished(5000)
             && proc.exitCode() == 0;
    }

    const QMutexLocker lk(&g_mtx);
    g_cache.insert(key, Entry{ size, mtime, ok });
    return ok;
}

} // namespace ToolProbe
